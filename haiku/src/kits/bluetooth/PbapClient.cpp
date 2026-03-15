/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * PbapClient — Phone Book Access Profile client implementation.
 */

#include <bluetooth/PbapClient.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <new>
#include <sys/socket.h>
#include <sys/time.h>

#include <Application.h>
#include <Messenger.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/L2CAP/btL2CAP.h>

#include <bluetooth/obex.h>
#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetoothserver_p.h>
#include <CommandManager.h>
#include <l2cap.h>

#include "ObexClient.h"
#include "RfcommSession.h"


#define TRACE_PBAP(fmt, ...) \
	fprintf(stderr, "PBAP: " fmt, ##__VA_ARGS__)

/* PBAP PSE (Phone Book Server Equipment) service UUID */
#define SDP_UUID16_PBAP_PSE		0x112F

/* SDP Attribute: GoepL2capPsm (PBAP 1.2 / GOEP 2.0) */
#define SDP_ATTR_GOEP_L2CAP_PSM	0x0200


/* =========================================================================
 * SDP Data Element parser (shared with SppClient)
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


static bool
ExtractRfcommChannel(const uint8* data, uint32 len, uint8& outChannel)
{
	uint32 off = 0;
	while (off < len) {
		DataElement stack;
		if (!ParseDataElement(data + off, len - off, stack))
			break;

		if (stack.type == SDP_DE_SEQUENCE
			|| stack.type == SDP_DE_ALTERNATIVE) {
			const uint8* inner = stack.data;
			uint32 innerLen = stack.dataLen;

			DataElement proto;
			if (ParseDataElement(inner, innerLen, proto)
				&& proto.type == SDP_DE_UUID && proto.dataLen == 2) {
				uint16 protoUuid = ReadBE16(proto.data);
				if (protoUuid == SDP_UUID16_RFCOMM) {
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
 * PbapClient implementation
 * ========================================================================= */

namespace Bluetooth {


PbapClient::PbapClient()
	:
	fRfcomm(NULL),
	fObex(NULL),
	fDlci(0),
	fL2capSocket(-1)
{
}


PbapClient::~PbapClient()
{
	Disconnect();
}


status_t
PbapClient::Connect(const bdaddr_t& address, uint8 rfcommChannel)
{
	if (fObex != NULL)
		return B_BUSY;

	/* Step 1: Ensure ACL connection */
	TRACE_PBAP("Establishing ACL connection...\n");
	if (!_EnsureAclConnection(address)) {
		TRACE_PBAP("Failed to establish ACL connection\n");
		return B_ERROR;
	}

	/* Step 2: SDP query — get both RFCOMM channel and GoepL2capPsm */
	uint16 goepL2capPsm = 0;
	if (rfcommChannel == 0) {
		TRACE_PBAP("Querying SDP for PBAP PSE...\n");
		if (!_QuerySdpForPbap(address, rfcommChannel, goepL2capPsm)) {
			TRACE_PBAP("SDP query failed — no PBAP PSE service found\n");
			return B_NAME_NOT_FOUND;
		}
		TRACE_PBAP("SDP found PBAP PSE: RFCOMM channel %u, "
			"GoepL2capPsm 0x%04X\n", rfcommChannel, goepL2capPsm);
	}

	/* Step 3: Try L2CAP direct transport (GOEP 2.0) if available.
	 * GOEP 2.0 uses OBEX over L2CAP with ERTM (Enhanced Retransmission
	 * Mode). The L2CAP module now supports ERTM and will negotiate it
	 * automatically for dynamic PSMs (>= 0x1000). */
	status_t result;
	if (goepL2capPsm != 0) {
		TRACE_PBAP("Trying L2CAP GOEP 2.0 on PSM 0x%04X...\n",
			goepL2capPsm);

		fL2capSocket = socket(PF_BLUETOOTH, SOCK_STREAM,
			BLUETOOTH_PROTO_L2CAP);
		if (fL2capSocket < 0) {
			TRACE_PBAP("L2CAP socket() failed: %s, "
				"falling back to RFCOMM\n", strerror(errno));
		} else {
			struct timeval tv;
			tv.tv_sec = 30;
			tv.tv_usec = 0;
			setsockopt(fL2capSocket, SOL_SOCKET, SO_RCVTIMEO,
				&tv, sizeof(tv));
			setsockopt(fL2capSocket, SOL_SOCKET, SO_SNDTIMEO,
				&tv, sizeof(tv));

			struct sockaddr_l2cap l2addr;
			memset(&l2addr, 0, sizeof(l2addr));
			l2addr.l2cap_len = sizeof(l2addr);
			l2addr.l2cap_family = PF_BLUETOOTH;
			l2addr.l2cap_psm = goepL2capPsm;
			memcpy(&l2addr.l2cap_bdaddr, &address, sizeof(bdaddr_t));

			if (connect(fL2capSocket, (struct sockaddr*)&l2addr,
					sizeof(l2addr)) < 0) {
				TRACE_PBAP("L2CAP connect() failed: %s, "
					"falling back to RFCOMM\n", strerror(errno));
				close(fL2capSocket);
				fL2capSocket = -1;
			} else {
				TRACE_PBAP("L2CAP connected (ERTM negotiated "
					"by kernel)\n");

				fObex = new(std::nothrow) ObexClient(fL2capSocket);
				if (fObex == NULL) {
					close(fL2capSocket);
					fL2capSocket = -1;
					return B_NO_MEMORY;
				}

				/* OBEX Connect with SupportedFeatures for PBAP 1.2 */
				uint8 sfParams[6];
				sfParams[0] = PBAP_TAG_SUPPORTED_FEATURES;
				sfParams[1] = 4;
				sfParams[2] = 0x00;
				sfParams[3] = 0x00;
				sfParams[4] = 0x03;
				sfParams[5] = 0xFF; /* 0x000003FF = all 10 bits */

				result = fObex->Connect(kPbapTargetUuid,
					sizeof(kPbapTargetUuid), sfParams, sizeof(sfParams));
				if (result == B_OK) {
					TRACE_PBAP("PBAP session established (L2CAP "
						"GOEP 2.0)\n");
					return B_OK;
				}

				TRACE_PBAP("OBEX Connect over L2CAP failed: %s, "
					"falling back to RFCOMM\n", strerror(result));
				delete fObex;
				fObex = NULL;
				close(fL2capSocket);
				fL2capSocket = -1;
			}
		}
	}

	/* Step 4: RFCOMM session (fallback or only transport) */
	if (rfcommChannel == 0) {
		TRACE_PBAP("No RFCOMM channel from SDP\n");
		return B_NAME_NOT_FOUND;
	}

	fRfcomm = new(std::nothrow) RfcommSession();
	if (fRfcomm == NULL)
		return B_NO_MEMORY;

	result = fRfcomm->Connect(address);
	if (result != B_OK) {
		TRACE_PBAP("RFCOMM connect failed: %s\n", strerror(result));
		delete fRfcomm;
		fRfcomm = NULL;
		return result;
	}

	result = fRfcomm->OpenChannel(rfcommChannel);
	if (result != B_OK) {
		TRACE_PBAP("RFCOMM channel open failed: %s\n", strerror(result));
		fRfcomm->Disconnect();
		delete fRfcomm;
		fRfcomm = NULL;
		return result;
	}

	fDlci = RFCOMM_DLCI(1, rfcommChannel);
	TRACE_PBAP("RFCOMM channel %u open (DLCI %u)\n", rfcommChannel, fDlci);

	/* Step 5: OBEX Connect with PBAP Target UUID */
	fObex = new(std::nothrow) ObexClient(fRfcomm, fDlci);
	if (fObex == NULL) {
		fRfcomm->Disconnect();
		delete fRfcomm;
		fRfcomm = NULL;
		return B_NO_MEMORY;
	}

	result = fObex->Connect(kPbapTargetUuid, sizeof(kPbapTargetUuid));
	if (result != B_OK) {
		TRACE_PBAP("OBEX Connect failed: %s\n", strerror(result));
		delete fObex;
		fObex = NULL;
		fRfcomm->Disconnect();
		delete fRfcomm;
		fRfcomm = NULL;
		return result;
	}

	TRACE_PBAP("PBAP session established (RFCOMM)\n");
	return B_OK;
}


status_t
PbapClient::ConnectL2cap(const bdaddr_t& address)
{
	if (fObex != NULL)
		return B_BUSY;

	/* Step 1: Ensure ACL connection */
	TRACE_PBAP("ConnectL2cap: Establishing ACL connection...\n");
	if (!_EnsureAclConnection(address)) {
		TRACE_PBAP("Failed to establish ACL connection\n");
		return B_ERROR;
	}

	/* Step 2: SDP query — get GoepL2capPsm */
	uint8 rfcommChannel = 0;
	uint16 goepL2capPsm = 0;
	TRACE_PBAP("ConnectL2cap: Querying SDP for PBAP PSE...\n");
	if (!_QuerySdpForPbap(address, rfcommChannel, goepL2capPsm)) {
		TRACE_PBAP("SDP query failed — no PBAP PSE service found\n");
		return B_NAME_NOT_FOUND;
	}

	if (goepL2capPsm == 0) {
		TRACE_PBAP("ConnectL2cap: Remote device does not advertise "
			"GoepL2capPsm — L2CAP GOEP 2.0 not available\n");
		return B_NOT_SUPPORTED;
	}

	TRACE_PBAP("ConnectL2cap: Using L2CAP PSM 0x%04X (GOEP 2.0)\n",
		goepL2capPsm);

	/* Step 3: L2CAP connect (ERTM negotiated by kernel) */
	fL2capSocket = socket(PF_BLUETOOTH, SOCK_STREAM,
		BLUETOOTH_PROTO_L2CAP);
	if (fL2capSocket < 0) {
		TRACE_PBAP("L2CAP socket() failed: %s\n", strerror(errno));
		return B_ERROR;
	}

	struct timeval tv;
	tv.tv_sec = 30;
	tv.tv_usec = 0;
	setsockopt(fL2capSocket, SOL_SOCKET, SO_RCVTIMEO,
		&tv, sizeof(tv));
	setsockopt(fL2capSocket, SOL_SOCKET, SO_SNDTIMEO,
		&tv, sizeof(tv));

	struct sockaddr_l2cap l2addr;
	memset(&l2addr, 0, sizeof(l2addr));
	l2addr.l2cap_len = sizeof(l2addr);
	l2addr.l2cap_family = PF_BLUETOOTH;
	l2addr.l2cap_psm = goepL2capPsm;
	memcpy(&l2addr.l2cap_bdaddr, &address, sizeof(bdaddr_t));

	if (connect(fL2capSocket, (struct sockaddr*)&l2addr,
			sizeof(l2addr)) < 0) {
		TRACE_PBAP("L2CAP connect() failed: %s\n", strerror(errno));
		close(fL2capSocket);
		fL2capSocket = -1;
		return B_ERROR;
	}

	TRACE_PBAP("ConnectL2cap: L2CAP connected (ERTM negotiated)\n");

	/* Step 4: OBEX Connect with PBAP Target UUID */
	fObex = new(std::nothrow) ObexClient(fL2capSocket);
	if (fObex == NULL) {
		close(fL2capSocket);
		fL2capSocket = -1;
		return B_NO_MEMORY;
	}

	/* SupportedFeatures for PBAP 1.2 */
	uint8 sfParams[6];
	sfParams[0] = PBAP_TAG_SUPPORTED_FEATURES;
	sfParams[1] = 4;
	sfParams[2] = 0x00;
	sfParams[3] = 0x00;
	sfParams[4] = 0x03;
	sfParams[5] = 0xFF;  /* 0x000003FF = all 10 bits */

	status_t result = fObex->Connect(kPbapTargetUuid,
		sizeof(kPbapTargetUuid), sfParams, sizeof(sfParams));
	if (result != B_OK) {
		TRACE_PBAP("OBEX Connect over L2CAP failed: %s\n",
			strerror(result));
		delete fObex;
		fObex = NULL;
		close(fL2capSocket);
		fL2capSocket = -1;
		return result;
	}

	TRACE_PBAP("PBAP session established (L2CAP GOEP 2.0)\n");
	return B_OK;
}


void
PbapClient::Disconnect()
{
	if (fObex != NULL) {
		fObex->Disconnect();
		delete fObex;
		fObex = NULL;
	}

	if (fRfcomm != NULL) {
		if (fDlci != 0)
			fRfcomm->CloseChannel(fDlci);
		fRfcomm->Disconnect();
		delete fRfcomm;
		fRfcomm = NULL;
	}

	if (fL2capSocket >= 0) {
		close(fL2capSocket);
		fL2capSocket = -1;
	}

	fDlci = 0;
}


bool
PbapClient::IsConnected() const
{
	return fObex != NULL && fObex->IsConnected();
}


/* =========================================================================
 * PullPhoneBook — OBEX GET with x-bt/phonebook type
 * ========================================================================= */

status_t
PbapClient::PullPhoneBook(const char* path, uint8 format,
	uint8** outData, size_t* outLen)
{
	if (!IsConnected())
		return B_NOT_ALLOWED;

	/* Build PBAP application parameters (matching BlueZ defaults):
	 *   Format (tag 0x07, 1 byte): vCard 2.1 or 3.0
	 *   MaxListCount (tag 0x04, 2 bytes): 0xFFFF = unlimited
	 *   ListStartOffset (tag 0x05, 2 bytes): 0x0000 */
	uint8 appParams[12];
	size_t appLen = 0;

	/* Format */
	appParams[appLen++] = PBAP_TAG_FORMAT;
	appParams[appLen++] = 1;
	appParams[appLen++] = format;

	/* MaxListCount = 65535 */
	appParams[appLen++] = PBAP_TAG_MAX_LIST_COUNT;
	appParams[appLen++] = 2;
	appParams[appLen++] = 0xFF;
	appParams[appLen++] = 0xFF;

	/* ListStartOffset = 0 */
	appParams[appLen++] = PBAP_TAG_LIST_START_OFFSET;
	appParams[appLen++] = 2;
	appParams[appLen++] = 0x00;
	appParams[appLen++] = 0x00;

	TRACE_PBAP("PullPhoneBook: %s (format=%u)\n", path, format);

	return fObex->Get(path, "x-bt/phonebook", appParams, appLen,
		outData, outLen);
}


/* =========================================================================
 * SetPath — OBEX SETPATH for virtual folder navigation
 * ========================================================================= */

status_t
PbapClient::SetPath(const char* name)
{
	if (!IsConnected())
		return B_NOT_ALLOWED;

	return fObex->SetPath(name, 0x02);
}


/* =========================================================================
 * GetPhoneBookSize — OBEX GET with MaxListCount=0 (size-only query)
 * ========================================================================= */

status_t
PbapClient::GetPhoneBookSize(const char* path, uint16* outSize)
{
	if (!IsConnected())
		return B_NOT_ALLOWED;

	/* MaxListCount=0 tells the PSE to return only the PhonebookSize
	 * in the response AppParams, without any body data. */
	uint8 appParams[4];
	size_t appLen = 0;

	/* MaxListCount = 0 */
	appParams[appLen++] = PBAP_TAG_MAX_LIST_COUNT;
	appParams[appLen++] = 2;
	appParams[appLen++] = 0x00;
	appParams[appLen++] = 0x00;

	TRACE_PBAP("GetPhoneBookSize: %s\n", path);

	uint8* data = NULL;
	size_t dataLen = 0;
	status_t result = fObex->Get(path, "x-bt/phonebook", appParams, appLen,
		&data, &dataLen);

	free(data);

	if (result != B_OK)
		return result;

	/* Parse PhonebookSize from response AppParams would require
	 * parsing the OBEX response headers in ObexClient.  For now,
	 * just report success — the trace will show any response. */
	*outSize = 0;
	TRACE_PBAP("GetPhoneBookSize: request completed (data=%zu bytes)\n",
		dataLen);

	return B_OK;
}


/* =========================================================================
 * PullvCardListing — OBEX GET with x-bt/vcard-listing type
 * ========================================================================= */

status_t
PbapClient::PullvCardListing(const char* path,
	uint8** outData, size_t* outLen)
{
	if (!IsConnected())
		return B_NOT_ALLOWED;

	/* MaxListCount = 65535, ListStartOffset = 0 */
	uint8 appParams[8];
	size_t appLen = 0;

	appParams[appLen++] = PBAP_TAG_MAX_LIST_COUNT;
	appParams[appLen++] = 2;
	appParams[appLen++] = 0xFF;
	appParams[appLen++] = 0xFF;

	appParams[appLen++] = PBAP_TAG_LIST_START_OFFSET;
	appParams[appLen++] = 2;
	appParams[appLen++] = 0x00;
	appParams[appLen++] = 0x00;

	TRACE_PBAP("PullvCardListing: %s\n", path);

	return fObex->Get(path, "x-bt/vcard-listing", appParams, appLen,
		outData, outLen);
}


/* =========================================================================
 * ACL connection via BluetoothServer
 * (Same pattern as SppClient::_EnsureAclConnection)
 * ========================================================================= */

bool
PbapClient::_EnsureAclConnection(const bdaddr_t& remote)
{
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		TRACE_PBAP("Cannot reach BluetoothServer\n");
		return false;
	}

	/* Acquire local HCI device */
	BMessage acquire(BT_MSG_ACQUIRE_LOCAL_DEVICE);
	BMessage acquireReply;
	if (messenger.SendMessage(&acquire, &acquireReply, B_INFINITE_TIMEOUT,
			5000000LL) != B_OK) {
		TRACE_PBAP("Failed to query local device\n");
		return false;
	}

	hci_id hid;
	if (acquireReply.FindInt32("hci_id", &hid) != B_OK || hid < 0) {
		TRACE_PBAP("No local Bluetooth device found\n");
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

	TRACE_PBAP("Sending HCI Create Connection (timeout 60s)...\n");
	TRACE_PBAP("  Petition events: CMD_STATUS, LINK_KEY_REQ, "
		"LINK_KEY_NOTIFY, IO_CAP_REQ, IO_CAP_RSP, "
		"USER_CONFIRM, SSP_COMPLETE, CONN_COMPLETE\n");

	int8 btStatus = BT_ERROR;
	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 60000000LL);

	TRACE_PBAP("SendMessage returned: %s (0x%08lx)\n",
		strerror(result), (long)result);

	if (result == B_TIMED_OUT) {
		TRACE_PBAP("ACL connection timed out\n");
		return false;
	}
	if (result != B_OK) {
		TRACE_PBAP("SendMessage failed: %s\n", strerror(result));
		return false;
	}

	TRACE_PBAP("Reply what=0x%08lx\n", (long)reply.what);
	reply.FindInt8("status", &btStatus);
	TRACE_PBAP("Reply status=0x%02X\n", (unsigned)(uint8)btStatus);
	if (btStatus == 0x0B) {
		/* 0x0B = ACL Connection Already Exists — phone auto-connected.
		 * The link may NOT be encrypted. We must still authenticate and
		 * encrypt, but we need the connection handle. Try to find it
		 * by probing with HCI_Authentication_Requested on likely handles
		 * (BCM2070 assigns handles starting at 0x000B). */
		TRACE_PBAP("ACL already exists (phone auto-connected)\n");

		int16 existingHandle = -1;
		for (uint16 tryHandle = 0x0001; tryHandle <= 0x0010; tryHandle++) {
			BluetoothCommand<typed_command(hci_cp_auth_requested)>
				probeCmd(OGF_LINK_CONTROL, OCF_AUTH_REQUESTED);
			probeCmd->handle = tryHandle;

			BMessage probeReq(BT_MSG_HANDLE_SIMPLE_REQUEST);
			BMessage probeReply;
			probeReq.AddInt32("hci_id", hid);
			probeReq.AddData("raw command", B_ANY_TYPE,
				probeCmd.Data(), probeCmd.Size());
			probeReq.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
			probeReq.AddInt16("opcodeExpected",
				PACK_OPCODE(OGF_LINK_CONTROL, OCF_AUTH_REQUESTED));
			probeReq.AddInt16("eventExpected", HCI_EVENT_LINK_KEY_REQ);
			probeReq.AddInt16("eventExpected",
				HCI_EVENT_IO_CAPABILITY_REQUEST);
			probeReq.AddInt16("eventExpected",
				HCI_EVENT_IO_CAPABILITY_RESPONSE);
			probeReq.AddInt16("eventExpected",
				HCI_EVENT_USER_CONFIRMATION_REQUEST);
			probeReq.AddInt16("eventExpected",
				HCI_EVENT_SIMPLE_PAIRING_COMPLETE);
			probeReq.AddInt16("eventExpected", HCI_EVENT_LINK_KEY_NOTIFY);
			probeReq.AddInt16("eventExpected", HCI_EVENT_AUTH_COMPLETE);

			result = messenger.SendMessage(&probeReq, &probeReply,
				B_INFINITE_TIMEOUT, 30000000LL);
			int8 probeStatus = BT_ERROR;
			if (result == B_OK)
				probeReply.FindInt8("status", &probeStatus);

			if (probeStatus == BT_OK) {
				existingHandle = (int16)tryHandle;
				TRACE_PBAP("Found ACL handle=0x%04X, "
					"authentication complete\n",
					(unsigned)tryHandle);
				break;
			}
			/* 0x02 = Unknown Connection Identifier — wrong handle,
			 * try next.
			 * 0x12 = Command Disallowed — handle exists but auth
			 * was not possible (already auth'd, or SCO handle).
			 * Remember it and keep probing for a better match. */
			if (probeStatus == 0x02) {
				continue;
			}
			TRACE_PBAP("Auth probe handle=0x%04X: status 0x%02X\n",
				(unsigned)tryHandle, (unsigned)(uint8)probeStatus);
			if (existingHandle < 0)
				existingHandle = (int16)tryHandle;
		}

		if (existingHandle < 0) {
			TRACE_PBAP("Could not find ACL handle, "
				"proceeding without auth/encrypt\n");
			return true;
		}

		/* Enable encryption on the existing link */
		{
			TRACE_PBAP("Enabling encryption on existing link...\n");
			BluetoothCommand<typed_command(hci_cp_set_conn_encrypt)>
				encCmd(OGF_LINK_CONTROL, OCF_SET_CONN_ENCRYPT);
			encCmd->handle = (uint16)existingHandle;
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
				TRACE_PBAP("Encryption failed (status 0x%02X), "
					"proceeding anyway\n",
					(unsigned)(uint8)encStatus);
			} else {
				TRACE_PBAP("Encryption enabled\n");
			}
		}

		return true;
	} else if (btStatus != BT_OK) {
		TRACE_PBAP("ACL connection failed (HCI status 0x%02X)\n",
			(unsigned)(uint8)btStatus);
		return false;
	}

	int16 handle = -1;
	reply.FindInt16("handle", &handle);
	TRACE_PBAP("ACL connection established (handle=0x%04X)\n",
		(unsigned)(uint16)handle);

	if (handle < 0) {
		TRACE_PBAP("No connection handle in reply, skipping auth/encrypt\n");
		return true;
	}

	/* Authenticate the link */
	{
		TRACE_PBAP("Requesting authentication...\n");
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
		authReq.AddInt16("eventExpected",
			HCI_EVENT_IO_CAPABILITY_REQUEST);
		authReq.AddInt16("eventExpected",
			HCI_EVENT_IO_CAPABILITY_RESPONSE);
		authReq.AddInt16("eventExpected",
			HCI_EVENT_USER_CONFIRMATION_REQUEST);
		authReq.AddInt16("eventExpected",
			HCI_EVENT_SIMPLE_PAIRING_COMPLETE);
		authReq.AddInt16("eventExpected", HCI_EVENT_LINK_KEY_NOTIFY);
		authReq.AddInt16("eventExpected", HCI_EVENT_AUTH_COMPLETE);

		result = messenger.SendMessage(&authReq, &authReply,
			B_INFINITE_TIMEOUT, 30000000LL);
		int8 authStatus = BT_ERROR;
		if (result == B_OK)
			authReply.FindInt8("status", &authStatus);
		if (authStatus != BT_OK) {
			TRACE_PBAP("Authentication failed (status 0x%02X)\n",
				(unsigned)(uint8)authStatus);
			return false;
		}
		TRACE_PBAP("Authentication complete\n");
	}

	/* The server now creates a petition for SET_CONN_ENCRYPT after
	 * AUTH_COMPLETE, so encryption completes asynchronously.
	 * Wait a bit for it to finish before proceeding. */
	TRACE_PBAP("Waiting for encryption to complete...\n");
	snooze(200000);	/* 200 ms — enough for encrypt handshake */

	return true;
}


/* =========================================================================
 * SDP query for PBAP PSE — extracts both RFCOMM channel and GoepL2capPsm
 * ========================================================================= */

bool
PbapClient::_QuerySdpForPbap(const bdaddr_t& remote,
	uint8& outChannel, uint16& outL2capPsm)
{
	outChannel = 0;
	outL2capPsm = 0;

	int sock = socket(PF_BLUETOOTH, SOCK_STREAM, BLUETOOTH_PROTO_L2CAP);
	if (sock < 0) {
		TRACE_PBAP("SDP socket() failed: %s\n", strerror(errno));
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

	TRACE_PBAP("SDP: connecting L2CAP to PSM %d...\n", L2CAP_PSM_SDP);
	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		TRACE_PBAP("SDP connect() failed: %s\n", strerror(errno));
		close(sock);
		return false;
	}
	TRACE_PBAP("SDP: L2CAP connected\n");

	/* Build ServiceSearchAttributeRequest for UUID 0x112F (PBAP PSE) */
	uint8 sendBuf[64];
	uint8* p = sendBuf;

	p[0] = SDP_SERVICE_SEARCH_ATTR_REQ;
	WriteBE16(p + 1, 0x0001);  /* transaction ID */
	p += SDP_PDU_HEADER_SIZE;

	/* ServiceSearchPattern: Sequence { UUID16(0x112F) } */
	*p++ = SDP_DE_HEADER(SDP_DE_SEQUENCE, SDP_DE_SIZE_NEXT1);
	*p++ = 3;
	*p++ = SDP_DE_HEADER(SDP_DE_UUID, SDP_DE_SIZE_2);
	WriteBE16(p, SDP_UUID16_PBAP_PSE);
	p += 2;

	/* MaximumAttributeByteCount */
	WriteBE16(p, 0xFFFF);
	p += 2;

	/* AttributeIDList: range 0x0000-0xFFFF */
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
		TRACE_PBAP("SDP send() failed\n");
		close(sock);
		return false;
	}

	/* Receive response */
	uint8 recvBuf[1024];
	ssize_t received = recv(sock, recvBuf, sizeof(recvBuf), 0);
	close(sock);

	if (received > 0) {
		TRACE_PBAP("SDP response: %zd bytes:", received);
		for (ssize_t i = 0; i < received; i++)
			fprintf(stderr, " %02X", recvBuf[i]);
		fprintf(stderr, "\n");
	}

	if (received < SDP_PDU_HEADER_SIZE) {
		TRACE_PBAP("SDP response too short (%zd bytes)\n", received);
		return false;
	}

	uint8 pduId = recvBuf[0];
	if (pduId != SDP_SERVICE_SEARCH_ATTR_RSP) {
		TRACE_PBAP("Unexpected SDP PDU: 0x%02X\n", pduId);
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

	/* Parse outer sequence */
	DataElement outerSeq;
	if (!ParseDataElement(rp, attrListByteCount, outerSeq)
		|| outerSeq.type != SDP_DE_SEQUENCE) {
		TRACE_PBAP("Failed to parse SDP outer sequence\n");
		return false;
	}

	/* Walk service records for ProtocolDescriptorList and GoepL2capPsm */
	const uint8* rec = outerSeq.data;
	uint32 recRemaining = outerSeq.dataLen;
	bool foundChannel = false;

	while (recRemaining > 0) {
		DataElement recordDe;
		if (!ParseDataElement(rec, recRemaining, recordDe))
			break;

		if (recordDe.type == SDP_DE_SEQUENCE) {
			const uint8* attrData = recordDe.data;
			uint32 attrRemaining = recordDe.dataLen;

			while (attrRemaining > 0) {
				DataElement attrIdDe;
				if (!ParseDataElement(attrData, attrRemaining, attrIdDe))
					break;
				if (attrIdDe.type != SDP_DE_UINT
					|| attrIdDe.dataLen != 2) {
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
						foundChannel = true;
					}
				}

				/* GoepL2capPsm (PBAP 1.2) — Uint16 */
				if (attrId == SDP_ATTR_GOEP_L2CAP_PSM
					&& value.type == SDP_DE_UINT
					&& value.dataLen == 2) {
					outL2capPsm = ReadBE16(value.data);
					TRACE_PBAP("SDP: GoepL2capPsm = 0x%04X\n",
						outL2capPsm);
				}

				attrData += value.totalLen;
				attrRemaining -= value.totalLen;
			}
		}

		rec += recordDe.totalLen;
		recRemaining -= recordDe.totalLen;
	}

	if (!foundChannel && outL2capPsm == 0) {
		TRACE_PBAP("No RFCOMM channel or L2CAP PSM found "
			"in PBAP SDP response\n");
		return false;
	}

	return true;
}


} /* namespace Bluetooth */
