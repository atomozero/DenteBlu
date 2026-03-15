/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * SdpServicesWindow — queries a remote Bluetooth device for SDP service
 * records and displays them in an outline list.  The SDP protocol logic
 * is adapted from src/tests/kits/bluetooth/bt_sdp_query.cpp.
 */

#include "SdpServicesWindow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <Button.h>
#include <Catalog.h>
#include <LayoutBuilder.h>
#include <Messenger.h>
#include <StringItem.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/L2CAP/btL2CAP.h>
#include <bluetooth/sdp.h>

#include <bluetoothserver_p.h>
#include <CommandManager.h>
#include <l2cap.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "SDP Services"

static const uint32 kMsgQueryComplete = 'SdQC';
static const uint32 kMsgQueryFailed   = 'SdQF';
static const uint32 kMsgQueryProgress = 'SdQP';
static const uint32 kMsgClose         = 'SdCl';


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
// Data Element parser (SDP TLV)
// ---------------------------------------------------------------------------

struct DataElement {
	uint8		type;
	uint32		dataLen;
	const uint8* data;
	uint32		totalLen;
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


// ---------------------------------------------------------------------------
// UUID formatting for GUI display
// ---------------------------------------------------------------------------

static BString
FormatUuid(const uint8* data, uint32 len)
{
	BString result;
	if (len == 2) {
		uint16 uuid16 = ReadBE16(data);
		const char* name = Uuid16Name(uuid16);
		if (name != NULL)
			result.SetToFormat("%s (0x%04X)", name, uuid16);
		else
			result.SetToFormat("0x%04X", uuid16);
	} else if (len == 4) {
		result.SetToFormat("0x%08X", (unsigned)ReadBE32(data));
	} else if (len == 16) {
		result.SetToFormat(
			"%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
			"%02X%02X%02X%02X%02X%02X",
			data[0], data[1], data[2], data[3],
			data[4], data[5], data[6], data[7],
			data[8], data[9], data[10], data[11],
			data[12], data[13], data[14], data[15]);
	}
	return result;
}


// ---------------------------------------------------------------------------
// Structured formatters for SDP attributes
// ---------------------------------------------------------------------------

static BString
FormatServiceClassIds(const uint8* data, uint32 len)
{
	BString result;
	uint32 off = 0;
	while (off < len) {
		DataElement de;
		if (!ParseDataElement(data + off, len - off, de))
			break;
		if (de.type == SDP_DE_UUID) {
			if (result.Length() > 0)
				result.Append(", ");
			result.Append(FormatUuid(de.data, de.dataLen));
		}
		off += de.totalLen;
	}
	return result;
}


static BString
FormatProtocolEntry(const uint8* innerData, uint32 innerLen)
{
	BString result;
	uint32 ioff = 0;

	DataElement proto;
	if (ioff >= innerLen
		|| !ParseDataElement(innerData + ioff, innerLen - ioff, proto))
		return result;

	if (proto.type == SDP_DE_UUID) {
		result = FormatUuid(proto.data, proto.dataLen);

		ioff += proto.totalLen;
		uint16 protoUuid = 0;
		if (proto.dataLen == 2)
			protoUuid = ReadBE16(proto.data);

		if (ioff < innerLen) {
			DataElement param;
			if (ParseDataElement(innerData + ioff, innerLen - ioff, param)
				&& param.type == SDP_DE_UINT) {
				uint32 val = 0;
				if (param.dataLen == 1)
					val = param.data[0];
				else if (param.dataLen == 2)
					val = ReadBE16(param.data);
				else if (param.dataLen == 4)
					val = ReadBE32(param.data);

				BString paramStr;
				if (protoUuid == 0x0100)
					paramStr.SetToFormat(" \xE2\x80\x94 PSM %u", (unsigned)val);
				else if (protoUuid == 0x0003)
					paramStr.SetToFormat(" \xE2\x80\x94 Channel %u",
						(unsigned)val);
				else
					paramStr.SetToFormat(" \xE2\x80\x94 param %u",
						(unsigned)val);
				result.Append(paramStr);

				ioff += param.totalLen;
				if (ioff < innerLen) {
					DataElement param2;
					if (ParseDataElement(innerData + ioff, innerLen - ioff,
							param2)
						&& param2.type == SDP_DE_UINT) {
						uint32 v = 0;
						if (param2.dataLen == 2)
							v = ReadBE16(param2.data);
						else if (param2.dataLen == 1)
							v = param2.data[0];
						BString verStr;
						verStr.SetToFormat(" v%u.%u", v >> 8, v & 0xFF);
						result.Append(verStr);
					}
				}
			}
		}
	}
	return result;
}


static BString
FormatProfileEntry(const uint8* innerData, uint32 innerLen)
{
	BString result;
	uint32 ioff = 0;

	DataElement uuid;
	if (ioff >= innerLen
		|| !ParseDataElement(innerData + ioff, innerLen - ioff, uuid)
		|| uuid.type != SDP_DE_UUID)
		return result;

	result = FormatUuid(uuid.data, uuid.dataLen);
	ioff += uuid.totalLen;

	if (ioff < innerLen) {
		DataElement ver;
		if (ParseDataElement(innerData + ioff, innerLen - ioff, ver)
			&& ver.type == SDP_DE_UINT && ver.dataLen == 2) {
			uint16 v = ReadBE16(ver.data);
			BString verStr;
			verStr.SetToFormat(" v%u.%u", v >> 8, v & 0xFF);
			result.Append(verStr);
		}
	}
	return result;
}


// ---------------------------------------------------------------------------
// SdpServicesWindow
// ---------------------------------------------------------------------------

SdpServicesWindow::SdpServicesWindow(const bdaddr_t& address,
	const char* deviceName)
	:
	BWindow(BRect(200, 200, 650, 550),
		B_TRANSLATE("SDP Services"),
		B_TITLED_WINDOW,
		B_AUTO_UPDATE_SIZE_LIMITS | B_ASYNCHRONOUS_CONTROLS),
	fDeviceName(deviceName),
	fQueryThread(-1),
	fHciError(0)
{
	bdaddrUtils::Copy(fAddress, address);

	FILE* f = fopen("/tmp/sdp_query.log", "a");
	if (f != NULL) {
		fprintf(f, "SdpServicesWindow created for %s\n", deviceName);
		fclose(f);
	}

	BString title;
	title.SetToFormat(B_TRANSLATE("Services on %s"), deviceName);
	SetTitle(title.String());

	fStatusBar = new BStatusBar("status");
	fStatusBar->SetText(B_TRANSLATE("Connecting" B_UTF8_ELLIPSIS));

	fServiceList = new BOutlineListView("ServiceList",
		B_SINGLE_SELECTION_LIST);
	fScrollView = new BScrollView("ScrollView", fServiceList, 0, false, true);

	BButton* closeButton = new BButton("close", B_TRANSLATE("Close"),
		new BMessage(kMsgClose));

	BLayoutBuilder::Group<>(this, B_VERTICAL, 5)
		.SetInsets(B_USE_WINDOW_SPACING)
		.Add(fStatusBar)
		.Add(fScrollView, 1.0)
		.AddGroup(B_HORIZONTAL)
			.AddGlue()
			.Add(closeButton)
		.End()
	.End();

	fQueryThread = spawn_thread(_QueryThreadEntry, "sdp_query",
		B_NORMAL_PRIORITY, this);
	if (fQueryThread >= 0)
		resume_thread(fQueryThread);
}


SdpServicesWindow::~SdpServicesWindow()
{
	// Clean up list items
	for (int32 i = fServiceList->FullListCountItems() - 1; i >= 0; i--)
		delete fServiceList->FullListItemAt(i);
}


bool
SdpServicesWindow::QuitRequested()
{
	return true;
}


void
SdpServicesWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgQueryProgress:
		{
			const char* text = NULL;
			if (message->FindString("text", &text) == B_OK)
				fStatusBar->SetText(text);
			break;
		}

		case kMsgQueryFailed:
		{
			const char* text = NULL;
			if (message->FindString("text", &text) == B_OK)
				fStatusBar->SetText(text);
			break;
		}

		case kMsgQueryComplete:
			_PopulateResults(message);
			break;

		case kMsgClose:
			PostMessage(B_QUIT_REQUESTED);
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


// ---------------------------------------------------------------------------
// Background query thread
// ---------------------------------------------------------------------------

/*static*/ int32
SdpServicesWindow::_QueryThreadEntry(void* data)
{
	static_cast<SdpServicesWindow*>(data)->_QueryThread();
	return 0;
}


void
SdpServicesWindow::_QueryThread()
{
	FILE* logFile = fopen("/tmp/sdp_query.log", "a");
	if (logFile == NULL)
		logFile = stderr;

#define SDP_LOG(fmt, ...) do { \
	fprintf(logFile, fmt "\n", ##__VA_ARGS__); \
	fflush(logFile); \
} while (0)

	SDP_LOG("=== SDP query started for %s ===", fDeviceName.String());

	// Step 1: establish ACL connection
	BMessage progress(kMsgQueryProgress);
	progress.AddString("text",
		B_TRANSLATE("Connecting" B_UTF8_ELLIPSIS));
	PostMessage(&progress);

	SDP_LOG("Calling _EnsureAclConnection...");
	status_t status = _EnsureAclConnection();
	SDP_LOG("_EnsureAclConnection returned %s (0x%08x), hci_error=%u",
		strerror(status), (int)status, fHciError);
	if (status != B_OK) {
		BMessage fail(kMsgQueryFailed);
		BString errText;
		if (fHciError != 0) {
			const char* hciErrName = BluetoothError(fHciError);
			errText.SetToFormat(
				B_TRANSLATE("Connection failed: %s (HCI 0x%02X)"),
				hciErrName, fHciError);
		} else {
			errText.SetToFormat(
				B_TRANSLATE("Connection failed: %s"), strerror(status));
		}
		fail.AddString("text", errText.String());
		PostMessage(&fail);
		if (logFile != stderr) fclose(logFile);
		return;
	}

	// Step 2: open L2CAP socket to SDP (PSM 1)
	progress.MakeEmpty();
	progress.what = kMsgQueryProgress;
	progress.AddString("text",
		B_TRANSLATE("Querying services" B_UTF8_ELLIPSIS));
	PostMessage(&progress);

	int sock = socket(PF_BLUETOOTH, SOCK_STREAM, BLUETOOTH_PROTO_L2CAP);
	SDP_LOG("socket() returned %d (errno %d: %s)", sock, errno,
		strerror(errno));
	if (sock < 0) {
		BMessage fail(kMsgQueryFailed);
		fail.AddString("text",
			B_TRANSLATE("Failed to create L2CAP socket"));
		PostMessage(&fail);
		if (logFile != stderr) fclose(logFile);
		return;
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
	memcpy(&addr.l2cap_bdaddr, &fAddress, sizeof(bdaddr_t));

	SDP_LOG("Connecting L2CAP to PSM 1...");
	if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		SDP_LOG("connect() failed: %s (errno %d)", strerror(errno), errno);
		close(sock);
		BMessage fail(kMsgQueryFailed);
		fail.AddString("text",
			B_TRANSLATE("L2CAP connect failed"));
		PostMessage(&fail);
		if (logFile != stderr) fclose(logFile);
		return;
	}
	SDP_LOG("L2CAP connected OK");

	// Step 3: do the SDP query
	uint8* rawBuf = NULL;
	uint32 rawLen = 0;
	bool ok = _DoSdpQuery(sock, &rawBuf, &rawLen);
	SDP_LOG("_DoSdpQuery returned %s, rawLen=%u",
		ok ? "true" : "false", rawLen);
	close(sock);

	if (!ok || rawBuf == NULL) {
		free(rawBuf);
		BMessage fail(kMsgQueryFailed);
		fail.AddString("text",
			B_TRANSLATE("SDP query failed"));
		PostMessage(&fail);
		if (logFile != stderr) fclose(logFile);
		return;
	}

	// Step 4: parse and post results
	SDP_LOG("Parsing %u bytes of results...", rawLen);
	_ParseAndPost(rawBuf, rawLen);
	free(rawBuf);

	SDP_LOG("=== SDP query finished ===");
	if (logFile != stderr) fclose(logFile);

#undef SDP_LOG
}


// ---------------------------------------------------------------------------
// ACL connection via BluetoothServer
// ---------------------------------------------------------------------------

status_t
SdpServicesWindow::_EnsureAclConnection()
{
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid())
		return B_ERROR;

	// Acquire local HCI device
	BMessage acquire(BT_MSG_ACQUIRE_LOCAL_DEVICE);
	BMessage acquireReply;
	if (messenger.SendMessage(&acquire, &acquireReply, B_INFINITE_TIMEOUT,
			5000000LL) != B_OK)
		return B_ERROR;

	hci_id hid;
	if (acquireReply.FindInt32("hci_id", &hid) != B_OK || hid < 0)
		return B_DEV_NOT_READY;

	// Cancel any ongoing Inquiry first — the HCI controller rejects
	// Create Connection with "Command Disallowed" while Inquiry is active.
	{
		size_t cancelSize;
		void* cancelCmd = buildInquiryCancel(&cancelSize);
		if (cancelCmd != NULL) {
			BMessage cancelReq(BT_MSG_HANDLE_SIMPLE_REQUEST);
			BMessage cancelReply;
			cancelReq.AddInt32("hci_id", hid);
			cancelReq.AddData("raw command", B_ANY_TYPE,
				cancelCmd, cancelSize);
			cancelReq.AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
			cancelReq.AddInt16("opcodeExpected",
				PACK_OPCODE(OGF_LINK_CONTROL, OCF_INQUIRY_CANCEL));
			// Don't care if this fails (no inquiry may be running)
			messenger.SendMessage(&cancelReq, &cancelReply,
				B_INFINITE_TIMEOUT, 3000000LL);
			free(cancelCmd);
		}
	}

	// Build HCI Create Connection command
	BluetoothCommand<typed_command(hci_cp_create_conn)>
		createConn(OGF_LINK_CONTROL, OCF_CREATE_CONN);

	bdaddrUtils::Copy(createConn->bdaddr, fAddress);
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

	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 30000000LL);

	if (result == B_TIMED_OUT)
		return B_TIMED_OUT;
	if (result != B_OK)
		return result;

	int8 btStatus = BT_ERROR;
	reply.FindInt8("status", &btStatus);

	// 0x0B = Connection Already Exists — that's fine
	if (btStatus != BT_OK && (uint8)btStatus != 0x0B) {
		fHciError = (uint8)btStatus;
		return B_ERROR;
	}

	return B_OK;
}


// ---------------------------------------------------------------------------
// SDP query with continuation
// ---------------------------------------------------------------------------

bool
SdpServicesWindow::_DoSdpQuery(int sock, uint8** outBuf, uint32* outLen)
{
	uint8 sendBuf[64];
	uint16 transId = 0x0001;

	uint8* accumBuf = NULL;
	uint32 accumLen = 0;
	uint8 contState[17];
	uint8 contLen = 0;

	for (;;) {
		uint8* p = sendBuf;
		p[0] = SDP_SERVICE_SEARCH_ATTR_REQ;
		WriteBE16(p + 1, transId);
		p += SDP_PDU_HEADER_SIZE;

		// ServiceSearchPattern: Public Browse Root (0x1002)
		*p++ = SDP_DE_HEADER(SDP_DE_SEQUENCE, SDP_DE_SIZE_NEXT1);
		*p++ = 3;
		*p++ = SDP_DE_HEADER(SDP_DE_UUID, SDP_DE_SIZE_2);
		WriteBE16(p, SDP_UUID16_PUBLIC_BROWSE_ROOT);
		p += 2;

		// MaximumAttributeByteCount
		WriteBE16(p, 0xFFFF);
		p += 2;

		// AttributeIDList: 0x0000-0xFFFF (all attributes)
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

		ssize_t sent = send(sock, sendBuf, sendLen, 0);
		if (sent < 0) {
			free(accumBuf);
			return false;
		}

		uint8 recvBuf[4096];
		ssize_t received = recv(sock, recvBuf, sizeof(recvBuf), 0);
		if (received < SDP_PDU_HEADER_SIZE) {
			free(accumBuf);
			return false;
		}

		uint8 pduId = recvBuf[0];

		if (pduId == SDP_ERROR_RSP) {
			free(accumBuf);
			return false;
		}

		if (pduId != SDP_SERVICE_SEARCH_ATTR_RSP) {
			free(accumBuf);
			return false;
		}

		const uint8* rp = recvBuf + SDP_PDU_HEADER_SIZE;
		uint32 rspRemaining = (uint32)received - SDP_PDU_HEADER_SIZE;

		if (rspRemaining < 2) {
			free(accumBuf);
			return false;
		}

		uint16 attrListByteCount = ReadBE16(rp);
		rp += 2;
		rspRemaining -= 2;

		if (attrListByteCount > rspRemaining) {
			free(accumBuf);
			return false;
		}

		accumBuf = (uint8*)realloc(accumBuf, accumLen + attrListByteCount);
		if (accumBuf == NULL)
			return false;
		memcpy(accumBuf + accumLen, rp, attrListByteCount);
		accumLen += attrListByteCount;

		rp += attrListByteCount;
		rspRemaining -= attrListByteCount;

		if (rspRemaining < 1) {
			free(accumBuf);
			return false;
		}
		contLen = *rp++;
		rspRemaining--;

		if (contLen > 16) {
			free(accumBuf);
			return false;
		}

		if (contLen > 0) {
			if (rspRemaining < contLen) {
				free(accumBuf);
				return false;
			}
			memcpy(contState, rp, contLen);
			transId++;
			continue;
		}

		break;
	}

	*outBuf = accumBuf;
	*outLen = accumLen;
	return true;
}


// ---------------------------------------------------------------------------
// Parse raw SDP data and post structured results to the window
// ---------------------------------------------------------------------------

void
SdpServicesWindow::_ParseAndPost(const uint8* data, uint32 len)
{
	DataElement outerSeq;
	if (!ParseDataElement(data, len, outerSeq)
		|| (outerSeq.type != SDP_DE_SEQUENCE
			&& outerSeq.type != SDP_DE_ALTERNATIVE)) {
		BMessage fail(kMsgQueryFailed);
		fail.AddString("text", B_TRANSLATE("Failed to parse SDP response"));
		PostMessage(&fail);
		return;
	}

	BMessage complete(kMsgQueryComplete);
	int serviceCount = 0;

	const uint8* rec = outerSeq.data;
	uint32 recRemaining = outerSeq.dataLen;

	while (recRemaining > 0) {
		DataElement recordDe;
		if (!ParseDataElement(rec, recRemaining, recordDe))
			break;

		if (recordDe.type == SDP_DE_SEQUENCE) {
			serviceCount++;

			BString serviceName;
			BString serviceClasses;
			BString protocols;
			BString profiles;

			// Walk attribute-id/value pairs
			uint32 off = 0;
			const uint8* rdata = recordDe.data;
			uint32 rlen = recordDe.dataLen;

			while (off + 1 < rlen) {
				DataElement attrIdDe;
				if (!ParseDataElement(rdata + off, rlen - off, attrIdDe))
					break;
				if (attrIdDe.type != SDP_DE_UINT || attrIdDe.dataLen != 2) {
					off += attrIdDe.totalLen;
					continue;
				}
				uint16 attrId = ReadBE16(attrIdDe.data);
				off += attrIdDe.totalLen;

				DataElement value;
				if (!ParseDataElement(rdata + off, rlen - off, value))
					break;

				switch (attrId) {
					case SDP_ATTR_SERVICE_CLASS_ID_LIST:
						if (value.type == SDP_DE_SEQUENCE
							|| value.type == SDP_DE_ALTERNATIVE)
							serviceClasses = FormatServiceClassIds(
								value.data, value.dataLen);
						break;

					case SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST:
						if (value.type == SDP_DE_SEQUENCE
							|| value.type == SDP_DE_ALTERNATIVE) {
							// Walk protocol stacks
							uint32 poff = 0;
							while (poff < value.dataLen) {
								DataElement stack;
								if (!ParseDataElement(value.data + poff,
										value.dataLen - poff, stack))
									break;
								if (stack.type == SDP_DE_SEQUENCE
									|| stack.type == SDP_DE_ALTERNATIVE) {
									if (protocols.Length() > 0)
										protocols.Append("; ");
									protocols.Append(FormatProtocolEntry(
										stack.data, stack.dataLen));
								}
								poff += stack.totalLen;
							}
						}
						break;

					case SDP_ATTR_PROFILE_DESCRIPTOR_LIST:
						if (value.type == SDP_DE_SEQUENCE) {
							uint32 poff = 0;
							while (poff < value.dataLen) {
								DataElement stack;
								if (!ParseDataElement(value.data + poff,
										value.dataLen - poff, stack))
									break;
								if (stack.type == SDP_DE_SEQUENCE) {
									if (profiles.Length() > 0)
										profiles.Append("; ");
									profiles.Append(FormatProfileEntry(
										stack.data, stack.dataLen));
								}
								poff += stack.totalLen;
							}
						}
						break;

					case SDP_ATTR_SERVICE_NAME:
						if (value.type == SDP_DE_STRING)
							serviceName.SetTo((const char*)value.data,
								value.dataLen);
						break;

					default:
						break;
				}
				off += value.totalLen;
			}

			// Build a service title
			BString title;
			if (serviceName.Length() > 0)
				title = serviceName;
			else if (serviceClasses.Length() > 0)
				title = serviceClasses;
			else
				title.SetToFormat("Service #%d", serviceCount);

			complete.AddString("title", title.String());
			complete.AddString("classes", serviceClasses.String());
			complete.AddString("protocols", protocols.String());
			complete.AddString("profiles", profiles.String());
			complete.AddString("name", serviceName.String());
		}

		rec += recordDe.totalLen;
		recRemaining -= recordDe.totalLen;
	}

	complete.AddInt32("count", serviceCount);
	PostMessage(&complete);
}


// ---------------------------------------------------------------------------
// Populate the outline list from query results
// ---------------------------------------------------------------------------

void
SdpServicesWindow::_PopulateResults(BMessage* message)
{
	int32 count = 0;
	message->FindInt32("count", &count);

	if (count == 0) {
		fStatusBar->SetText(B_TRANSLATE("No services found"));
		return;
	}

	for (int32 i = 0; i < count; i++) {
		const char* title = NULL;
		const char* classes = NULL;
		const char* protocols = NULL;
		const char* profiles = NULL;
		const char* name = NULL;

		message->FindString("title", i, &title);
		message->FindString("classes", i, &classes);
		message->FindString("protocols", i, &protocols);
		message->FindString("profiles", i, &profiles);
		message->FindString("name", i, &name);

		// Level 0: service title
		BStringItem* serviceItem = new BStringItem(
			title != NULL ? title : "Unknown");
		fServiceList->AddItem(serviceItem);

		// Level 1 sub-items
		if (classes != NULL && classes[0] != '\0') {
			BString classStr;
			classStr.SetToFormat(B_TRANSLATE("Classes: %s"), classes);
			fServiceList->AddUnder(new BStringItem(classStr.String()),
				serviceItem);
		}

		if (protocols != NULL && protocols[0] != '\0') {
			BString protoStr;
			protoStr.SetToFormat(B_TRANSLATE("Protocols: %s"), protocols);
			fServiceList->AddUnder(new BStringItem(protoStr.String()),
				serviceItem);
		}

		if (profiles != NULL && profiles[0] != '\0') {
			BString profStr;
			profStr.SetToFormat(B_TRANSLATE("Profiles: %s"), profiles);
			fServiceList->AddUnder(new BStringItem(profStr.String()),
				serviceItem);
		}

		if (name != NULL && name[0] != '\0' && title != NULL
			&& strcmp(name, title) != 0) {
			BString nameStr;
			nameStr.SetToFormat(B_TRANSLATE("Name: %s"), name);
			fServiceList->AddUnder(new BStringItem(nameStr.String()),
				serviceItem);
		}
	}

	BString statusText;
	statusText.SetToFormat(B_TRANSLATE("%d service(s) found"), (int)count);
	fStatusBar->SetText(statusText.String());
}
