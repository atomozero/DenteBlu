/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_reconnect_test — Auto-reconnect configuration test.
 *
 * Usage:
 *   bt_reconnect_test --enable <BD_ADDR>    Enable auto-reconnect
 *   bt_reconnect_test --disable <BD_ADDR>   Disable auto-reconnect
 *   bt_reconnect_test --list                List paired devices + reconnect flags
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <Messenger.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/bluetooth.h>
#include <bluetoothserver_p.h>


using namespace Bluetooth;


static void
Usage(const char* progName)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s --enable <BD_ADDR>    Enable auto-reconnect for device\n"
		"  %s --disable <BD_ADDR>   Disable auto-reconnect for device\n"
		"  %s --list                List paired devices and reconnect flags\n",
		progName, progName, progName);
}


static status_t
SetAutoReconnect(const char* addrStr, bool enabled)
{
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		fprintf(stderr, "Cannot reach BluetoothServer\n");
		return B_ERROR;
	}

	BMessage request(BT_MSG_SET_AUTO_RECONNECT);
	request.AddString("bdaddr", addrStr);
	request.AddBool("enabled", enabled);

	BMessage reply;
	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 5000000LL);
	if (result != B_OK) {
		fprintf(stderr, "SendMessage failed: %s\n", strerror(result));
		return result;
	}

	int32 status;
	if (reply.FindInt32("status", &status) != B_OK || status != B_OK) {
		fprintf(stderr, "Server returned error: %" B_PRId32 "\n", status);
		return (status_t)status;
	}

	printf("Auto-reconnect for %s: %s\n", addrStr,
		enabled ? "ENABLED" : "DISABLED");
	return B_OK;
}


static status_t
ListPairedDevices()
{
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		fprintf(stderr, "Cannot reach BluetoothServer\n");
		return B_ERROR;
	}

	BMessage request(BT_MSG_GET_PAIRED_DEVICES);
	BMessage reply;
	status_t result = messenger.SendMessage(&request, &reply,
		B_INFINITE_TIMEOUT, 5000000LL);
	if (result != B_OK) {
		fprintf(stderr, "SendMessage failed: %s\n", strerror(result));
		return result;
	}

	int32 count = 0;
	reply.FindInt32("count", &count);

	if (count == 0) {
		printf("No paired devices.\n");
		return B_OK;
	}

	printf("Paired devices (%d):\n", (int)count);
	printf("%-20s %-20s %s\n", "BD_ADDR", "Name", "Auto-Reconnect");
	printf("%-20s %-20s %s\n", "---", "---", "---");

	for (int32 i = 0; i < count; i++) {
		BString addrStr;
		BString name;
		reply.FindString("bdaddr", i, &addrStr);
		reply.FindString("name", i, &name);

		if (name.Length() == 0)
			name = "(unknown)";

		// Auto-reconnect flag is not in the reply yet — we'd need
		// server support for that. For now, just show the device list.
		printf("%-20s %-20s (check server log)\n",
			addrStr.String(), name.String());
	}

	return B_OK;
}


int
main(int argc, char* argv[])
{
	if (argc < 2) {
		Usage(argv[0]);
		return 1;
	}

	BApplication app("application/x-vnd.Haiku-bt_reconnect_test");

	if (strcmp(argv[1], "--list") == 0) {
		return ListPairedDevices() == B_OK ? 0 : 1;
	}

	if (argc < 3) {
		Usage(argv[0]);
		return 1;
	}

	bool enable;
	if (strcmp(argv[1], "--enable") == 0)
		enable = true;
	else if (strcmp(argv[1], "--disable") == 0)
		enable = false;
	else {
		Usage(argv[0]);
		return 1;
	}

	// Validate address format
	bdaddr_t addr = bdaddrUtils::FromString(argv[2]);
	if (bdaddrUtils::Compare(addr, bdaddrUtils::NullAddress())) {
		fprintf(stderr, "Invalid BD_ADDR: %s\n", argv[2]);
		return 1;
	}

	status_t result = SetAutoReconnect(argv[2], enable);
	return (result == B_OK) ? 0 : 1;
}
