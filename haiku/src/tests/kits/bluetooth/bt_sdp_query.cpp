/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_sdp_query — Query remote Bluetooth device for SDP service records.
 *
 * Opens an L2CAP connection to PSM 1 (SDP) and sends a
 * ServiceSearchAttributeRequest for the Public Browse Root (0x1002),
 * then decodes and prints every service record found.
 */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <Application.h>
#include <Messenger.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/L2CAP/btL2CAP.h>

#include <bluetooth/sdp.h>
#include <bluetoothserver_p.h>
#include <CommandManager.h>
#include <l2cap.h>


// ---------------------------------------------------------------------------
// Big-endian helpers
// ---------------------------------------------------------------------------

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


// ---------------------------------------------------------------------------
// Well-known UUID16 names
// ---------------------------------------------------------------------------

struct uuid16_entry {
	uint16		uuid;
	const char*	name;
};

static const uuid16_entry kUuid16Names[] = {
	{ 0x0001, "SDP" },
	{ 0x0003, "RFCOMM" },
	{ 0x0005, "TCS-BIN" },
	{ 0x0007, "ATT" },
	{ 0x0008, "OBEX" },
	{ 0x000F, "BNEP" },
	{ 0x0017, "AVCTP" },
	{ 0x0019, "AVDTP" },
	{ 0x001B, "CMTP" },
	{ 0x0100, "L2CAP" },
	{ 0x1000, "ServiceDiscoveryServer" },
	{ 0x1001, "BrowseGroupDescriptor" },
	{ 0x1002, "PublicBrowseRoot" },
	{ 0x1101, "SerialPort (SPP)" },
	{ 0x1102, "LANAccessUsingPPP" },
	{ 0x1103, "DialupNetworking" },
	{ 0x1104, "IrMCSync" },
	{ 0x1105, "OBEXObjectPush" },
	{ 0x1106, "OBEXFileTransfer" },
	{ 0x1107, "IrMCSyncCommand" },
	{ 0x1108, "Headset" },
	{ 0x110A, "AudioSource (A2DP)" },
	{ 0x110B, "AudioSink (A2DP)" },
	{ 0x110C, "AVRCP Target" },
	{ 0x110D, "AdvancedAudioDistribution" },
	{ 0x110E, "AVRCP Controller" },
	{ 0x110F, "AVRCP" },
	{ 0x1112, "Headset AG" },
	{ 0x1115, "PANU" },
	{ 0x1116, "NAP" },
	{ 0x1117, "GN" },
	{ 0x111E, "Handsfree" },
	{ 0x111F, "HandsfreeAG" },
	{ 0x1124, "HumanInterfaceDevice (HID)" },
	{ 0x112D, "SIM Access" },
	{ 0x112F, "PhonebookAccess (PBAP)" },
	{ 0x1130, "PhonebookAccess (PBAP-PSE)" },
	{ 0x1132, "MessageAccess (MAP)" },
	{ 0x1133, "MessageAccess (MAP-MNS)" },
	{ 0x1134, "MessageAccess (MAP-MAS)" },
	{ 0x1200, "PnPInformation" },
	{ 0x1203, "GenericAudio" },
	{ 0x1303, "VideoSource" },
	{ 0x1304, "VideoSink" },
	{ 0, NULL }
};


static const char*
Uuid16Name(uint16 uuid)
{
	for (const uuid16_entry* e = kUuid16Names; e->name != NULL; e++) {
		if (e->uuid == uuid)
			return e->name;
	}
	return NULL;
}


// ---------------------------------------------------------------------------
// Data Element parser
//
// SDP data elements are TLV with a 1-byte type/size header.  The upper 5 bits
// encode the type, the lower 3 bits encode the size descriptor.
// ---------------------------------------------------------------------------

struct DataElement {
	uint8	type;		// SDP_DE_*
	uint32	dataLen;	// length of value bytes
	const uint8* data;	// pointer into receive buffer
	uint32	totalLen;	// header + value
};


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
		// NIL has no payload
		dataLen = 0;
	} else if (sizeDesc <= SDP_DE_SIZE_16) {
		// Fixed sizes: 1, 2, 4, 8, 16 bytes
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


// ---------------------------------------------------------------------------
// Pretty-printers for common SDP attributes
// ---------------------------------------------------------------------------

static void
PrintUuid(const uint8* data, uint32 len)
{
	if (len == 2) {
		uint16 uuid16 = ReadBE16(data);
		const char* name = Uuid16Name(uuid16);
		if (name != NULL)
			printf("0x%04X (%s)", uuid16, name);
		else
			printf("0x%04X", uuid16);
	} else if (len == 4) {
		printf("0x%08X", (unsigned)ReadBE32(data));
	} else if (len == 16) {
		// Full 128-bit UUID: XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
		printf("%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
			"%02X%02X%02X%02X%02X%02X",
			data[0], data[1], data[2], data[3],
			data[4], data[5], data[6], data[7],
			data[8], data[9], data[10], data[11],
			data[12], data[13], data[14], data[15]);
	}
}


static void
PrintServiceClassIdList(const uint8* data, uint32 len)
{
	printf("  ServiceClassIDList:\n");
	uint32 off = 0;
	while (off < len) {
		DataElement de;
		if (!ParseDataElement(data + off, len - off, de))
			break;
		if (de.type == SDP_DE_UUID) {
			printf("    ");
			PrintUuid(de.data, de.dataLen);
			printf("\n");
		}
		off += de.totalLen;
	}
}


static void
PrintProtocolDescriptorList(const uint8* data, uint32 len)
{
	printf("  ProtocolDescriptorList:\n");

	// The list is a sequence of sequences, each describing one protocol layer.
	uint32 off = 0;
	while (off < len) {
		DataElement stack;
		if (!ParseDataElement(data + off, len - off, stack))
			break;

		if (stack.type == SDP_DE_SEQUENCE || stack.type == SDP_DE_ALTERNATIVE) {
			const uint8* inner = stack.data;
			uint32 innerLen = stack.dataLen;
			uint32 ioff = 0;

			// First element is the protocol UUID
			DataElement proto;
			if (ioff < innerLen
				&& ParseDataElement(inner + ioff, innerLen - ioff, proto)) {
				printf("    ");
				if (proto.type == SDP_DE_UUID) {
					PrintUuid(proto.data, proto.dataLen);

					// Check for L2CAP PSM or RFCOMM channel as second param
					ioff += proto.totalLen;
					uint16 protoUuid = 0;
					if (proto.dataLen == 2)
						protoUuid = ReadBE16(proto.data);

					if (ioff < innerLen) {
						DataElement param;
						if (ParseDataElement(inner + ioff, innerLen - ioff,
								param)) {
							if (param.type == SDP_DE_UINT) {
								uint32 val = 0;
								if (param.dataLen == 1)
									val = param.data[0];
								else if (param.dataLen == 2)
									val = ReadBE16(param.data);
								else if (param.dataLen == 4)
									val = ReadBE32(param.data);

								if (protoUuid == 0x0100)
									printf(" — PSM %u", (unsigned)val);
								else if (protoUuid == 0x0003)
									printf(" — Channel %u", (unsigned)val);
								else
									printf(" — param %u", (unsigned)val);
							}
							ioff += param.totalLen;

							// Third element (e.g. AVDTP version)
							if (ioff < innerLen) {
								DataElement param2;
								if (ParseDataElement(inner + ioff,
										innerLen - ioff, param2)
									&& param2.type == SDP_DE_UINT) {
									uint32 val = 0;
									if (param2.dataLen == 2)
										val = ReadBE16(param2.data);
									else if (param2.dataLen == 1)
										val = param2.data[0];
									printf(" v%u.%u", val >> 8, val & 0xFF);
								}
							}
						}
					}
				} else {
					printf("(unexpected type %u)", proto.type);
				}
				printf("\n");
			}
		}
		off += stack.totalLen;
	}
}


static void
PrintProfileDescriptorList(const uint8* data, uint32 len)
{
	printf("  ProfileDescriptorList:\n");

	uint32 off = 0;
	while (off < len) {
		DataElement stack;
		if (!ParseDataElement(data + off, len - off, stack))
			break;

		if (stack.type == SDP_DE_SEQUENCE) {
			const uint8* inner = stack.data;
			uint32 innerLen = stack.dataLen;
			uint32 ioff = 0;

			DataElement uuid;
			if (ioff < innerLen
				&& ParseDataElement(inner + ioff, innerLen - ioff, uuid)
				&& uuid.type == SDP_DE_UUID) {
				printf("    ");
				PrintUuid(uuid.data, uuid.dataLen);
				ioff += uuid.totalLen;

				// Version follows
				if (ioff < innerLen) {
					DataElement ver;
					if (ParseDataElement(inner + ioff, innerLen - ioff, ver)
						&& ver.type == SDP_DE_UINT && ver.dataLen == 2) {
						uint16 v = ReadBE16(ver.data);
						printf(" v%u.%u", v >> 8, v & 0xFF);
					}
				}
				printf("\n");
			}
		}
		off += stack.totalLen;
	}
}


static void
PrintServiceName(const uint8* data, uint32 len)
{
	printf("  ServiceName: %.*s\n", (int)len, (const char*)data);
}


// ---------------------------------------------------------------------------
// Service record printer — walks the attribute-id/value list
// ---------------------------------------------------------------------------

static int sServiceIndex = 0;

static void
PrintServiceRecord(const uint8* data, uint32 len)
{
	sServiceIndex++;
	printf("\n=== Service #%d ===\n", sServiceIndex);

	uint32 off = 0;
	while (off + 1 < len) {
		// Each entry: attribute-id (UINT16 DE) + value (DE)
		DataElement attrIdDe;
		if (!ParseDataElement(data + off, len - off, attrIdDe))
			break;
		if (attrIdDe.type != SDP_DE_UINT || attrIdDe.dataLen != 2) {
			off += attrIdDe.totalLen;
			continue;
		}
		uint16 attrId = ReadBE16(attrIdDe.data);
		off += attrIdDe.totalLen;

		DataElement value;
		if (!ParseDataElement(data + off, len - off, value))
			break;

		switch (attrId) {
			case SDP_ATTR_SERVICE_RECORD_HANDLE:
				if (value.type == SDP_DE_UINT && value.dataLen == 4) {
					printf("  RecordHandle: 0x%08X\n",
						(unsigned)ReadBE32(value.data));
				}
				break;

			case SDP_ATTR_SERVICE_CLASS_ID_LIST:
				if (value.type == SDP_DE_SEQUENCE
					|| value.type == SDP_DE_ALTERNATIVE)
					PrintServiceClassIdList(value.data, value.dataLen);
				break;

			case SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST:
				if (value.type == SDP_DE_SEQUENCE
					|| value.type == SDP_DE_ALTERNATIVE)
					PrintProtocolDescriptorList(value.data, value.dataLen);
				break;

			case SDP_ATTR_PROFILE_DESCRIPTOR_LIST:
				if (value.type == SDP_DE_SEQUENCE)
					PrintProfileDescriptorList(value.data, value.dataLen);
				break;

			case SDP_ATTR_SERVICE_NAME:
				if (value.type == SDP_DE_STRING)
					PrintServiceName(value.data, value.dataLen);
				break;

			case SDP_ATTR_BROWSE_GROUP_LIST:
				// Not critical — skip silently
				break;

			default:
				printf("  Attribute 0x%04X (type %u, %u bytes)\n",
					attrId, value.type, (unsigned)value.dataLen);
				break;
		}
		off += value.totalLen;
	}
}


// ---------------------------------------------------------------------------
// ACL connection setup via BluetoothServer
// ---------------------------------------------------------------------------

static bool
EnsureAclConnection(const bdaddr_t& remote)
{
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		fprintf(stderr, "Cannot reach BluetoothServer\n");
		return false;
	}

	// Get the first local HCI device
	printf("  Acquiring local HCI device...\n");
	fflush(stdout);

	BMessage acquire(BT_MSG_ACQUIRE_LOCAL_DEVICE);
	BMessage acquireReply;
	if (messenger.SendMessage(&acquire, &acquireReply, B_INFINITE_TIMEOUT,
			5000000LL) != B_OK) {
		fprintf(stderr, "Failed to query local device\n");
		return false;
	}

	hci_id hid;
	if (acquireReply.FindInt32("hci_id", &hid) != B_OK || hid < 0) {
		fprintf(stderr, "No local Bluetooth device found\n");
		return false;
	}
	printf("  Using HCI device %" B_PRId32 "\n", hid);
	fflush(stdout);

	// Build HCI Create Connection command
	BluetoothCommand<typed_command(hci_cp_create_conn)>
		createConn(OGF_LINK_CONTROL, OCF_CREATE_CONN);

	bdaddrUtils::Copy(createConn->bdaddr, remote);
	createConn->pkt_type = 0xCC18;  // DM1, DH1, DM3, DH3, DM5, DH5
	createConn->pscan_rep_mode = 0x01;  // R1
	createConn->pscan_mode = 0x00;
	createConn->clock_offset = 0x0000;
	createConn->role_switch = 0x01;  // allow role switch

	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	BMessage reply;

	request.AddInt32("hci_id", hid);
	request.AddData("raw command", B_ANY_TYPE,
		createConn.Data(), createConn.Size());

	// CMD_STATUS acknowledges the command was accepted
	request.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
	request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_LINK_CONTROL, OCF_CREATE_CONN));

	// Link key handling — BluetoothServer will reply NEG if no stored key,
	// which triggers SSP re-pairing
	request.AddInt16("eventExpected", HCI_EVENT_LINK_KEY_REQ);
	request.AddInt16("eventExpected", HCI_EVENT_LINK_KEY_NOTIFY);

	// SSP events — needed if re-pairing is required
	request.AddInt16("eventExpected", HCI_EVENT_IO_CAPABILITY_REQUEST);
	request.AddInt16("eventExpected", HCI_EVENT_IO_CAPABILITY_RESPONSE);
	request.AddInt16("eventExpected", HCI_EVENT_USER_CONFIRMATION_REQUEST);
	request.AddInt16("eventExpected", HCI_EVENT_SIMPLE_PAIRING_COMPLETE);

	// CONN_COMPLETE is the terminal event — ConnectionComplete() sends
	// the reply and clears the entire request from the queue
	request.AddInt16("eventExpected", HCI_EVENT_CONN_COMPLETE);

	printf("  Sending HCI Create Connection (timeout 30s)...\n");
	fflush(stdout);

	int8 btStatus = BT_ERROR;
	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 30000000LL);

	if (result == B_TIMED_OUT) {
		fprintf(stderr, "ACL connection timed out (30s)\n");
		return false;
	}
	if (result != B_OK) {
		fprintf(stderr, "SendMessage failed: %s\n", strerror(result));
		return false;
	}

	reply.FindInt8("status", &btStatus);
	if (btStatus != BT_OK) {
		fprintf(stderr, "ACL connection failed (HCI status 0x%02X)\n",
			(unsigned)(uint8)btStatus);
		return false;
	}

	int16 handle = 0;
	reply.FindInt16("handle", &handle);
	printf("  ACL connection established (handle 0x%04X)\n", (unsigned)handle);
	fflush(stdout);
	return true;
}


// ---------------------------------------------------------------------------
// SDP query with continuation support
// ---------------------------------------------------------------------------

static bool
DoSdpQuery(int sock)
{
	uint8 sendBuf[64];
	uint16 transId = 0x0001;

	// Accumulate attribute lists across continuations
	uint8* accumBuf = NULL;
	uint32 accumLen = 0;
	uint8 contState[17];  // max 16 bytes + 1 length byte
	uint8 contLen = 0;

	for (;;) {
		// Build request
		uint8* p = sendBuf;
		p[0] = SDP_SERVICE_SEARCH_ATTR_REQ;
		WriteBE16(p + 1, transId);
		p += SDP_PDU_HEADER_SIZE;

		// ServiceSearchPattern
		*p++ = SDP_DE_HEADER(SDP_DE_SEQUENCE, SDP_DE_SIZE_NEXT1);
		*p++ = 3;
		*p++ = SDP_DE_HEADER(SDP_DE_UUID, SDP_DE_SIZE_2);
		WriteBE16(p, SDP_UUID16_PUBLIC_BROWSE_ROOT);
		p += 2;

		// MaximumAttributeByteCount
		WriteBE16(p, 0xFFFF);
		p += 2;

		// AttributeIDList
		*p++ = SDP_DE_HEADER(SDP_DE_SEQUENCE, SDP_DE_SIZE_NEXT1);
		*p++ = 5;
		*p++ = SDP_DE_HEADER(SDP_DE_UINT, SDP_DE_SIZE_4);
		WriteBE32(p, 0x0000FFFF);
		p += 4;

		// ContinuationState
		*p++ = contLen;
		if (contLen > 0) {
			memcpy(p, contState, contLen);
			p += contLen;
		}

		uint16 paramLen = (uint16)(p - sendBuf - SDP_PDU_HEADER_SIZE);
		WriteBE16(sendBuf + 3, paramLen);
		uint32 sendLen = (uint32)(p - sendBuf);

		// Debug: hex dump of outgoing PDU
		printf("  SDP TX (%u bytes):", sendLen);
		for (uint32 i = 0; i < sendLen; i++)
			printf(" %02X", sendBuf[i]);
		printf("\n");
		fflush(stdout);

		ssize_t sent = send(sock, sendBuf, sendLen, 0);
		if (sent < 0) {
			perror("send");
			free(accumBuf);
			return false;
		}
		printf("  send() returned %zd\n", sent);
		fflush(stdout);

		// Receive response
		printf("  Waiting for SDP response (10s timeout)...\n");
		fflush(stdout);
		uint8 recvBuf[4096];
		ssize_t received = recv(sock, recvBuf, sizeof(recvBuf), 0);
		if (received < 0) {
			perror("recv");
			free(accumBuf);
			return false;
		}
		printf("  recv() returned %zd bytes\n", received);
		fflush(stdout);
		if (received < SDP_PDU_HEADER_SIZE) {
			fprintf(stderr, "Short SDP response (%zd bytes)\n", received);
			free(accumBuf);
			return false;
		}

		uint8 pduId = recvBuf[0];
		// uint16 rspTransId = ReadBE16(recvBuf + 1);
		uint16 rspParamLen = ReadBE16(recvBuf + 3);

		if (pduId == SDP_ERROR_RSP) {
			uint16 errCode = 0;
			if (rspParamLen >= 2)
				errCode = ReadBE16(recvBuf + SDP_PDU_HEADER_SIZE);
			fprintf(stderr, "SDP error response: 0x%04X\n", errCode);
			free(accumBuf);
			return false;
		}

		if (pduId != SDP_SERVICE_SEARCH_ATTR_RSP) {
			fprintf(stderr, "Unexpected PDU ID: 0x%02X\n", pduId);
			free(accumBuf);
			return false;
		}

		// Parse response: AttributeListsByteCount (2) + AttributeLists + ContinuationState
		const uint8* rp = recvBuf + SDP_PDU_HEADER_SIZE;
		uint32 rspRemaining = (uint32)received - SDP_PDU_HEADER_SIZE;

		if (rspRemaining < 2) {
			fprintf(stderr, "Truncated SDP response\n");
			free(accumBuf);
			return false;
		}

		uint16 attrListByteCount = ReadBE16(rp);
		rp += 2;
		rspRemaining -= 2;

		if (attrListByteCount > rspRemaining) {
			fprintf(stderr, "AttributeListsByteCount (%u) exceeds data (%u)\n",
				attrListByteCount, rspRemaining);
			free(accumBuf);
			return false;
		}

		// Accumulate attribute data
		accumBuf = (uint8*)realloc(accumBuf, accumLen + attrListByteCount);
		if (accumBuf == NULL) {
			fprintf(stderr, "Out of memory\n");
			return false;
		}
		memcpy(accumBuf + accumLen, rp, attrListByteCount);
		accumLen += attrListByteCount;

		rp += attrListByteCount;
		rspRemaining -= attrListByteCount;

		// Continuation state
		if (rspRemaining < 1) {
			fprintf(stderr, "Missing continuation state\n");
			free(accumBuf);
			return false;
		}
		contLen = *rp++;
		rspRemaining--;

		if (contLen > 16) {
			fprintf(stderr, "Invalid continuation length: %u\n", contLen);
			free(accumBuf);
			return false;
		}

		if (contLen > 0) {
			if (rspRemaining < contLen) {
				fprintf(stderr, "Truncated continuation state\n");
				free(accumBuf);
				return false;
			}
			memcpy(contState, rp, contLen);
			transId++;
			printf("(continuation, %u bytes so far...)\n", accumLen);
			continue;
		}

		// Done — no more continuation
		break;
	}

	printf("Received %u bytes of attribute data.\n", accumLen);

	// The accumulated data should be one big DE_SEQUENCE of service records
	DataElement outerSeq;
	if (!ParseDataElement(accumBuf, accumLen, outerSeq)
		|| (outerSeq.type != SDP_DE_SEQUENCE
			&& outerSeq.type != SDP_DE_ALTERNATIVE)) {
		fprintf(stderr, "Failed to parse outer sequence (type=%u, len=%u)\n",
			outerSeq.type, outerSeq.dataLen);
		free(accumBuf);
		return false;
	}

	// Walk each service record (each is a DE_SEQUENCE of attr-id/value pairs)
	const uint8* rec = outerSeq.data;
	uint32 recRemaining = outerSeq.dataLen;

	while (recRemaining > 0) {
		DataElement recordDe;
		if (!ParseDataElement(rec, recRemaining, recordDe))
			break;

		if (recordDe.type == SDP_DE_SEQUENCE)
			PrintServiceRecord(recordDe.data, recordDe.dataLen);

		rec += recordDe.totalLen;
		recRemaining -= recordDe.totalLen;
	}

	printf("\n%d service(s) found.\n", sServiceIndex);

	free(accumBuf);
	return true;
}


// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr,
			"Usage: %s <BD_ADDR>\n"
			"  e.g. %s 0C:7D:B0:B2:81:6A\n",
			argv[0], argv[0]);
		return 1;
	}

	bdaddr_t remote = bdaddrUtils::FromString(argv[1]);
	if (bdaddrUtils::Compare(remote, bdaddrUtils::NullAddress())) {
		fprintf(stderr, "Invalid BD_ADDR: %s\n", argv[1]);
		return 1;
	}

	// BApplication needed for BMessenger to work
	BApplication app("application/x-vnd.Haiku-bt_sdp_query");

	printf("Querying SDP services on %s ...\n", argv[1]);
	fflush(stdout);

	// Establish ACL connection via BluetoothServer
	if (!EnsureAclConnection(remote)) {
		fprintf(stderr, "Could not establish ACL connection\n");
		return 1;
	}

	// Create L2CAP socket
	int sock = socket(PF_BLUETOOTH, SOCK_STREAM, BLUETOOTH_PROTO_L2CAP);
	if (sock < 0) {
		perror("socket");
		return 1;
	}

	// Set 10-second receive timeout so we don't block forever
	struct timeval tv;
	tv.tv_sec = 10;
	tv.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	// Connect to remote SDP (PSM 1)
	struct sockaddr_l2cap addr;
	memset(&addr, 0, sizeof(addr));
	addr.l2cap_len = sizeof(addr);
	addr.l2cap_family = PF_BLUETOOTH;
	addr.l2cap_psm = L2CAP_PSM_SDP;
	memcpy(&addr.l2cap_bdaddr, &remote, sizeof(bdaddr_t));

	printf("Connecting L2CAP to PSM %d (SDP)...\n", L2CAP_PSM_SDP);
	fflush(stdout);
	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("connect");
		close(sock);
		return 1;
	}
	printf("Connected.\n");
	fflush(stdout);

	bool ok = DoSdpQuery(sock);

	close(sock);
	return ok ? 0 : 1;
}
