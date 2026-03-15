/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * HfpClient — Hands-Free Profile client implementation.
 */

#include <bluetooth/HfpClient.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

#include <bluetooth/rfcomm.h>
#include <bluetooth/sdp.h>
#include <bluetoothserver_p.h>
#include <CommandManager.h>
#include <l2cap.h>

#include "AtParser.h"
#include "RfcommSession.h"


#define TRACE_HFP(fmt, ...) \
	fprintf(stderr, "HFP: " fmt, ##__VA_ARGS__)

/* HFP AG UUID */
#define SDP_UUID16_HFP_AG	0x111F
#define SDP_UUID16_HFP_HF	0x111E


/* =========================================================================
 * SDP Data Element parser (shared pattern with SppClient/PbapClient/OppClient)
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
 * HfpClient implementation
 * ========================================================================= */

namespace Bluetooth {


HfpClient::HfpClient()
	:
	fRfcomm(NULL),
	fDlci(0),
	fLocalFeatures(HFP_HF_FEATURE_CLIP | HFP_HF_FEATURE_VOLUME
		| HFP_HF_FEATURE_3WAY | HFP_HF_FEATURE_ECS),
	fRemoteFeatures(0),
	fSlcEstablished(false)
{
}


HfpClient::~HfpClient()
{
	Disconnect();
}


status_t
HfpClient::Connect(const bdaddr_t& address, uint8 rfcommChannel)
{
	if (fRfcomm != NULL)
		return B_BUSY;

	/* Step 1: ACL connection */
	TRACE_HFP("Establishing ACL connection...\n");
	if (!_EnsureAclConnection(address)) {
		TRACE_HFP("Failed to establish ACL connection\n");
		return B_ERROR;
	}

	/* Step 2: SDP query for HFP AG channel */
	if (rfcommChannel == 0) {
		TRACE_HFP("Querying SDP for HFP AG channel...\n");
		if (!_QuerySdpForHfp(address, rfcommChannel)) {
			TRACE_HFP("SDP query failed — no HFP AG service found\n");
			return B_NAME_NOT_FOUND;
		}
		TRACE_HFP("SDP found HFP AG RFCOMM channel %u\n", rfcommChannel);
	}

	/* Step 3: RFCOMM session */
	fRfcomm = new(std::nothrow) RfcommSession();
	if (fRfcomm == NULL)
		return B_NO_MEMORY;

	status_t result = fRfcomm->Connect(address);
	if (result != B_OK) {
		TRACE_HFP("RFCOMM connect failed: %s\n", strerror(result));
		delete fRfcomm;
		fRfcomm = NULL;
		return result;
	}

	result = fRfcomm->OpenChannel(rfcommChannel);
	if (result != B_OK) {
		TRACE_HFP("RFCOMM channel open failed: %s\n", strerror(result));
		fRfcomm->Disconnect();
		delete fRfcomm;
		fRfcomm = NULL;
		return result;
	}

	fDlci = RFCOMM_DLCI(1, rfcommChannel);
	TRACE_HFP("RFCOMM channel %u open (DLCI %u)\n", rfcommChannel, fDlci);

	return B_OK;
}


void
HfpClient::Disconnect()
{
	fSlcEstablished = false;
	fRemoteFeatures = 0;

	if (fRfcomm != NULL) {
		if (fDlci != 0)
			fRfcomm->CloseChannel(fDlci);
		fRfcomm->Disconnect();
		delete fRfcomm;
		fRfcomm = NULL;
	}

	fDlci = 0;
}


bool
HfpClient::IsConnected() const
{
	return fRfcomm != NULL;
}


bool
HfpClient::IsServiceLevelEstablished() const
{
	return fSlcEstablished;
}


status_t
HfpClient::EstablishServiceLevel()
{
	if (fRfcomm == NULL)
		return B_NOT_ALLOWED;

	char response[256];

	/* Step 1: AT+BRSF=<features> */
	{
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "AT+BRSF=%u",
			(unsigned)fLocalFeatures);
		TRACE_HFP("SLC: Sending %s\n", cmd);
		status_t result = SendAt(cmd, response, sizeof(response));
		if (result != B_OK) {
			TRACE_HFP("SLC: AT+BRSF failed\n");
			return result;
		}
		TRACE_HFP("SLC: AG response: %s\n", response);

		// Parse +BRSF:<features> from response
		const char* brsf = strstr(response, "+BRSF:");
		if (brsf != NULL)
			fRemoteFeatures = (uint32)atoi(brsf + 6);
		TRACE_HFP("SLC: AG features=0x%08x\n", (unsigned)fRemoteFeatures);
	}

	/* Step 2: AT+CIND=? (indicator mapping) */
	{
		TRACE_HFP("SLC: Sending AT+CIND=?\n");
		status_t result = SendAt("AT+CIND=?", response, sizeof(response));
		if (result != B_OK) {
			TRACE_HFP("SLC: AT+CIND=? failed\n");
			return result;
		}
		TRACE_HFP("SLC: CIND test: %s\n", response);
	}

	/* Step 3: AT+CIND? (indicator values) */
	{
		TRACE_HFP("SLC: Sending AT+CIND?\n");
		status_t result = SendAt("AT+CIND?", response, sizeof(response));
		if (result != B_OK) {
			TRACE_HFP("SLC: AT+CIND? failed\n");
			return result;
		}
		TRACE_HFP("SLC: CIND read: %s\n", response);
	}

	/* Step 4: AT+CMER=3,0,0,1 (enable indicator reporting) */
	{
		TRACE_HFP("SLC: Sending AT+CMER=3,0,0,1\n");
		status_t result = SendAt("AT+CMER=3,0,0,1", response,
			sizeof(response));
		if (result != B_OK) {
			TRACE_HFP("SLC: AT+CMER failed\n");
			return result;
		}
	}

	/* Step 5: AT+CHLD=? (call hold features) */
	{
		TRACE_HFP("SLC: Sending AT+CHLD=?\n");
		status_t result = SendAt("AT+CHLD=?", response, sizeof(response));
		if (result != B_OK) {
			TRACE_HFP("SLC: AT+CHLD=? failed\n");
			return result;
		}
		TRACE_HFP("SLC: CHLD test: %s\n", response);
	}

	fSlcEstablished = true;
	TRACE_HFP("SLC: Service Level Connection established\n");
	return B_OK;
}


status_t
HfpClient::Dial(const char* number)
{
	if (!fSlcEstablished)
		return B_NOT_ALLOWED;

	char cmd[128];
	snprintf(cmd, sizeof(cmd), "ATD%s;", number);

	char response[256];
	return SendAt(cmd, response, sizeof(response), 30000000);
}


status_t
HfpClient::Answer()
{
	if (!fSlcEstablished)
		return B_NOT_ALLOWED;

	char response[256];
	return SendAt("ATA", response, sizeof(response), 10000000);
}


status_t
HfpClient::HangUp()
{
	if (!fSlcEstablished)
		return B_NOT_ALLOWED;

	char response[256];
	return SendAt("AT+CHUP", response, sizeof(response));
}


status_t
HfpClient::SetSpeakerVolume(uint8 level)
{
	if (!fSlcEstablished)
		return B_NOT_ALLOWED;

	if (level > 15)
		level = 15;

	char cmd[32];
	snprintf(cmd, sizeof(cmd), "AT+VGS=%u", level);

	char response[256];
	return SendAt(cmd, response, sizeof(response));
}


status_t
HfpClient::SetMicVolume(uint8 level)
{
	if (!fSlcEstablished)
		return B_NOT_ALLOWED;

	if (level > 15)
		level = 15;

	char cmd[32];
	snprintf(cmd, sizeof(cmd), "AT+VGM=%u", level);

	char response[256];
	return SendAt(cmd, response, sizeof(response));
}


status_t
HfpClient::SendAt(const char* command, char* response, size_t maxLen,
	bigtime_t timeout)
{
	status_t result = _SendAtLine(command);
	if (result != B_OK)
		return result;

	// Accumulate response lines until we get OK or ERROR
	size_t totalLen = 0;
	if (response != NULL && maxLen > 0)
		response[0] = '\0';

	while (true) {
		char line[256];
		result = _ReadAtLine(line, sizeof(line), timeout);
		if (result != B_OK) {
			TRACE_HFP("AT read failed: %s\n", strerror(result));
			return result;
		}

		AtParser::Command parsed;
		AtParser::Parse(line, parsed);

		// Append to response buffer
		if (response != NULL) {
			size_t lineLen = strlen(line);
			if (totalLen + lineLen + 2 < maxLen) {
				if (totalLen > 0)
					response[totalLen++] = '\n';
				memcpy(response + totalLen, line, lineLen);
				totalLen += lineLen;
				response[totalLen] = '\0';
			}
		}

		if (parsed.type == AtParser::AT_CMD_OK)
			return B_OK;
		if (parsed.type == AtParser::AT_CMD_ERROR
			|| parsed.type == AtParser::AT_CMD_CME_ERROR)
			return B_ERROR;
	}
}


status_t
HfpClient::_SendAtLine(const char* line)
{
	if (fRfcomm == NULL)
		return B_NO_INIT;

	// Send AT command with \r terminator
	size_t len = strlen(line);
	uint8 buf[512];
	if (len + 1 > sizeof(buf))
		return B_BUFFER_OVERFLOW;

	memcpy(buf, line, len);
	buf[len] = '\r';

	ssize_t sent = fRfcomm->Send(fDlci, buf, len + 1);
	if (sent < 0)
		return (status_t)sent;

	return B_OK;
}


status_t
HfpClient::_ReadAtLine(char* buf, size_t maxLen, bigtime_t timeout)
{
	if (fRfcomm == NULL)
		return B_NO_INIT;

	// Read until \r or \n
	size_t pos = 0;
	bigtime_t deadline = system_time() + timeout;

	while (pos < maxLen - 1) {
		uint8 byte;
		ssize_t received = fRfcomm->Receive(fDlci, &byte, 1,
			deadline - system_time());

		if (received < 0) {
			if (received == B_TIMED_OUT && pos > 0)
				break;
			return (status_t)received;
		}
		if (received == 0)
			return B_IO_ERROR;

		if (byte == '\r' || byte == '\n') {
			if (pos > 0)
				break;
			continue;  // skip leading \r\n
		}

		buf[pos++] = (char)byte;
	}

	buf[pos] = '\0';

	if (pos == 0)
		return B_TIMED_OUT;

	return B_OK;
}


/* =========================================================================
 * ACL connection via BluetoothServer
 * (Same pattern as SppClient/OppClient)
 * ========================================================================= */

bool
HfpClient::_EnsureAclConnection(const bdaddr_t& remote)
{
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		TRACE_HFP("Cannot reach BluetoothServer\n");
		return false;
	}

	BMessage acquire(BT_MSG_ACQUIRE_LOCAL_DEVICE);
	BMessage acquireReply;
	if (messenger.SendMessage(&acquire, &acquireReply, B_INFINITE_TIMEOUT,
			5000000LL) != B_OK) {
		TRACE_HFP("Failed to query local device\n");
		return false;
	}

	hci_id hid;
	if (acquireReply.FindInt32("hci_id", &hid) != B_OK || hid < 0) {
		TRACE_HFP("No local Bluetooth device found\n");
		return false;
	}

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

	TRACE_HFP("Sending HCI Create Connection (timeout 30s)...\n");

	int8 btStatus = BT_ERROR;
	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 30000000LL);

	if (result == B_TIMED_OUT) {
		TRACE_HFP("ACL connection timed out\n");
		return false;
	}
	if (result != B_OK) {
		TRACE_HFP("SendMessage failed: %s\n", strerror(result));
		return false;
	}

	reply.FindInt8("status", &btStatus);
	if (btStatus == 0x0B) {
		TRACE_HFP("ACL already exists\n");
		return true;
	} else if (btStatus != BT_OK) {
		TRACE_HFP("ACL connection failed (HCI status 0x%02X)\n",
			(unsigned)(uint8)btStatus);
		return false;
	}

	int16 handle = -1;
	reply.FindInt16("handle", &handle);
	TRACE_HFP("ACL connection established (handle=0x%04X)\n",
		(unsigned)(uint16)handle);

	if (handle < 0)
		return true;

	/* Authenticate */
	{
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
			TRACE_HFP("Authentication failed (status 0x%02X)\n",
				(unsigned)(uint8)authStatus);
			return false;
		}
	}

	/* Encrypt */
	{
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
			TRACE_HFP("Encryption failed (status 0x%02X)\n",
				(unsigned)(uint8)encStatus);
			return false;
		}
	}

	return true;
}


/* =========================================================================
 * SDP query for HFP AG RFCOMM channel
 * ========================================================================= */

bool
HfpClient::_QuerySdpForHfp(const bdaddr_t& remote, uint8& outChannel)
{
	outChannel = 0;

	int sock = socket(PF_BLUETOOTH, SOCK_STREAM, BLUETOOTH_PROTO_L2CAP);
	if (sock < 0) {
		TRACE_HFP("SDP socket() failed: %s\n", strerror(errno));
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
		TRACE_HFP("SDP connect() failed: %s\n", strerror(errno));
		close(sock);
		return false;
	}

	/* Build ServiceSearchAttributeRequest for UUID 0x111F (HFP AG) */
	uint8 sendBuf[64];
	uint8* p = sendBuf;

	p[0] = SDP_SERVICE_SEARCH_ATTR_REQ;
	WriteBE16(p + 1, 0x0001);
	p += SDP_PDU_HEADER_SIZE;

	/* ServiceSearchPattern: Sequence { UUID16(0x111F) } */
	*p++ = SDP_DE_HEADER(SDP_DE_SEQUENCE, SDP_DE_SIZE_NEXT1);
	*p++ = 3;
	*p++ = SDP_DE_HEADER(SDP_DE_UUID, SDP_DE_SIZE_2);
	WriteBE16(p, SDP_UUID16_HFP_AG);
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
		TRACE_HFP("SDP send() failed\n");
		close(sock);
		return false;
	}

	uint8 recvBuf[1024];
	ssize_t received = recv(sock, recvBuf, sizeof(recvBuf), 0);
	close(sock);

	if (received < SDP_PDU_HEADER_SIZE) {
		TRACE_HFP("SDP response too short\n");
		return false;
	}

	if (recvBuf[0] != SDP_SERVICE_SEARCH_ATTR_RSP) {
		TRACE_HFP("Unexpected SDP PDU: 0x%02X\n", recvBuf[0]);
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
		return false;
	}

	/* Walk service records */
	const uint8* rec = outerSeq.data;
	uint32 recRemaining = outerSeq.dataLen;

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

	return outChannel != 0;
}


} /* namespace Bluetooth */
