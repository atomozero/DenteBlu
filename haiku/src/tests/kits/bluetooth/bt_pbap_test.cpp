/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_pbap_test — Phone Book Access Profile (PBAP) test tool.
 *
 * Connects to a remote device's PBAP server and downloads the phonebook.
 *
 * Usage: bt_pbap_test <BD_ADDR> [rfcomm_channel]
 *   If rfcomm_channel is omitted, SDP is queried for UUID 0x112F.
 *
 * Options:
 *   --list    Download vCard listing instead of full phonebook
 *   --vcard30 Use vCard 3.0 format (default is 2.1)
 *   --path <path>  Override phonebook path (default: telecom/pb.vcf)
 *   --output <file> Save output to file (default: stdout)
 *   --l2cap   Force GOEP 2.0 transport over L2CAP (requires ERTM)
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>
#include <String.h>

#include <Messenger.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/obex.h>
#include <bluetooth/PbapClient.h>
#include <bluetoothserver_p.h>
#include <CommandManager.h>


static status_t
SetDeviceClass(hci_id hid, uint8 devClass[3])
{
	size_t size;
	void* command = buildWriteClassOfDevice(devClass, &size);
	if (command == NULL)
		return B_NO_MEMORY;

	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid())
		return B_ERROR;

	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	BMessage reply;
	request.AddInt32("hci_id", hid);
	request.AddData("raw command", B_ANY_TYPE, command, size);
	request.AddInt16("eventExpected", HCI_EVENT_CMD_COMPLETE);
	request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_CONTROL_BASEBAND, OCF_WRITE_CLASS_OF_DEV));

	status_t status = messenger.SendMessage(&request, &reply);
	free(command);
	if (status == B_OK) {
		int8 bt_status;
		if (reply.FindInt8("status", &bt_status) == B_OK)
			return bt_status == BT_OK ? B_OK : B_ERROR;
	}
	return status;
}


static void
PrintUsage(const char* progName)
{
	fprintf(stderr,
		"Usage: %s <BD_ADDR> [options]\n"
		"\n"
		"Options:\n"
		"  --channel <N>   RFCOMM channel (1-30, default: SDP auto-detect)\n"
		"  --l2cap         Force GOEP 2.0 transport over L2CAP (ERTM)\n"
		"  --size          Query phonebook size only (no data download)\n"
		"  --list          Download vCard listing (XML) instead of phonebook\n"
		"  --vcard30       Use vCard 3.0 format (default: 2.1)\n"
		"  --path <path>   Phonebook path (default: telecom/pb.vcf)\n"
		"  --output <file> Save to file instead of stdout\n"
		"\n"
		"Example:\n"
		"  %s 0C:7D:B0:B2:81:6A\n"
		"  %s 0C:7D:B0:B2:81:6A --vcard30 --output contacts.vcf\n"
		"  %s 0C:7D:B0:B2:81:6A --list --path telecom/pb\n",
		progName, progName, progName, progName);
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		PrintUsage(argv[0]);
		return 1;
	}

	/* Parse arguments */
	const char* addrStr = argv[1];
	uint8 rfcommChannel = 0;
	bool listMode = false;
	bool sizeOnly = false;
	bool useL2cap = false;
	uint8 format = PBAP_FORMAT_VCARD_21;
	const char* path = PBAP_PATH_PHONEBOOK;
	const char* outputFile = NULL;
	int waitSeconds = 0;

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
			rfcommChannel = (uint8)atoi(argv[++i]);
		} else if (strcmp(argv[i], "--list") == 0) {
			listMode = true;
		} else if (strcmp(argv[i], "--size") == 0) {
			sizeOnly = true;
		} else if (strcmp(argv[i], "--vcard30") == 0) {
			format = PBAP_FORMAT_VCARD_30;
		} else if (strcmp(argv[i], "--path") == 0 && i + 1 < argc) {
			path = argv[++i];
		} else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
			outputFile = argv[++i];
		} else if (strcmp(argv[i], "--l2cap") == 0) {
			useL2cap = true;
		} else if (strcmp(argv[i], "--wait") == 0 && i + 1 < argc) {
			waitSeconds = atoi(argv[++i]);
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			PrintUsage(argv[0]);
			return 1;
		}
	}

	bdaddr_t remote = bdaddrUtils::FromString(addrStr);
	if (bdaddrUtils::Compare(remote, bdaddrUtils::NullAddress())) {
		fprintf(stderr, "Invalid BD_ADDR: %s\n", addrStr);
		return 1;
	}

	/* BApplication needed for BMessenger IPC with BluetoothServer */
	BApplication app("application/x-vnd.Haiku-bt_pbap_test");

	/* Acquire LocalDevice — constructor enables SSP, Event Mask, EIR */
	Bluetooth::LocalDevice* localDev =
		Bluetooth::LocalDevice::GetLocalDevice();
	if (localDev == NULL) {
		fprintf(stderr, "No local Bluetooth device available\n");
		return 1;
	}
	printf("Local device ready (hci_id=%d)\n", localDev->ID());

	/* Set friendly name so phone shows "Haiku" instead of "null" */
	BString name("Haiku");
	localDev->SetFriendlyName(name);

	/* Set Class of Device: Computer/Laptop with Object Transfer service.
	 * CoD byte 0: minor=3(Laptop) << 2 = 0x0C
	 * CoD byte 1: major=1(Computer) = 0x01
	 * CoD byte 2: service bit 19 (Object Transfer) = 0x08 */
	uint8 devClassBytes[3] = {0x0C, 0x01, 0x08};
	SetDeviceClass(localDev->ID(), devClassBytes);

	/* Disable page scan to prevent phone from auto-connecting.
	 * We'll enable it only for the --wait pairing phase. */
	localDev->SetDiscoverable(0x00);

	if (waitSeconds > 0) {
		/* Enable discoverable for pairing */
		localDev->SetDiscoverable(0x03);
		printf("Discoverable mode enabled\n");
		printf("Waiting %d seconds for phone-initiated pairing...\n",
			waitSeconds);
		printf("Pair from phone now! (Bluetooth > Pair new device > Haiku)\n");
		fflush(stdout);
		sleep(waitSeconds);
		printf("Wait complete, proceeding with PBAP connection.\n");
		/* Disable page scan again to prevent auto-connect during test */
		localDev->SetDiscoverable(0x00);
	}

	printf("PBAP Test — connecting to %s", addrStr);
	if (useL2cap)
		printf(" via L2CAP GOEP 2.0 (ERTM)");
	else if (rfcommChannel > 0)
		printf(" channel %u", rfcommChannel);
	else
		printf(" (SDP auto-detect)");
	printf("...\n");
	fflush(stdout);

	/* Connect */
	Bluetooth::PbapClient pbap;
	status_t result;
	if (useL2cap)
		result = pbap.ConnectL2cap(remote);
	else
		result = pbap.Connect(remote, rfcommChannel);
	if (result != B_OK) {
		fprintf(stderr, "PBAP Connect failed: %s\n", strerror(result));
		return 1;
	}

	printf("PBAP session established.\n");
	fflush(stdout);

	/* Pull data */
	uint8* data = NULL;
	size_t dataLen = 0;

	/* Navigate to phonebook folder first (BlueZ does this).
	 * SETPATH to root → telecom → pb before any GET. */
	printf("Navigating: SETPATH root → telecom → pb\n");
	fflush(stdout);
	result = pbap.SetPath(NULL);  /* root */
	if (result != B_OK) {
		fprintf(stderr, "SETPATH to root failed: %s\n", strerror(result));
		pbap.Disconnect();
		return 1;
	}
	result = pbap.SetPath("telecom");
	if (result != B_OK) {
		fprintf(stderr, "SETPATH to telecom failed: %s\n", strerror(result));
		pbap.Disconnect();
		return 1;
	}
	result = pbap.SetPath("pb");
	if (result != B_OK) {
		fprintf(stderr, "SETPATH to pb failed: %s\n", strerror(result));
		pbap.Disconnect();
		return 1;
	}
	printf("SETPATH navigation complete.\n");
	fflush(stdout);

	if (sizeOnly) {
		printf("Querying phonebook size: %s\n", path);
		fflush(stdout);
		uint16 pbSize = 0;
		result = pbap.GetPhoneBookSize(path, &pbSize);
		if (result != B_OK) {
			fprintf(stderr, "GetPhoneBookSize failed: %s\n",
				strerror(result));
			pbap.Disconnect();
			return 1;
		}
		printf("Phonebook size: %u entries\n", pbSize);
		pbap.Disconnect();
		printf("Done.\n");
		return 0;
	} else if (listMode) {
		printf("\n");
		printf("==========================================================\n");
		printf("  CHECK YOUR PHONE NOW!\n");
		printf("  Approve the 'contact access' / 'contact sharing'\n");
		printf("  notification for \"Haiku\" before proceeding.\n");
		printf("  (Settings > Connected devices > Haiku > Contact sharing)\n");
		printf("==========================================================\n");
		printf("Waiting 15 seconds for you to approve...\n");
		fflush(stdout);
		sleep(15);
		printf("Pulling vCard listing: %s\n", path);
		fflush(stdout);
		result = pbap.PullvCardListing(path, &data, &dataLen);
	} else {
		printf("\n");
		printf("==========================================================\n");
		printf("  CHECK YOUR PHONE NOW!\n");
		printf("  Approve the 'contact access' / 'contact sharing'\n");
		printf("  notification for \"Haiku\" before proceeding.\n");
		printf("  (Settings > Connected devices > Haiku > Contact sharing)\n");
		printf("==========================================================\n");
		printf("Waiting 15 seconds for you to approve...\n");
		fflush(stdout);
		sleep(15);
		printf("Pulling phonebook: %s (format=%s)\n", path,
			format == PBAP_FORMAT_VCARD_30 ? "vCard 3.0" : "vCard 2.1");
		fflush(stdout);
		result = pbap.PullPhoneBook(path, format, &data, &dataLen);
	}

	if (result != B_OK) {
		fprintf(stderr, "Pull failed: %s\n", strerror(result));
		pbap.Disconnect();
		return 1;
	}

	printf("Received %zu bytes of data.\n", dataLen);
	fflush(stdout);

	/* Output */
	if (data != NULL && dataLen > 0) {
		if (outputFile != NULL) {
			FILE* fp = fopen(outputFile, "w");
			if (fp != NULL) {
				fwrite(data, 1, dataLen, fp);
				fclose(fp);
				printf("Saved to %s\n", outputFile);
			} else {
				fprintf(stderr, "Cannot open %s: %s\n", outputFile,
					strerror(errno));
				/* Fall back to stdout */
				fwrite(data, 1, dataLen, stdout);
			}
		} else {
			printf("--- BEGIN DATA ---\n");
			fwrite(data, 1, dataLen, stdout);
			printf("\n--- END DATA ---\n");
		}
	}

	free(data);

	/* Disconnect */
	printf("Disconnecting...\n");
	fflush(stdout);
	pbap.Disconnect();

	printf("Done.\n");
	return 0;
}
