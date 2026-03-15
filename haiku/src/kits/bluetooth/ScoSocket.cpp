/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * ScoSocket — SCO/eSCO connection management via HCI commands.
 * Uses BMessage IPC to BluetoothServer for HCI command/event handling.
 */

#include <bluetooth/ScoSocket.h>

#include <stdio.h>
#include <string.h>

#include <Application.h>
#include <Messenger.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>

#include <bluetoothserver_p.h>
#include <CommandManager.h>


#define TRACE_SCO(fmt, ...) \
	fprintf(stderr, "SCO: " fmt, ##__VA_ARGS__)


/* Standard eSCO parameters for CVSD voice (HFP) */
#define SCO_TX_BANDWIDTH		8000	/* 8000 bytes/sec = 8kHz mono 8-bit */
#define SCO_RX_BANDWIDTH		8000
#define SCO_MAX_LATENCY			0x000D	/* 13ms */
#define SCO_VOICE_SETTING		0x0060	/* CVSD, linear coding, 16-bit */
#define SCO_RETRANSMIT_EFFORT	0x02	/* optimized for link quality */
#define SCO_PKT_TYPE			0x003F	/* all eSCO + SCO types */


using Bluetooth::ScoSocket;


ScoSocket::ScoSocket()
	:
	fScoHandle(0),
	fRxPktLen(0),
	fTxPktLen(0),
	fLinkType(0),
	fConnected(false)
{
	memset(&fRemoteAddr, 0, sizeof(fRemoteAddr));
}


ScoSocket::~ScoSocket()
{
	Disconnect();
}


status_t
ScoSocket::Connect(const bdaddr_t& address, uint16 aclHandle)
{
	if (fConnected)
		return B_BUSY;

	memcpy(&fRemoteAddr, &address, sizeof(bdaddr_t));

	hci_id hid;
	if (!_GetHciId(hid))
		return B_ERROR;

	/* Build HCI Setup Synchronous Connection command */
	BluetoothCommand<typed_command(hci_cp_setup_sync_conn)>
		setupCmd(OGF_LINK_CONTROL, OCF_SETUP_SYNC_CONN);

	setupCmd->handle = aclHandle;
	setupCmd->tx_bandwidth = SCO_TX_BANDWIDTH;
	setupCmd->rx_bandwidth = SCO_RX_BANDWIDTH;
	setupCmd->max_latency = SCO_MAX_LATENCY;
	setupCmd->voice_setting = SCO_VOICE_SETTING;
	setupCmd->retransmit_effort = SCO_RETRANSMIT_EFFORT;
	setupCmd->pkt_type = SCO_PKT_TYPE;

	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		TRACE_SCO("Cannot reach BluetoothServer\n");
		return B_ERROR;
	}

	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	BMessage reply;

	request.AddInt32("hci_id", hid);
	request.AddData("raw command", B_ANY_TYPE,
		setupCmd.Data(), setupCmd.Size());

	request.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
	request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_LINK_CONTROL, OCF_SETUP_SYNC_CONN));
	request.AddInt16("eventExpected",
		HCI_EVENT_SYNCHRONOUS_CONNECTION_COMPLETED);

	TRACE_SCO("Sending Setup Synchronous Connection "
		"(ACL handle=0x%04X)...\n", aclHandle);

	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 15000000LL);

	if (result == B_TIMED_OUT) {
		TRACE_SCO("SCO setup timed out\n");
		return B_TIMED_OUT;
	}
	if (result != B_OK) {
		TRACE_SCO("SendMessage failed: %s\n", strerror(result));
		return result;
	}

	int8 status = BT_ERROR;
	reply.FindInt8("status", &status);

	if (status != BT_OK) {
		TRACE_SCO("SCO setup failed (HCI status 0x%02X)\n",
			(unsigned)(uint8)status);
		return B_ERROR;
	}

	int16 handle = -1;
	reply.FindInt16("handle", &handle);
	fScoHandle = (uint16)handle;

	int16 rxLen = 0, txLen = 0;
	reply.FindInt16("rx_pkt_len", &rxLen);
	reply.FindInt16("tx_pkt_len", &txLen);
	fRxPktLen = (uint16)rxLen;
	fTxPktLen = (uint16)txLen;

	int8 linkType = 0;
	reply.FindInt8("link_type", &linkType);
	fLinkType = (uint8)linkType;

	fConnected = true;

	TRACE_SCO("SCO connected: handle=0x%04X link_type=%u "
		"rx_pkt_len=%u tx_pkt_len=%u\n",
		fScoHandle, fLinkType, fRxPktLen, fTxPktLen);

	return B_OK;
}


status_t
ScoSocket::Accept(const bdaddr_t& address)
{
	if (fConnected)
		return B_BUSY;

	memcpy(&fRemoteAddr, &address, sizeof(bdaddr_t));

	hci_id hid;
	if (!_GetHciId(hid))
		return B_ERROR;

	/* Build HCI Accept Synchronous Connection command */
	BluetoothCommand<typed_command(hci_cp_accept_sync_conn)>
		acceptCmd(OGF_LINK_CONTROL, OCF_ACCEPT_SYNC_CONN);

	bdaddrUtils::Copy(acceptCmd->bdaddr, address);
	acceptCmd->tx_bandwidth = SCO_TX_BANDWIDTH;
	acceptCmd->rx_bandwidth = SCO_RX_BANDWIDTH;
	acceptCmd->max_latency = SCO_MAX_LATENCY;
	acceptCmd->content_format = SCO_VOICE_SETTING;
	acceptCmd->retransmit_effort = SCO_RETRANSMIT_EFFORT;
	acceptCmd->pkt_type = SCO_PKT_TYPE;

	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		TRACE_SCO("Cannot reach BluetoothServer\n");
		return B_ERROR;
	}

	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	BMessage reply;

	request.AddInt32("hci_id", hid);
	request.AddData("raw command", B_ANY_TYPE,
		acceptCmd.Data(), acceptCmd.Size());

	request.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
	request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_LINK_CONTROL, OCF_ACCEPT_SYNC_CONN));
	request.AddInt16("eventExpected",
		HCI_EVENT_SYNCHRONOUS_CONNECTION_COMPLETED);

	TRACE_SCO("Sending Accept Synchronous Connection...\n");

	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 15000000LL);

	if (result == B_TIMED_OUT) {
		TRACE_SCO("SCO accept timed out\n");
		return B_TIMED_OUT;
	}
	if (result != B_OK) {
		TRACE_SCO("SendMessage failed: %s\n", strerror(result));
		return result;
	}

	int8 status = BT_ERROR;
	reply.FindInt8("status", &status);

	if (status != BT_OK) {
		TRACE_SCO("SCO accept failed (HCI status 0x%02X)\n",
			(unsigned)(uint8)status);
		return B_ERROR;
	}

	int16 handle = -1;
	reply.FindInt16("handle", &handle);
	fScoHandle = (uint16)handle;

	int16 rxLen = 0, txLen = 0;
	reply.FindInt16("rx_pkt_len", &rxLen);
	reply.FindInt16("tx_pkt_len", &txLen);
	fRxPktLen = (uint16)rxLen;
	fTxPktLen = (uint16)txLen;

	int8 linkType = 0;
	reply.FindInt8("link_type", &linkType);
	fLinkType = (uint8)linkType;

	fConnected = true;

	TRACE_SCO("SCO accepted: handle=0x%04X link_type=%u "
		"rx_pkt_len=%u tx_pkt_len=%u\n",
		fScoHandle, fLinkType, fRxPktLen, fTxPktLen);

	return B_OK;
}


void
ScoSocket::Disconnect()
{
	if (!fConnected)
		return;

	hci_id hid;
	if (_GetHciId(hid)) {
		/* Send HCI Disconnect for the SCO handle */
		BluetoothCommand<typed_command(hci_disconnect)>
			discCmd(OGF_LINK_CONTROL, OCF_DISCONNECT);
		discCmd->handle = fScoHandle;
		discCmd->reason = 0x13;  /* Remote User Terminated Connection */

		BMessenger messenger(BLUETOOTH_SIGNATURE);
		if (messenger.IsValid()) {
			BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
			BMessage reply;

			request.AddInt32("hci_id", hid);
			request.AddData("raw command", B_ANY_TYPE,
				discCmd.Data(), discCmd.Size());
			request.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
			request.AddInt16("opcodeExpected",
				PACK_OPCODE(OGF_LINK_CONTROL, OCF_DISCONNECT));
			request.AddInt16("eventExpected",
				HCI_EVENT_DISCONNECTION_COMPLETE);

			messenger.SendMessage(&request, &reply,
				B_INFINITE_TIMEOUT, 5000000LL);
		}
	}

	fConnected = false;
	fScoHandle = 0;
	TRACE_SCO("SCO disconnected\n");
}


bool
ScoSocket::IsConnected() const
{
	return fConnected;
}


bool
ScoSocket::_GetHciId(hci_id& outHid)
{
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		TRACE_SCO("Cannot reach BluetoothServer\n");
		return false;
	}

	BMessage acquire(BT_MSG_ACQUIRE_LOCAL_DEVICE);
	BMessage acquireReply;
	if (messenger.SendMessage(&acquire, &acquireReply,
			B_INFINITE_TIMEOUT, 5000000LL) != B_OK) {
		TRACE_SCO("Failed to query local device\n");
		return false;
	}

	if (acquireReply.FindInt32("hci_id", &outHid) != B_OK
		|| outHid < 0) {
		TRACE_SCO("No local Bluetooth device found\n");
		return false;
	}

	return true;
}
