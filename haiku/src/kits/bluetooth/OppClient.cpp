/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * OppClient — Object Push Profile client implementation.
 */

#include <bluetooth/OppClient.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <new>
#include <sys/socket.h>
#include <sys/stat.h>
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


#define TRACE_OPP(fmt, ...) \
	fprintf(stderr, "OPP: " fmt, ##__VA_ARGS__)

/* OPP service UUID */
#define SDP_UUID16_OBEX_OBJECT_PUSH		0x1105


/* =========================================================================
 * SDP Data Element parser (shared pattern with SppClient/PbapClient)
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
 * OppClient implementation
 * ========================================================================= */

namespace Bluetooth {


OppClient::OppClient()
	:
	fRfcomm(NULL),
	fObex(NULL),
	fDlci(0)
{
}


OppClient::~OppClient()
{
	Disconnect();
}


status_t
OppClient::Connect(const bdaddr_t& address, uint8 rfcommChannel)
{
	if (fObex != NULL)
		return B_BUSY;

	/* Step 1: Ensure ACL connection */
	TRACE_OPP("Establishing ACL connection...\n");
	if (!_EnsureAclConnection(address)) {
		TRACE_OPP("Failed to establish ACL connection\n");
		return B_ERROR;
	}

	/* Step 2: SDP query for OPP channel */
	if (rfcommChannel == 0) {
		TRACE_OPP("Querying SDP for OPP channel...\n");
		if (!_QuerySdpForOpp(address, rfcommChannel)) {
			TRACE_OPP("SDP query failed — no OPP service found\n");
			return B_NAME_NOT_FOUND;
		}
		TRACE_OPP("SDP found OPP RFCOMM channel %u\n", rfcommChannel);
	}

	/* Step 3: RFCOMM session */
	fRfcomm = new(std::nothrow) RfcommSession();
	if (fRfcomm == NULL)
		return B_NO_MEMORY;

	status_t result = fRfcomm->Connect(address);
	if (result != B_OK) {
		TRACE_OPP("RFCOMM connect failed: %s\n", strerror(result));
		delete fRfcomm;
		fRfcomm = NULL;
		return result;
	}

	result = fRfcomm->OpenChannel(rfcommChannel);
	if (result != B_OK) {
		TRACE_OPP("RFCOMM channel open failed: %s\n", strerror(result));
		fRfcomm->Disconnect();
		delete fRfcomm;
		fRfcomm = NULL;
		return result;
	}

	fDlci = RFCOMM_DLCI(1, rfcommChannel);
	TRACE_OPP("RFCOMM channel %u open (DLCI %u)\n", rfcommChannel, fDlci);

	/* Step 4: OBEX Connect (no Target UUID for OPP) */
	fObex = new(std::nothrow) ObexClient(fRfcomm, fDlci);
	if (fObex == NULL) {
		fRfcomm->Disconnect();
		delete fRfcomm;
		fRfcomm = NULL;
		return B_NO_MEMORY;
	}

	result = fObex->Connect(NULL, 0);
	if (result != B_OK) {
		TRACE_OPP("OBEX Connect failed: %s\n", strerror(result));
		delete fObex;
		fObex = NULL;
		fRfcomm->Disconnect();
		delete fRfcomm;
		fRfcomm = NULL;
		return result;
	}

	TRACE_OPP("OPP session established\n");
	return B_OK;
}


void
OppClient::Disconnect()
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

	fDlci = 0;
}


bool
OppClient::IsConnected() const
{
	return fObex != NULL && fObex->IsConnected();
}


status_t
OppClient::PushFile(const char* filePath)
{
	if (!IsConnected())
		return B_NOT_ALLOWED;

	/* Open and read the file */
	int fd = open(filePath, O_RDONLY);
	if (fd < 0) {
		TRACE_OPP("Cannot open file %s: %s\n", filePath, strerror(errno));
		return B_ENTRY_NOT_FOUND;
	}

	struct stat st;
	if (fstat(fd, &st) < 0) {
		close(fd);
		return B_ERROR;
	}

	size_t fileSize = (size_t)st.st_size;
	if (fileSize == 0) {
		close(fd);
		return B_BAD_VALUE;
	}

	uint8* fileData = (uint8*)malloc(fileSize);
	if (fileData == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}

	ssize_t bytesRead = read(fd, fileData, fileSize);
	close(fd);

	if (bytesRead < (ssize_t)fileSize) {
		free(fileData);
		return B_IO_ERROR;
	}

	/* Extract filename from path */
	const char* name = strrchr(filePath, '/');
	name = (name != NULL) ? name + 1 : filePath;

	/* Determine MIME type from extension.
	 * If unrecognised, leave NULL — the Type header is optional in
	 * OBEX and some phones reject "application/octet-stream". */
	const char* mimeType = NULL;
	const char* ext = strrchr(name, '.');
	if (ext != NULL) {
		if (strcasecmp(ext, ".vcf") == 0)
			mimeType = "text/x-vcard";
		else if (strcasecmp(ext, ".txt") == 0
			|| strcasecmp(ext, ".text") == 0
			|| strcasecmp(ext, ".log") == 0
			|| strcasecmp(ext, ".csv") == 0)
			mimeType = "text/plain";
		else if (strcasecmp(ext, ".jpg") == 0
			|| strcasecmp(ext, ".jpeg") == 0)
			mimeType = "image/jpeg";
		else if (strcasecmp(ext, ".png") == 0)
			mimeType = "image/png";
		else if (strcasecmp(ext, ".gif") == 0)
			mimeType = "image/gif";
		else if (strcasecmp(ext, ".pdf") == 0)
			mimeType = "application/pdf";
		else if (strcasecmp(ext, ".mp3") == 0)
			mimeType = "audio/mpeg";
		else if (strcasecmp(ext, ".wav") == 0)
			mimeType = "audio/wav";
		else if (strcasecmp(ext, ".mp4") == 0)
			mimeType = "video/mp4";
	}

	TRACE_OPP("Pushing file: %s (%zu bytes, type=%s)\n",
		name, fileSize, mimeType ? mimeType : "(none)");

	status_t result = fObex->Put(name, mimeType, fileData, fileSize);
	free(fileData);

	return result;
}


status_t
OppClient::PushData(const char* name, const char* mimeType,
	const uint8* data, size_t dataLen)
{
	if (!IsConnected())
		return B_NOT_ALLOWED;

	TRACE_OPP("Pushing data: %s (%zu bytes, type=%s)\n",
		name, dataLen, mimeType ? mimeType : "(none)");

	return fObex->Put(name, mimeType, data, dataLen);
}


/* =========================================================================
 * ACL connection via BluetoothServer
 * (Same pattern as SppClient::_EnsureAclConnection)
 * ========================================================================= */

bool
OppClient::_EnsureAclConnection(const bdaddr_t& remote)
{
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		TRACE_OPP("Cannot reach BluetoothServer\n");
		return false;
	}

	BMessage acquire(BT_MSG_ACQUIRE_LOCAL_DEVICE);
	BMessage acquireReply;
	if (messenger.SendMessage(&acquire, &acquireReply, B_INFINITE_TIMEOUT,
			5000000LL) != B_OK) {
		TRACE_OPP("Failed to query local device\n");
		return false;
	}

	hci_id hid;
	if (acquireReply.FindInt32("hci_id", &hid) != B_OK || hid < 0) {
		TRACE_OPP("No local Bluetooth device found\n");
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

	TRACE_OPP("Sending HCI Create Connection (timeout 30s)...\n");

	int8 btStatus = BT_ERROR;
	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 30000000LL);

	if (result == B_TIMED_OUT) {
		TRACE_OPP("ACL connection timed out\n");
		return false;
	}
	if (result != B_OK) {
		TRACE_OPP("SendMessage failed: %s\n", strerror(result));
		return false;
	}

	reply.FindInt8("status", &btStatus);
	if (btStatus == 0x0B) {
		TRACE_OPP("ACL already exists, skipping auth/encrypt\n");
		return true;
	} else if (btStatus != BT_OK) {
		TRACE_OPP("ACL connection failed (HCI status 0x%02X)\n",
			(unsigned)(uint8)btStatus);
		return false;
	}

	int16 handle = -1;
	reply.FindInt16("handle", &handle);
	TRACE_OPP("ACL connection established (handle=0x%04X)\n",
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
			TRACE_OPP("Authentication failed (status 0x%02X)\n",
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
			TRACE_OPP("Encryption failed (status 0x%02X)\n",
				(unsigned)(uint8)encStatus);
			return false;
		}
	}

	return true;
}


/* =========================================================================
 * SDP query for OPP RFCOMM channel
 * ========================================================================= */

bool
OppClient::_QuerySdpForOpp(const bdaddr_t& remote, uint8& outChannel)
{
	outChannel = 0;

	int sock = socket(PF_BLUETOOTH, SOCK_STREAM, BLUETOOTH_PROTO_L2CAP);
	if (sock < 0) {
		TRACE_OPP("SDP socket() failed: %s\n", strerror(errno));
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
		TRACE_OPP("SDP connect() failed: %s\n", strerror(errno));
		close(sock);
		return false;
	}

	/* Build ServiceSearchAttributeRequest for UUID 0x1105 (OPP) */
	uint8 sendBuf[64];
	uint8* p = sendBuf;

	p[0] = SDP_SERVICE_SEARCH_ATTR_REQ;
	WriteBE16(p + 1, 0x0001);
	p += SDP_PDU_HEADER_SIZE;

	/* ServiceSearchPattern: Sequence { UUID16(0x1105) } */
	*p++ = SDP_DE_HEADER(SDP_DE_SEQUENCE, SDP_DE_SIZE_NEXT1);
	*p++ = 3;
	*p++ = SDP_DE_HEADER(SDP_DE_UUID, SDP_DE_SIZE_2);
	WriteBE16(p, SDP_UUID16_OBEX_OBJECT_PUSH);
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
		TRACE_OPP("SDP send() failed\n");
		close(sock);
		return false;
	}

	uint8 recvBuf[1024];
	ssize_t received = recv(sock, recvBuf, sizeof(recvBuf), 0);
	close(sock);

	if (received < SDP_PDU_HEADER_SIZE) {
		TRACE_OPP("SDP response too short\n");
		return false;
	}

	if (recvBuf[0] != SDP_SERVICE_SEARCH_ATTR_RSP) {
		TRACE_OPP("Unexpected SDP PDU: 0x%02X\n", recvBuf[0]);
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
