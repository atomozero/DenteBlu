/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_sco_test — Test SCO/eSCO connection setup to a remote device.
 *
 * Usage:
 *   bt_sco_test <BD_ADDR>
 *
 * Establishes an ACL connection, then sets up a synchronous (SCO/eSCO)
 * connection. Reports connection parameters and holds the link open
 * until the user presses Enter.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <Messenger.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/ScoSocket.h>

#include <bluetoothserver_p.h>
#include <CommandManager.h>


static bool
ParseBdAddr(const char* str, bdaddr_t& addr)
{
	unsigned int b[6];
	if (sscanf(str, "%02X:%02X:%02X:%02X:%02X:%02X",
			&b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6)
		return false;

	for (int i = 0; i < 6; i++)
		addr.b[i] = (uint8)b[5 - i];	/* bdaddr_t is little-endian */

	return true;
}


static status_t
EnsureAclConnection(const bdaddr_t& address, uint16& outHandle)
{
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		fprintf(stderr, "Cannot reach BluetoothServer\n");
		return B_ERROR;
	}

	/* Get local HCI ID */
	BMessage acquire(BT_MSG_ACQUIRE_LOCAL_DEVICE);
	BMessage acquireReply;
	if (messenger.SendMessage(&acquire, &acquireReply,
			B_INFINITE_TIMEOUT, 5000000LL) != B_OK) {
		fprintf(stderr, "Failed to query local device\n");
		return B_ERROR;
	}

	hci_id hid;
	if (acquireReply.FindInt32("hci_id", &hid) != B_OK || hid < 0) {
		fprintf(stderr, "No local Bluetooth device found\n");
		return B_ERROR;
	}

	/* Create ACL connection */
	BluetoothCommand<typed_command(hci_cp_create_conn)>
		createCmd(OGF_LINK_CONTROL, OCF_CREATE_CONN);
	bdaddrUtils::Copy(createCmd->bdaddr, address);
	createCmd->pkt_type = 0xCC18;		/* DM1, DH1, DM3, DH3, DM5, DH5 */
	createCmd->pscan_rep_mode = 0x02;	/* R2 */
	createCmd->pscan_mode = 0;
	createCmd->clock_offset = 0;
	createCmd->role_switch = 0x01;		/* allow role switch */

	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	BMessage reply;

	request.AddInt32("hci_id", hid);
	request.AddData("raw command", B_ANY_TYPE,
		createCmd.Data(), createCmd.Size());
	request.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
	request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_LINK_CONTROL, OCF_CREATE_CONN));
	request.AddInt16("eventExpected", HCI_EVENT_CONN_COMPLETE);

	printf("Creating ACL connection...\n");
	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 15000000LL);

	if (result != B_OK) {
		fprintf(stderr, "ACL connection failed: %s\n", strerror(result));
		return result;
	}

	int8 status = -1;
	reply.FindInt8("status", &status);
	if (status != 0) {
		fprintf(stderr, "ACL connection HCI error 0x%02X\n",
			(unsigned)(uint8)status);
		return B_ERROR;
	}

	int16 handle = -1;
	reply.FindInt16("handle", &handle);
	outHandle = (uint16)handle;

	printf("ACL connected: handle=0x%04X\n", outHandle);

	/* Request authentication */
	BluetoothCommand<typed_command(hci_cp_auth_requested)>
		authCmd(OGF_LINK_CONTROL, OCF_AUTH_REQUESTED);
	authCmd->handle = outHandle;

	BMessage authReq(BT_MSG_HANDLE_SIMPLE_REQUEST);
	BMessage authReply;
	authReq.AddInt32("hci_id", hid);
	authReq.AddData("raw command", B_ANY_TYPE,
		authCmd.Data(), authCmd.Size());
	authReq.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
	authReq.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_LINK_CONTROL, OCF_AUTH_REQUESTED));
	authReq.AddInt16("eventExpected", HCI_EVENT_AUTH_COMPLETE);

	printf("Requesting authentication...\n");
	messenger.SendMessage(&authReq, &authReply,
		B_INFINITE_TIMEOUT, 30000000LL);

	int8 authStatus = -1;
	authReply.FindInt8("status", &authStatus);
	printf("Authentication %s (0x%02X)\n",
		authStatus == 0 ? "OK" : "failed",
		(unsigned)(uint8)authStatus);

	return B_OK;
}


int
main(int argc, char* argv[])
{
	if (argc < 2) {
		printf("Usage: bt_sco_test <BD_ADDR>\n");
		printf("  Example: bt_sco_test CC:A7:C1:F2:52:65\n");
		return 1;
	}

	bdaddr_t address;
	if (!ParseBdAddr(argv[1], address)) {
		fprintf(stderr, "Invalid BD_ADDR: %s\n", argv[1]);
		return 1;
	}

	BApplication app("application/x-vnd.Haiku-bt_sco_test");

	/* Step 1: Ensure ACL connection */
	uint16 aclHandle = 0;
	status_t result = EnsureAclConnection(address, aclHandle);
	if (result != B_OK) {
		fprintf(stderr, "ACL connection failed\n");
		return 1;
	}

	/* Step 2: Set up SCO/eSCO connection */
	Bluetooth::ScoSocket sco;

	printf("Setting up SCO connection (ACL handle=0x%04X)...\n", aclHandle);
	result = sco.Connect(address, aclHandle);
	if (result != B_OK) {
		fprintf(stderr, "SCO connection failed: %s\n", strerror(result));
		return 1;
	}

	printf("\n=== SCO Connection Established ===\n");
	printf("  Handle:         0x%04X\n", sco.Handle());
	printf("  Link type:      %u (%s)\n", sco.LinkType(),
		sco.LinkType() == 0 ? "SCO" : "eSCO");
	printf("  Rx packet len:  %u bytes\n", sco.RxPacketLength());
	printf("  Tx packet len:  %u bytes\n", sco.TxPacketLength());
	printf("\nPress Enter to disconnect...\n");

	getchar();

	printf("Disconnecting SCO...\n");
	sco.Disconnect();
	printf("Done.\n");

	return 0;
}
