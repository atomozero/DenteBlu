/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * BLE Pair — full NUS pairing test with a MeshCore device.
 *
 * Usage: ble_pair <BD_ADDR> <address_type> <passkey>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/NusClient.h>
#include <bluetooth/HCI/btHCI.h>


using Bluetooth::NusClient;
using Bluetooth::bdaddrUtils;


static void
DataReceived(const uint8* data, uint16 length, void* /*cookie*/)
{
	printf("  RX [%d bytes]:", length);
	for (uint16 i = 0; i < length; i++) {
		if (i > 0 && (i % 16) == 0)
			printf("\n                ");
		printf(" %02x", data[i]);
	}
	printf("\n");

	/* Also try printing as text if it looks printable */
	bool printable = true;
	for (uint16 i = 0; i < length; i++) {
		if (data[i] < 0x20 && data[i] != '\n' && data[i] != '\r'
			&& data[i] != '\t') {
			printable = false;
			break;
		}
	}
	if (printable && length > 0) {
		printf("  RX (text): ");
		fwrite(data, 1, length, stdout);
		printf("\n");
	}
}


int
main(int argc, char** argv)
{
	if (argc < 4 || strcmp(argv[1], "--help") == 0
		|| strcmp(argv[1], "-h") == 0) {
		printf("Usage: ble_pair <BD_ADDR> <address_type> <passkey>\n");
		printf("  BD_ADDR:      Bluetooth address (XX:XX:XX:XX:XX:XX)\n");
		printf("  address_type: 0 = public, 1 = random\n");
		printf("  passkey:      6-digit numeric passkey\n");
		printf("\nPerforms full NUS pairing with a MeshCore device:\n");
		printf("  connect -> pair -> discover NUS -> send test message\n");
		return argc < 4 ? 1 : 0;
	}

	bdaddr_t address = bdaddrUtils::FromString(argv[1]);
	if (bdaddrUtils::Compare(address, bdaddrUtils::NullAddress())) {
		fprintf(stderr, "Invalid BD address: %s\n", argv[1]);
		return 1;
	}

	int type = atoi(argv[2]);
	if (type < 0 || type > 3) {
		fprintf(stderr, "Invalid address type: %s (must be 0-3)\n", argv[2]);
		return 1;
	}
	uint8 addressType = (uint8)type;

	uint32 passkey = (uint32)strtoul(argv[3], NULL, 10);
	if (passkey > 999999) {
		fprintf(stderr, "Invalid passkey: %s (must be 0-999999)\n", argv[3]);
		return 1;
	}

	/* We need a BApplication for BMessenger to work */
	BApplication app("application/x-vnd.Haiku-ble_pair");

	printf("NUS Pairing Test\n");
	printf("  Address: %s\n", bdaddrUtils::ToString(address).String());
	printf("  Type:    %s\n",
		addressType == HCI_LE_ADDR_PUBLIC ? "public" : "random");
	printf("  Passkey: %06" B_PRIu32 "\n", passkey);
	printf("\n");

	NusClient nus;
	nus.SetDataCallback(DataReceived, NULL);

	printf("Step 1: Initializing NUS (connect + pair + discover)...\n");
	status_t status = nus.Initialize(address, addressType, passkey);
	if (status != B_OK) {
		fprintf(stderr, "NUS initialization failed: %s\n", strerror(status));
		return 1;
	}
	printf("  NUS initialized successfully!\n\n");

	/* Send a test message */
	const char* testMsg = "Hello from Haiku!";
	printf("Step 2: Sending test message: \"%s\"\n", testMsg);
	status = nus.Send((const uint8*)testMsg, strlen(testMsg));
	if (status != B_OK) {
		fprintf(stderr, "  Send failed: %s\n", strerror(status));
	} else {
		printf("  Sent %d bytes.\n", (int)strlen(testMsg));
	}

	printf("\nStep 3: Waiting 5 seconds for incoming data...\n");
	sleep(5);

	printf("\nStep 4: Shutting down...\n");
	nus.Shutdown();
	printf("Done.\n");

	return 0;
}
