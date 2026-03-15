/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * BLE Connect — connects to a BLE device and performs GATT discovery.
 *
 * Usage: ble_connect <BD_ADDR> [address_type]
 *        address_type: 0 = public (default), 1 = random
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/BleDevice.h>
#include <bluetooth/HCI/btHCI.h>


using Bluetooth::BleDevice;
using Bluetooth::bdaddrUtils;


int
main(int argc, char** argv)
{
	if (argc < 2 || strcmp(argv[1], "--help") == 0
		|| strcmp(argv[1], "-h") == 0) {
		printf("Usage: ble_connect <BD_ADDR> [address_type]\n");
		printf("  BD_ADDR:      Bluetooth address (XX:XX:XX:XX:XX:XX)\n");
		printf("  address_type: 0 = public (default), 1 = random\n");
		printf("\nConnects to a BLE device and discovers GATT services.\n");
		printf("Press Enter to disconnect.\n");
		return argc < 2 ? 1 : 0;
	}

	bdaddr_t address = bdaddrUtils::FromString(argv[1]);
	if (bdaddrUtils::Compare(address, bdaddrUtils::NullAddress())) {
		fprintf(stderr, "Invalid BD address: %s\n", argv[1]);
		return 1;
	}

	uint8 addressType = HCI_LE_ADDR_PUBLIC;
	if (argc >= 3) {
		int type = atoi(argv[2]);
		if (type < 0 || type > 3) {
			fprintf(stderr, "Invalid address type: %s (must be 0-3)\n",
				argv[2]);
			return 1;
		}
		addressType = (uint8)type;
	}

	/* We need a BApplication for BMessenger to work */
	BApplication app("application/x-vnd.Haiku-ble_connect");

	printf("Connecting to %s (type: %s)...\n",
		bdaddrUtils::ToString(address).String(),
		addressType == HCI_LE_ADDR_PUBLIC ? "public" : "random");

	BleDevice device(address, addressType);

	status_t status = device.Connect();
	if (status != B_OK) {
		fprintf(stderr, "Connect failed: %s\n", strerror(status));
		return 1;
	}

	printf("Connected! Handle: 0x%04x\n", device.ConnectionHandle());

	printf("Discovering GATT services...\n");
	status = device.DiscoverServices();
	if (status != B_OK) {
		fprintf(stderr, "GATT discovery failed: %s\n", strerror(status));
	} else {
		printf("GATT discovery complete.\n");
	}

	printf("\nPress Enter to disconnect...");
	fflush(stdout);
	getchar();

	printf("Disconnecting...\n");
	status = device.Disconnect();
	if (status != B_OK)
		fprintf(stderr, "Disconnect error: %s\n", strerror(status));
	else
		printf("Disconnected.\n");

	return 0;
}
