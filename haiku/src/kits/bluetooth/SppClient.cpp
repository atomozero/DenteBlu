/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SppClient — Serial Port Profile client implementation.
 */

#include <bluetooth/SppClient.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <Application.h>
#include <Messenger.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/L2CAP/btL2CAP.h>

#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetoothserver_p.h>
#include <CommandManager.h>
#include <l2cap.h>

#include "RfcommSession.h"


#define TRACE_SPP(fmt, ...) \
	fprintf(stderr, "SPP: " fmt, ##__VA_ARGS__)


/* =========================================================================
 * SDP Data Element parser (simplified, from bt_sdp_query.cpp)
 * ========================================================================= */

namespace {

struct DataElement {
	uint8		type;
	uint32		dataLen;
	const uint8* data;
	uint32		totalLen;
};


static inline uint16
ReadBE16(const uint8* p)
{
	return (uint16)(p[0] << 8 | p[1]);
}


static inline uint32
ReadBE32(const uint8* p)
{
	return (uint32)(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
}


static inline void
WriteBE16(uint8* p, uint16 v)
{
	p[0] = (uint8)(v >> 8);
	p[1] = (uint8)(v);
}


static inline void
WriteBE32(uint8* p, uint32 v)
{
	p[0] = (uint8)(v >> 24);
	p[1] = (uint8)(v >> 16);
	p[2] = (uint8)(v >> 8);
	p[3] = (uint8)(v);
}


static bool
ParseDataElement(const uint8* buf, uint32 bufLen, DataElement& out)
{
	if (bufLen < 1)
		return false;

	uint8 header = buf[0];
	out.type = header >> 3;
	uint8 sizeDesc = header & 0x07;

	uint32 headerLen = 1;
	uint32 dataLen = 0;

	if (out.type == SDP_DE_NIL) {
		dataLen = 0;
	} else if (sizeDesc <= SDP_DE_SIZE_16) {
		static const uint32 kFixedSizes[] = { 1, 2, 4, 8, 16 };
		dataLen = kFixedSizes[sizeDesc];
	} else if (sizeDesc == SDP_DE_SIZE_NEXT1) {
		if (bufLen < 2)
			return false;
		headerLen = 2;
		dataLen = buf[1];
	} else if (sizeDesc == SDP_DE_SIZE_NEXT2) {
		if (bufLen < 3)
			return false;
		headerLen = 3;
		dataLen = ReadBE16(buf + 1);
	} else if (sizeDesc == SDP_DE_SIZE_NEXT4) {
		if (bufLen < 5)
			return false;
		headerLen = 5;
		dataLen = ReadBE32(buf + 1);
	}

	if (headerLen + dataLen > bufLen)
		return false;

	out.dataLen = dataLen;
	out.data = buf + headerLen;
	out.totalLen = headerLen + dataLen;
	return true;
}


/* Extract RFCOMM channel from a ProtocolDescriptorList.
 * Looks for: Sequence { Sequence{UUID(L2CAP)}, Sequence{UUID(RFCOMM), uint8(ch)} }
 */
static bool
ExtractRfcommChannel(const uint8* data, uint32 len, uint8& outChannel)
{
	uint32 off = 0;
	while (off < len) {
		DataElement stack;
		if (!ParseDataElement(data + off, len - off, stack))
			break;

		if (stack.type == SDP_DE_SEQUENCE || stack.type == SDP_DE_ALTERNATIVE) {
			const uint8* inner = stack.data;
			uint32 innerLen = stack.dataLen;

			/* First element: protocol UUID */
			DataElement proto;
			if (ParseDataElement(inner, innerLen, proto)
				&& proto.type == SDP_DE_UUID && proto.dataLen == 2) {
				uint16 protoUuid = ReadBE16(proto.data);
				if (protoUuid == SDP_UUID16_RFCOMM) {
					/* Second element: channel number */
					uint32 ioff = proto.totalLen;
					if (ioff < innerLen) {
						DataElement param;
						if (ParseDataElement(inner + ioff, innerLen - ioff,
								param)
							&& param.type == SDP_DE_UINT
							&& param.dataLen == 1) {
							outChannel = param.data[0];
							return true;
						}
					}
				}
			}
		}
		off += stack.totalLen;
	}
	return false;
}


} /* anonymous namespace */


/* =========================================================================
 * SppClient implementation
 * ========================================================================= */

namespace Bluetooth {


SppClient::SppClient()
	:
	fSession(NULL),
	fServerSocket(-1),
	fDlci(0),
	fCallback(NULL),
	fCallbackCookie(NULL)
{
}


SppClient::~SppClient()
{
	Disconnect();
}


status_t
SppClient::Connect(const bdaddr_t& address, uint8 rfcommChannel)
{
	if (fSession != NULL)
		return B_BUSY;

	/* Ensure ACL connection to the remote device */
	TRACE_SPP("Establishing ACL connection...\n");
	if (!_EnsureAclConnection(address)) {
		TRACE_SPP("Failed to establish ACL connection\n");
		return B_ERROR;
	}

	/* Query SDP if no channel specified */
	if (rfcommChannel == 0) {
		TRACE_SPP("Querying SDP for SPP channel...\n");
		if (!_QuerySdpForChannel(address, rfcommChannel)) {
			TRACE_SPP("SDP query failed — no SPP service found\n");
			return B_NAME_NOT_FOUND;
		}
		TRACE_SPP("SDP found RFCOMM channel %u\n", rfcommChannel);
	}

	/* Create RFCOMM session */
	fSession = new(std::nothrow) RfcommSession();
	if (fSession == NULL)
		return B_NO_MEMORY;

	/* Connect L2CAP + open multiplexer */
	status_t result = fSession->Connect(address);
	if (result != B_OK) {
		TRACE_SPP("RFCOMM session connect failed: %s\n", strerror(result));
		delete fSession;
		fSession = NULL;
		return result;
	}

	/* Open RFCOMM channel */
	result = fSession->OpenChannel(rfcommChannel);
	if (result != B_OK) {
		TRACE_SPP("RFCOMM channel open failed: %s\n", strerror(result));
		fSession->Disconnect();
		delete fSession;
		fSession = NULL;
		return result;
	}

	fDlci = RFCOMM_DLCI(1, rfcommChannel);
	TRACE_SPP("Connected on RFCOMM channel %u (DLCI %u)\n",
		rfcommChannel, fDlci);
	return B_OK;
}


status_t
SppClient::Listen(bigtime_t timeout)
{
	if (fSession != NULL)
		return B_BUSY;

	/* Open L2CAP server socket on PSM 3 (RFCOMM) */
	fServerSocket = socket(PF_BLUETOOTH, SOCK_STREAM,
		BLUETOOTH_PROTO_L2CAP);
	if (fServerSocket < 0) {
		TRACE_SPP("Listen: socket() failed: %s\n", strerror(errno));
		return B_ERROR;
	}

	int reuse = 1;
	setsockopt(fServerSocket, SOL_SOCKET, SO_REUSEADDR, &reuse,
		sizeof(reuse));

	struct sockaddr_l2cap addr;
	memset(&addr, 0, sizeof(addr));
	addr.l2cap_len = sizeof(addr);
	addr.l2cap_family = AF_BLUETOOTH;
	addr.l2cap_bdaddr = BDADDR_ANY;
	addr.l2cap_psm = B_HOST_TO_LENDIAN_INT16(L2CAP_PSM_RFCOMM);

	if (bind(fServerSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		TRACE_SPP("Listen: bind(PSM 3) failed: %s\n", strerror(errno));
		close(fServerSocket);
		fServerSocket = -1;
		return B_ERROR;
	}

	if (listen(fServerSocket, 1) < 0) {
		TRACE_SPP("Listen: listen() failed: %s\n", strerror(errno));
		close(fServerSocket);
		fServerSocket = -1;
		return B_ERROR;
	}

	TRACE_SPP("Listening on L2CAP PSM 3 (RFCOMM)...\n");

	/* Set accept timeout */
	struct timeval tv;
	tv.tv_sec = timeout / 1000000;
	tv.tv_usec = timeout % 1000000;
	setsockopt(fServerSocket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	/* Accept one connection */
	struct sockaddr_l2cap remoteAddr;
	uint remoteLen = sizeof(remoteAddr);
	int clientSocket = accept(fServerSocket, (struct sockaddr*)&remoteAddr,
		&remoteLen);
	close(fServerSocket);
	fServerSocket = -1;

	if (clientSocket < 0) {
		TRACE_SPP("Listen: accept() failed: %s\n", strerror(errno));
		return B_TIMED_OUT;
	}

	TRACE_SPP("Accepted L2CAP connection from %s\n",
		bdaddrUtils::ToString(remoteAddr.l2cap_bdaddr).String());

	/* Hand socket to RFCOMM in server mode */
	fSession = new(std::nothrow) RfcommSession();
	if (fSession == NULL) {
		close(clientSocket);
		return B_NO_MEMORY;
	}

	status_t result = fSession->AcceptFrom(clientSocket);
	if (result != B_OK) {
		TRACE_SPP("Listen: RFCOMM AcceptFrom failed: %s\n",
			strerror(result));
		delete fSession;
		fSession = NULL;
		return result;
	}

	/* Wait for remote to open an RFCOMM channel */
	TRACE_SPP("Waiting for RFCOMM channel...\n");
	uint8 dlci = fSession->WaitForChannel(30000000LL);
	if (dlci == 0) {
		TRACE_SPP("Listen: no RFCOMM channel opened by remote\n");
		delete fSession;
		fSession = NULL;
		return B_TIMED_OUT;
	}

	fDlci = dlci;
	TRACE_SPP("Listen: RFCOMM channel open on DLCI %u\n", dlci);
	return B_OK;
}


void
SppClient::Disconnect()
{
	if (fSession != NULL) {
		if (fDlci != 0)
			fSession->CloseChannel(fDlci);
		fSession->Disconnect();
		delete fSession;
		fSession = NULL;
	}
	if (fServerSocket >= 0) {
		close(fServerSocket);
		fServerSocket = -1;
	}
	fDlci = 0;
}


bool
SppClient::IsConnected() const
{
	return fSession != NULL && fSession->IsConnected() && fDlci != 0;
}


ssize_t
SppClient::Send(const void* data, size_t length)
{
	if (fSession == NULL || fDlci == 0)
		return B_NOT_ALLOWED;

	return fSession->Send(fDlci, data, length);
}


ssize_t
SppClient::Receive(void* buffer, size_t maxLength, bigtime_t timeout)
{
	if (fSession == NULL || fDlci == 0)
		return B_NOT_ALLOWED;

	return fSession->Receive(fDlci, buffer, maxLength, timeout);
}


void
SppClient::SetDataCallback(spp_data_callback callback, void* cookie)
{
	fCallback = callback;
	fCallbackCookie = cookie;
}


uint16
SppClient::Mtu() const
{
	if (fSession != NULL)
		return fSession->Mtu();
	return RFCOMM_DEFAULT_MTU;
}


/* =========================================================================
 * ACL connection via BluetoothServer
 * ========================================================================= */

bool
SppClient::_EnsureAclConnection(const bdaddr_t& remote)
{
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		TRACE_SPP("Cannot reach BluetoothServer\n");
		return false;
	}

	/* Acquire local HCI device */
	BMessage acquire(BT_MSG_ACQUIRE_LOCAL_DEVICE);
	BMessage acquireReply;
	if (messenger.SendMessage(&acquire, &acquireReply, B_INFINITE_TIMEOUT,
			5000000LL) != B_OK) {
		TRACE_SPP("Failed to query local device\n");
		return false;
	}

	hci_id hid;
	if (acquireReply.FindInt32("hci_id", &hid) != B_OK || hid < 0) {
		TRACE_SPP("No local Bluetooth device found\n");
		return false;
	}

	/* Build HCI Create Connection command */
	BluetoothCommand<typed_command(hci_cp_create_conn)>
		createConn(OGF_LINK_CONTROL, OCF_CREATE_CONN);

	bdaddrUtils::Copy(createConn->bdaddr, remote);
	createConn->pkt_type = 0xCC18;
	createConn->pscan_rep_mode = 0x01;
	createConn->pscan_mode = 0x00;
	createConn->clock_offset = 0x0000;
	createConn->role_switch = 0x01;

	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	BMessage reply;

	request.AddInt32("hci_id", hid);
	request.AddData("raw command", B_ANY_TYPE,
		createConn.Data(), createConn.Size());

	request.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
	request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_LINK_CONTROL, OCF_CREATE_CONN));

	request.AddInt16("eventExpected", HCI_EVENT_LINK_KEY_REQ);
	request.AddInt16("eventExpected", HCI_EVENT_LINK_KEY_NOTIFY);
	request.AddInt16("eventExpected", HCI_EVENT_IO_CAPABILITY_REQUEST);
	request.AddInt16("eventExpected", HCI_EVENT_IO_CAPABILITY_RESPONSE);
	request.AddInt16("eventExpected", HCI_EVENT_USER_CONFIRMATION_REQUEST);
	request.AddInt16("eventExpected", HCI_EVENT_SIMPLE_PAIRING_COMPLETE);
	request.AddInt16("eventExpected", HCI_EVENT_CONN_COMPLETE);

	TRACE_SPP("Sending HCI Create Connection (timeout 30s)...\n");

	int8 btStatus = BT_ERROR;
	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 30000000LL);

	if (result == B_TIMED_OUT) {
		TRACE_SPP("ACL connection timed out\n");
		return false;
	}
	if (result != B_OK) {
		TRACE_SPP("SendMessage failed: %s\n", strerror(result));
		return false;
	}

	reply.FindInt8("status", &btStatus);
	if (btStatus == 0x0B) {
		/* 0x0B = ACL Connection Already Exists — phone auto-connected.
		 * The link is already authenticated/encrypted from the original
		 * connection, so skip auth/encrypt and proceed directly. */
		TRACE_SPP("ACL already exists (phone auto-connected), "
			"skipping auth/encrypt\n");
		return true;
	} else if (btStatus != BT_OK) {
		TRACE_SPP("ACL connection failed (HCI status 0x%02X)\n",
			(unsigned)(uint8)btStatus);
		return false;
	}

	int16 handle = -1;
	reply.FindInt16("handle", &handle);
	TRACE_SPP("ACL connection established (handle=0x%04X)\n",
		(unsigned)(uint16)handle);

	if (handle < 0) {
		TRACE_SPP("No connection handle in reply, skipping auth/encrypt\n");
		return true;
	}

	/* Step 2: Authenticate the link (uses stored link key) */
	{
		TRACE_SPP("Requesting authentication...\n");
		BluetoothCommand<typed_command(hci_cp_auth_requested)>
			authCmd(OGF_LINK_CONTROL, OCF_AUTH_REQUESTED);
		authCmd->handle = (uint16)handle;

		BMessage authReq(BT_MSG_HANDLE_SIMPLE_REQUEST);
		BMessage authReply;
		authReq.AddInt32("hci_id", hid);
		authReq.AddData("raw command", B_ANY_TYPE,
			authCmd.Data(), authCmd.Size());
		authReq.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
		authReq.AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_LINK_CONTROL, OCF_AUTH_REQUESTED));
		authReq.AddInt16("eventExpected", HCI_EVENT_LINK_KEY_REQ);
		authReq.AddInt16("eventExpected", HCI_EVENT_AUTH_COMPLETE);

		result = messenger.SendMessage(&authReq, &authReply,
			B_INFINITE_TIMEOUT, 10000000LL);
		int8 authStatus = BT_ERROR;
		if (result == B_OK)
			authReply.FindInt8("status", &authStatus);
		if (authStatus != BT_OK) {
			TRACE_SPP("Authentication failed (status 0x%02X)\n",
				(unsigned)(uint8)authStatus);
			return false;
		}
		TRACE_SPP("Authentication complete\n");
	}

	/* Step 3: Enable encryption on the link */
	{
		TRACE_SPP("Enabling encryption...\n");
		BluetoothCommand<typed_command(hci_cp_set_conn_encrypt)>
			encCmd(OGF_LINK_CONTROL, OCF_SET_CONN_ENCRYPT);
		encCmd->handle = (uint16)handle;
		encCmd->encrypt = 1;

		BMessage encReq(BT_MSG_HANDLE_SIMPLE_REQUEST);
		BMessage encReply;
		encReq.AddInt32("hci_id", hid);
		encReq.AddData("raw command", B_ANY_TYPE,
			encCmd.Data(), encCmd.Size());
		encReq.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
		encReq.AddInt16("opcodeExpected",
			PACK_OPCODE(OGF_LINK_CONTROL, OCF_SET_CONN_ENCRYPT));
		encReq.AddInt16("eventExpected", HCI_EVENT_ENCRYPT_CHANGE);

		result = messenger.SendMessage(&encReq, &encReply,
			B_INFINITE_TIMEOUT, 10000000LL);
		int8 encStatus = BT_ERROR;
		if (result == B_OK)
			encReply.FindInt8("status", &encStatus);
		if (encStatus != BT_OK) {
			TRACE_SPP("Encryption failed (status 0x%02X)\n",
				(unsigned)(uint8)encStatus);
			return false;
		}
		TRACE_SPP("Encryption enabled\n");
	}

	return true;
}


/* =========================================================================
 * SDP query for SPP RFCOMM channel
 * ========================================================================= */

bool
SppClient::_QuerySdpForChannel(const bdaddr_t& remote, uint8& outChannel)
{
	/* Open L2CAP socket to SDP (PSM 1) */
	int sock = socket(PF_BLUETOOTH, SOCK_STREAM, BLUETOOTH_PROTO_L2CAP);
	if (sock < 0) {
		TRACE_SPP("SDP socket() failed: %s\n", strerror(errno));
		return false;
	}

	struct timeval tv;
	tv.tv_sec = 10;
	tv.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	struct sockaddr_l2cap addr;
	memset(&addr, 0, sizeof(addr));
	addr.l2cap_len = sizeof(addr);
	addr.l2cap_family = PF_BLUETOOTH;
	addr.l2cap_psm = L2CAP_PSM_SDP;
	memcpy(&addr.l2cap_bdaddr, &remote, sizeof(bdaddr_t));

	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		TRACE_SPP("SDP connect() failed: %s\n", strerror(errno));
		close(sock);
		return false;
	}

	/* Build ServiceSearchAttributeRequest for UUID 0x1101 (Serial Port) */
	uint8 sendBuf[64];
	uint8* p = sendBuf;

	p[0] = SDP_SERVICE_SEARCH_ATTR_REQ;
	WriteBE16(p + 1, 0x0001);  /* transaction ID */
	p += SDP_PDU_HEADER_SIZE;

	/* ServiceSearchPattern: Sequence { UUID16(0x1101) } */
	*p++ = SDP_DE_HEADER(SDP_DE_SEQUENCE, SDP_DE_SIZE_NEXT1);
	*p++ = 3;
	*p++ = SDP_DE_HEADER(SDP_DE_UUID, SDP_DE_SIZE_2);
	WriteBE16(p, SDP_UUID16_SERIAL_PORT);
	p += 2;

	/* MaximumAttributeByteCount */
	WriteBE16(p, 0xFFFF);
	p += 2;

	/* AttributeIDList: range 0x0000-0xFFFF (all attributes) */
	*p++ = SDP_DE_HEADER(SDP_DE_SEQUENCE, SDP_DE_SIZE_NEXT1);
	*p++ = 5;
	*p++ = SDP_DE_HEADER(SDP_DE_UINT, SDP_DE_SIZE_4);
	WriteBE32(p, 0x0000FFFF);
	p += 4;

	/* ContinuationState = 0 */
	*p++ = 0x00;

	uint16 paramLen = (uint16)(p - sendBuf - SDP_PDU_HEADER_SIZE);
	WriteBE16(sendBuf + 3, paramLen);
	uint32 sendLen = (uint32)(p - sendBuf);

	ssize_t sent = send(sock, sendBuf, sendLen, 0);
	if (sent < (ssize_t)sendLen) {
		TRACE_SPP("SDP send() failed\n");
		close(sock);
		return false;
	}

	/* Receive response */
	uint8 recvBuf[1024];
	ssize_t received = recv(sock, recvBuf, sizeof(recvBuf), 0);
	close(sock);

	if (received > 0) {
		TRACE_SPP("SDP response: %zd bytes:", received);
		for (ssize_t i = 0; i < received && i < 64; i++)
			fprintf(stderr, " %02X", recvBuf[i]);
		fprintf(stderr, "\n");
	}

	if (received < SDP_PDU_HEADER_SIZE) {
		TRACE_SPP("SDP response too short (%zd bytes)\n", received);
		return false;
	}

	uint8 pduId = recvBuf[0];
	if (pduId != SDP_SERVICE_SEARCH_ATTR_RSP) {
		TRACE_SPP("Unexpected SDP PDU: 0x%02X\n", pduId);
		return false;
	}

	const uint8* rp = recvBuf + SDP_PDU_HEADER_SIZE;
	uint32 rspRemaining = (uint32)received - SDP_PDU_HEADER_SIZE;

	if (rspRemaining < 2)
		return false;

	uint16 attrListByteCount = ReadBE16(rp);
	rp += 2;
	rspRemaining -= 2;

	if (attrListByteCount > rspRemaining)
		return false;

	/* Parse outer sequence (list of service records) */
	DataElement outerSeq;
	if (!ParseDataElement(rp, attrListByteCount, outerSeq)
		|| outerSeq.type != SDP_DE_SEQUENCE) {
		TRACE_SPP("Failed to parse SDP outer sequence\n");
		return false;
	}

	/* Walk each service record looking for ProtocolDescriptorList with RFCOMM */
	const uint8* rec = outerSeq.data;
	uint32 recRemaining = outerSeq.dataLen;

	while (recRemaining > 0) {
		DataElement recordDe;
		if (!ParseDataElement(rec, recRemaining, recordDe))
			break;

		if (recordDe.type == SDP_DE_SEQUENCE) {
			/* Walk attribute pairs in this record */
			const uint8* attrData = recordDe.data;
			uint32 attrRemaining = recordDe.dataLen;

			while (attrRemaining > 0) {
				DataElement attrIdDe;
				if (!ParseDataElement(attrData, attrRemaining, attrIdDe))
					break;
				if (attrIdDe.type != SDP_DE_UINT || attrIdDe.dataLen != 2) {
					attrData += attrIdDe.totalLen;
					attrRemaining -= attrIdDe.totalLen;
					continue;
				}
				uint16 attrId = ReadBE16(attrIdDe.data);
				attrData += attrIdDe.totalLen;
				attrRemaining -= attrIdDe.totalLen;

				DataElement value;
				if (!ParseDataElement(attrData, attrRemaining, value))
					break;

				if (attrId == SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST
					&& (value.type == SDP_DE_SEQUENCE
						|| value.type == SDP_DE_ALTERNATIVE)) {
					if (ExtractRfcommChannel(value.data, value.dataLen,
							outChannel)) {
						return true;
					}
				}

				attrData += value.totalLen;
				attrRemaining -= value.totalLen;
			}
		}

		rec += recordDe.totalLen;
		recRemaining -= recordDe.totalLen;
	}

	TRACE_SPP("No RFCOMM channel found in SDP response\n");
	return false;
}


} /* namespace Bluetooth */
