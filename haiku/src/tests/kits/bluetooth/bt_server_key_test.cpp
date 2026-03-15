/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * Integration test for LocalDeviceImpl key persistence handlers:
 *   LinkKeyRequested, LinkKeyNotify, LeLtkRequest
 *
 * Uses a MockHCIDelegate to capture HCI commands issued by the handlers,
 * and synthetic HCI event packets to drive HandleEvent().
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>
#include <OS.h>

#include "LocalDeviceImpl.h"
#include "BluetoothKeyStore.h"

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_command_le.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/HCI/btHCI_event_le.h>


// ============================================================
// Stubs for symbols pulled in by LocalDeviceImpl.cpp that we
// don't exercise in these tests.
// ============================================================

// BluetoothServer string helpers (used by TRACE_BT macros)
const char* BluetoothError(uint8) { return "stub"; }
const char* BluetoothEvent(uint8) { return "stub"; }
const char* BluetoothCommandOpcode(uint16) { return "stub"; }
const char* BluetoothManufacturer(uint16) { return "stub"; }
const char* BluetoothHciVersion(uint16) { return "stub"; }
const char* BluetoothLmpVersion(uint16) { return "stub"; }

// HCI accessor constructors/destructors (used by factory methods we don't call)
class HCIControllerAccessor;
class HCITransportAccessor;

HCIControllerAccessor::HCIControllerAccessor(BPath*)
	: HCIDelegate(NULL) {}
HCIControllerAccessor::~HCIControllerAccessor() {}
status_t HCIControllerAccessor::IssueCommand(raw_command, size_t)
	{ return B_ERROR; }
status_t HCIControllerAccessor::Launch() { return B_ERROR; }

HCITransportAccessor::HCITransportAccessor(BPath*)
	: HCIDelegate(NULL) {}
HCITransportAccessor::~HCITransportAccessor() {}
status_t HCITransportAccessor::IssueCommand(raw_command, size_t)
	{ return B_ERROR; }
status_t HCITransportAccessor::Launch() { return B_ERROR; }
status_t HCITransportAccessor::GattIoctl(uint32, void*, size_t)
	{ return B_ERROR; }

// ConnectionIncoming stub — provides minimal BWindow-derived implementations
// so we don't link against the real GUI code.
#include <ConnectionIncoming.h>

ConnectionIncoming::ConnectionIncoming(bdaddr_t)
	: BWindow(BRect(0, 0, 1, 1), "stub", B_TITLED_WINDOW, 0), fView(NULL) {}
ConnectionIncoming::ConnectionIncoming(RemoteDevice*)
	: BWindow(BRect(0, 0, 1, 1), "stub", B_TITLED_WINDOW, 0), fView(NULL) {}
ConnectionIncoming::~ConnectionIncoming() {}
void ConnectionIncoming::MessageReceived(BMessage*) {}
bool ConnectionIncoming::QuitRequested() { return true; }

// PincodeWindow stub
#include <PincodeWindow.h>

PincodeWindow::PincodeWindow(bdaddr_t address, hci_id hid)
	: BWindow(BRect(0, 0, 1, 1), "stub", B_TITLED_WINDOW, 0),
	  fBdaddr(address), fHid(hid),
	  fMessage(NULL), fRemoteInfo(NULL), fAcceptButton(NULL),
	  fCancelButton(NULL), fPincodeText(NULL), fIcon(NULL),
	  fMessage2(NULL), fDeviceLabel(NULL), fDeviceText(NULL),
	  fAddressLabel(NULL), fAddressText(NULL) {}
PincodeWindow::PincodeWindow(RemoteDevice*)
	: BWindow(BRect(0, 0, 1, 1), "stub", B_TITLED_WINDOW, 0),
	  fHid(-1),
	  fMessage(NULL), fRemoteInfo(NULL), fAcceptButton(NULL),
	  fCancelButton(NULL), fPincodeText(NULL), fIcon(NULL),
	  fMessage2(NULL), fDeviceLabel(NULL), fDeviceText(NULL),
	  fAddressLabel(NULL), fAddressText(NULL) {}
void PincodeWindow::MessageReceived(BMessage*) {}
bool PincodeWindow::QuitRequested() { return true; }
void PincodeWindow::SetBDaddr(BString) {}
void PincodeWindow::InitUI() {}

// Command builder stubs
void* buildAcceptConnectionRequest(bdaddr_t, uint8, size_t* s)
	{ *s = 0; return NULL; }
void* buildIoCapabilityRequestReply(bdaddr_t, uint8, uint8, uint8, size_t* s)
	{ *s = 0; return NULL; }
void* buildUserConfirmationRequestReply(bdaddr_t, size_t* s)
	{ *s = 0; return NULL; }
void* buildUserConfirmationRequestNegReply(bdaddr_t, size_t* s)
	{ *s = 0; return NULL; }
void* buildLeSetScanParameters(uint8, uint16, uint16, uint8, uint8, size_t* s)
	{ *s = 0; return NULL; }
void* buildLeSetScanEnable(uint8, uint8, size_t* s)
	{ *s = 0; return NULL; }
void* buildLeCreateConnection(uint16, uint16, uint8, uint8, bdaddr_t,
	uint8, uint16, uint16, uint16, uint16, size_t* s)
	{ *s = 0; return NULL; }


// ============================================================
// MockHCIDelegate — captures commands issued by the handlers
// ============================================================

class MockHCIDelegate : public HCIDelegate {
public:
	uint8	fLastCommand[256];
	size_t	fLastCommandSize;
	int		fCommandCount;

	MockHCIDelegate()
		: HCIDelegate(NULL)
	{
		fIdentifier = 99;
		fLastCommandSize = 0;
		fCommandCount = 0;
		memset(fLastCommand, 0, sizeof(fLastCommand));
	}

	virtual status_t IssueCommand(raw_command rc, size_t size)
	{
		if (size > sizeof(fLastCommand))
			size = sizeof(fLastCommand);
		memcpy(fLastCommand, rc, size);
		fLastCommandSize = size;
		fCommandCount++;
		return B_OK;
	}

	virtual status_t Launch() { return B_OK; }

	uint16 LastOpcode() const
	{
		if (fLastCommandSize < sizeof(struct hci_command_header))
			return 0;
		const struct hci_command_header* hdr
			= (const struct hci_command_header*)fLastCommand;
		return B_LENDIAN_TO_HOST_INT16(hdr->opcode);
	}

	const uint8* LastPayload() const
	{
		return fLastCommand + sizeof(struct hci_command_header);
	}

	uint8 LastPayloadSize() const
	{
		if (fLastCommandSize <= sizeof(struct hci_command_header))
			return 0;
		return fLastCommandSize - sizeof(struct hci_command_header);
	}
};


// ============================================================
// TestLocalDeviceImpl — subclass that exposes the constructor
// ============================================================

class TestLocalDeviceImpl : public LocalDeviceImpl {
public:
	TestLocalDeviceImpl(HCIDelegate* hd)
		: LocalDeviceImpl(hd) {}
};


// ============================================================
// Helpers
// ============================================================

static int sTestCount = 0;
static int sPassCount = 0;

static void
Check(bool condition, const char* description)
{
	sTestCount++;
	if (condition) {
		sPassCount++;
		printf("  PASS: %s\n", description);
	} else {
		printf("  FAIL: %s\n", description);
	}
}


static bdaddr_t
MakeAddr(uint8 b5, uint8 b4, uint8 b3, uint8 b2, uint8 b1, uint8 b0)
{
	bdaddr_t addr;
	addr.b[0] = b0;
	addr.b[1] = b1;
	addr.b[2] = b2;
	addr.b[3] = b3;
	addr.b[4] = b4;
	addr.b[5] = b5;
	return addr;
}


// Build a synthetic HCI event packet: [hci_event_header | payload]
// Returns pointer to a static buffer (not reentrant).
static uint8*
BuildHciEvent(uint8 ecode, const void* payload, uint8 payloadLen)
{
	static uint8 sBuffer[260];
	struct hci_event_header* hdr = (struct hci_event_header*)sBuffer;
	hdr->ecode = ecode;
	hdr->elen = payloadLen;
	if (payload != NULL && payloadLen > 0)
		memcpy(sBuffer + sizeof(struct hci_event_header), payload, payloadLen);
	return sBuffer;
}


// ============================================================
// Test 1: LinkKeyRequested without key → NEG_REPLY
// ============================================================

static void
TestLinkKeyRequestedNoKey()
{
	printf("Test 1: LinkKeyRequested without key -> NEG_REPLY\n");

	MockHCIDelegate* mock = new MockHCIDelegate();
	TestLocalDeviceImpl device(mock);
	BluetoothKeyStore keyStore;
	device.SetKeyStore(&keyStore);

	bdaddr_t addr = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF);

	// Build LINK_KEY_REQ event (0x17): payload is just bdaddr_t
	struct hci_ev_link_key_req payload;
	bdaddrUtils::Copy(payload.bdaddr, addr);

	uint8* event = BuildHciEvent(HCI_EVENT_LINK_KEY_REQ,
		&payload, sizeof(payload));

	device.HandleEvent((struct hci_event_header*)event);

	// Verify: NEG_REPLY opcode
	uint16 expectedOpcode = PACK_OPCODE(OGF_LINK_CONTROL,
		OCF_LINK_KEY_NEG_REPLY);
	Check(mock->fCommandCount == 1,
		"exactly 1 command issued");
	Check(mock->LastOpcode() == expectedOpcode,
		"opcode is LINK_KEY_NEG_REPLY");

	// Verify: payload contains the bdaddr
	const uint8* cmdPayload = mock->LastPayload();
	Check(memcmp(cmdPayload, &addr, sizeof(bdaddr_t)) == 0,
		"NEG_REPLY payload contains correct bdaddr");
}


// ============================================================
// Test 2: LinkKeyNotify + LinkKeyRequested → REPLY with key
// ============================================================

static void
TestLinkKeyNotifyThenRequest()
{
	printf("Test 2: LinkKeyNotify + LinkKeyRequested -> REPLY with key\n");

	MockHCIDelegate* mock = new MockHCIDelegate();
	TestLocalDeviceImpl device(mock);
	BluetoothKeyStore keyStore;
	device.SetKeyStore(&keyStore);

	bdaddr_t addr = MakeAddr(0x11, 0x22, 0x33, 0x44, 0x55, 0x66);

	// Build LINK_KEY_NOTIFY event (0x18): bdaddr + link_key[16] + key_type
	struct hci_ev_link_key_notify notifyPayload;
	bdaddrUtils::Copy(notifyPayload.bdaddr, addr);
	for (int i = 0; i < 16; i++)
		notifyPayload.link_key.l[i] = (uint8)(0xA0 + i);
	notifyPayload.key_type = 0x04; // authenticated combination key

	uint8* event = BuildHciEvent(HCI_EVENT_LINK_KEY_NOTIFY,
		&notifyPayload, sizeof(notifyPayload));

	device.HandleEvent((struct hci_event_header*)event);

	// Verify keystore has the key
	linkkey_t storedKey;
	uint8 storedType;
	Check(keyStore.FindLinkKey(addr, &storedKey, &storedType),
		"keystore has the key after NOTIFY");
	Check(memcmp(storedKey.l, notifyPayload.link_key.l, 16) == 0,
		"stored key matches notified key");
	Check(storedType == 0x04,
		"stored key type matches");

	// Now request the key
	mock->fCommandCount = 0;

	struct hci_ev_link_key_req reqPayload;
	bdaddrUtils::Copy(reqPayload.bdaddr, addr);

	event = BuildHciEvent(HCI_EVENT_LINK_KEY_REQ,
		&reqPayload, sizeof(reqPayload));

	device.HandleEvent((struct hci_event_header*)event);

	// Verify: REPLY opcode
	uint16 expectedOpcode = PACK_OPCODE(OGF_LINK_CONTROL,
		OCF_LINK_KEY_REPLY);
	Check(mock->fCommandCount == 1,
		"exactly 1 command issued");
	Check(mock->LastOpcode() == expectedOpcode,
		"opcode is LINK_KEY_REPLY");

	// Verify: payload = bdaddr + link_key[16]
	const uint8* cmdPayload = mock->LastPayload();
	Check(memcmp(cmdPayload, &addr, sizeof(bdaddr_t)) == 0,
		"REPLY payload starts with correct bdaddr");
	Check(memcmp(cmdPayload + sizeof(bdaddr_t),
		notifyPayload.link_key.l, 16) == 0,
		"REPLY payload contains correct 16-byte key");
}


// ============================================================
// Test 3: LeLtkRequest without LTK → NEG_REPLY
// ============================================================

static void
TestLeLtkRequestNoKey()
{
	printf("Test 3: LeLtkRequest without LTK -> NEG_REPLY\n");

	MockHCIDelegate* mock = new MockHCIDelegate();
	TestLocalDeviceImpl device(mock);
	BluetoothKeyStore keyStore;
	device.SetKeyStore(&keyStore);

	bdaddr_t peerAddr = MakeAddr(0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01);

	// First, simulate LE Connection Complete to set fLeConnectionAddress
	struct {
		struct hci_ev_le_meta meta;
		struct hci_ev_le_conn_complete conn;
	} __attribute__((packed)) leConnPayload;
	leConnPayload.meta.subevent = HCI_LE_SUBEVENT_CONN_COMPLETE;
	leConnPayload.conn.status = 0; // BT_OK
	leConnPayload.conn.handle = 0x0040;
	leConnPayload.conn.role = 0; // central
	leConnPayload.conn.peer_address_type = 0;
	bdaddrUtils::Copy(leConnPayload.conn.peer_address, peerAddr);
	leConnPayload.conn.interval = 0x0018;
	leConnPayload.conn.latency = 0;
	leConnPayload.conn.supervision_timeout = 0x002C;
	leConnPayload.conn.master_clock_accuracy = 0;

	uint8* event = BuildHciEvent(HCI_EVENT_LE_META,
		&leConnPayload, sizeof(leConnPayload));

	device.HandleEvent((struct hci_event_header*)event);

	// Now inject LTK Request
	mock->fCommandCount = 0;

	struct {
		struct hci_ev_le_meta meta;
		struct hci_ev_le_ltk_request ltk;
	} __attribute__((packed)) ltkPayload;
	ltkPayload.meta.subevent = HCI_LE_SUBEVENT_LTK_REQUEST;
	ltkPayload.ltk.handle = 0x0040;
	memset(ltkPayload.ltk.random, 0, 8);
	ltkPayload.ltk.ediv = 0;

	event = BuildHciEvent(HCI_EVENT_LE_META,
		&ltkPayload, sizeof(ltkPayload));

	device.HandleEvent((struct hci_event_header*)event);

	// Verify: NEG_REPLY
	uint16 expectedOpcode = PACK_OPCODE(OGF_LE_CONTROL,
		OCF_LE_LTK_REQUEST_NEG_REPLY);
	Check(mock->fCommandCount == 1,
		"exactly 1 command issued");
	Check(mock->LastOpcode() == expectedOpcode,
		"opcode is LE_LTK_REQUEST_NEG_REPLY");
}


// ============================================================
// Test 4: LeLtkRequest with LTK → REPLY
// ============================================================

static void
TestLeLtkRequestWithKey()
{
	printf("Test 4: LeLtkRequest with LTK -> REPLY\n");

	MockHCIDelegate* mock = new MockHCIDelegate();
	TestLocalDeviceImpl device(mock);
	BluetoothKeyStore keyStore;
	device.SetKeyStore(&keyStore);

	bdaddr_t peerAddr = MakeAddr(0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x02);

	// Add an LTK to the keystore
	uint8 ltk[16];
	for (int i = 0; i < 16; i++)
		ltk[i] = (uint8)(0xB0 + i);
	uint8 rand[8] = {0};
	keyStore.AddLtk(peerAddr, ltk, 0, rand);

	// Simulate LE Connection Complete
	struct {
		struct hci_ev_le_meta meta;
		struct hci_ev_le_conn_complete conn;
	} __attribute__((packed)) leConnPayload;
	leConnPayload.meta.subevent = HCI_LE_SUBEVENT_CONN_COMPLETE;
	leConnPayload.conn.status = 0;
	leConnPayload.conn.handle = 0x0041;
	leConnPayload.conn.role = 0;
	leConnPayload.conn.peer_address_type = 0;
	bdaddrUtils::Copy(leConnPayload.conn.peer_address, peerAddr);
	leConnPayload.conn.interval = 0x0018;
	leConnPayload.conn.latency = 0;
	leConnPayload.conn.supervision_timeout = 0x002C;
	leConnPayload.conn.master_clock_accuracy = 0;

	uint8* event = BuildHciEvent(HCI_EVENT_LE_META,
		&leConnPayload, sizeof(leConnPayload));

	device.HandleEvent((struct hci_event_header*)event);

	// Now inject LTK Request
	mock->fCommandCount = 0;

	struct {
		struct hci_ev_le_meta meta;
		struct hci_ev_le_ltk_request ltkReq;
	} __attribute__((packed)) ltkPayload;
	ltkPayload.meta.subevent = HCI_LE_SUBEVENT_LTK_REQUEST;
	ltkPayload.ltkReq.handle = 0x0041;
	memset(ltkPayload.ltkReq.random, 0, 8);
	ltkPayload.ltkReq.ediv = 0;

	event = BuildHciEvent(HCI_EVENT_LE_META,
		&ltkPayload, sizeof(ltkPayload));

	device.HandleEvent((struct hci_event_header*)event);

	// Verify: REPLY with LTK
	uint16 expectedOpcode = PACK_OPCODE(OGF_LE_CONTROL,
		OCF_LE_LTK_REQUEST_REPLY);
	Check(mock->fCommandCount == 1,
		"exactly 1 command issued");
	Check(mock->LastOpcode() == expectedOpcode,
		"opcode is LE_LTK_REQUEST_REPLY");

	// Payload: handle (2) + ltk (16)
	const uint8* cmdPayload = mock->LastPayload();
	uint16 replyHandle;
	memcpy(&replyHandle, cmdPayload, 2);
	Check(replyHandle == 0x0041,
		"REPLY handle matches connection handle");
	Check(memcmp(cmdPayload + 2, ltk, 16) == 0,
		"REPLY contains correct 16-byte LTK");
}


// ============================================================
// Test 5: LinkKeyNotify persists to disk
// ============================================================

static void
TestLinkKeyNotifyPersists()
{
	printf("Test 5: LinkKeyNotify persists to disk\n");

	// Create a temp file path
	char tmpPath[] = "/tmp/bt_key_test_XXXXXX";
	int fd = mkstemp(tmpPath);
	Check(fd >= 0, "created temp file");
	if (fd < 0)
		return;
	close(fd);

	{
		MockHCIDelegate* mock = new MockHCIDelegate();
		TestLocalDeviceImpl device(mock);
		BluetoothKeyStore keyStore;
		device.SetKeyStore(&keyStore);

		bdaddr_t addr = MakeAddr(0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC);

		// Build LINK_KEY_NOTIFY event
		struct hci_ev_link_key_notify notifyPayload;
		bdaddrUtils::Copy(notifyPayload.bdaddr, addr);
		for (int i = 0; i < 16; i++)
			notifyPayload.link_key.l[i] = (uint8)(0xC0 + i);
		notifyPayload.key_type = 0x03;

		uint8* event = BuildHciEvent(HCI_EVENT_LINK_KEY_NOTIFY,
			&notifyPayload, sizeof(notifyPayload));

		device.HandleEvent((struct hci_event_header*)event);

		// Save to temp file
		status_t status = keyStore.Save(tmpPath);
		Check(status == B_OK, "keystore saved to temp file");
	}

	// Load into a fresh keystore and verify
	{
		BluetoothKeyStore keyStore2;
		status_t status = keyStore2.Load(tmpPath);
		Check(status == B_OK, "keystore loaded from temp file");

		bdaddr_t addr = MakeAddr(0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC);
		linkkey_t key;
		uint8 type;
		bool found = keyStore2.FindLinkKey(addr, &key, &type);
		Check(found, "key found in reloaded keystore");

		if (found) {
			uint8 expected[16];
			for (int i = 0; i < 16; i++)
				expected[i] = (uint8)(0xC0 + i);
			Check(memcmp(key.l, expected, 16) == 0,
				"persisted key matches original");
			Check(type == 0x03,
				"persisted key type matches");
		}
	}

	unlink(tmpPath);
}


// ============================================================
// Main
// ============================================================

int
main(int argc, char** argv)
{
	// BApplication is needed for BMessenger (be_app_messenger)
	BApplication app("application/x-vnd.Haiku-BtServerKeyTest");

	printf("=== bt_server_key_test ===\n\n");

	TestLinkKeyRequestedNoKey();
	printf("\n");

	TestLinkKeyNotifyThenRequest();
	printf("\n");

	TestLeLtkRequestNoKey();
	printf("\n");

	TestLeLtkRequestWithKey();
	printf("\n");

	TestLinkKeyNotifyPersists();
	printf("\n");

	printf("=== Results: %d/%d passed ===\n", sPassCount, sTestCount);

	return (sPassCount == sTestCount) ? 0 : 1;
}
