/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * BLE Scanner — scans for BLE devices and prints advertising reports.
 *
 * Usage: ble_scan [--duration <seconds>]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <Application.h>
#include <Message.h>
#include <Messenger.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/BleDevice.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetoothserver_p.h>


using Bluetooth::BleDevice;
using Bluetooth::bdaddrUtils;


static const char*
AddressTypeString(int8 type)
{
	switch (type) {
		case HCI_LE_ADDR_PUBLIC:			return "public";
		case HCI_LE_ADDR_RANDOM:			return "random";
		case HCI_LE_ADDR_PUBLIC_IDENTITY:	return "public-id";
		case HCI_LE_ADDR_RANDOM_IDENTITY:	return "random-id";
		default:							return "unknown";
	}
}


static const char*
EventTypeString(int8 type)
{
	switch (type) {
		case 0x00:	return "ADV_IND";
		case 0x01:	return "ADV_DIRECT_IND";
		case 0x02:	return "ADV_SCAN_IND";
		case 0x03:	return "ADV_NONCONN_IND";
		case 0x04:	return "SCAN_RSP";
		default:	return "UNKNOWN";
	}
}


static void
ParseAdvertisingData(const uint8* data, ssize_t length)
{
	ssize_t offset = 0;

	while (offset < length) {
		uint8 fieldLen = data[offset];
		if (fieldLen == 0 || offset + 1 + fieldLen > length)
			break;

		uint8 adType = data[offset + 1];
		const uint8* payload = &data[offset + 2];
		uint8 payloadLen = fieldLen - 1;

		switch (adType) {
			case 0x01:	/* Flags */
			{
				if (payloadLen >= 1) {
					uint8 flags = payload[0];
					printf("    Flags: 0x%02x", flags);
					if (flags & 0x01) printf(" LE-Limited");
					if (flags & 0x02) printf(" LE-General");
					if (flags & 0x04) printf(" BR/EDR-Not-Supported");
					printf("\n");
				}
				break;
			}

			case 0x08:	/* Shortened Local Name */
			case 0x09:	/* Complete Local Name */
			{
				char name[256];
				uint8 copyLen = payloadLen < 255 ? payloadLen : 255;
				memcpy(name, payload, copyLen);
				name[copyLen] = '\0';
				printf("    Name: %s%s\n", name,
					adType == 0x08 ? " (short)" : "");
				break;
			}

			case 0x0A:	/* TX Power Level */
			{
				if (payloadLen >= 1)
					printf("    TX Power: %d dBm\n", (int8)payload[0]);
				break;
			}

			case 0x06:	/* Incomplete 128-bit UUIDs */
			case 0x07:	/* Complete 128-bit UUIDs */
			{
				for (uint8 i = 0; i + 16 <= payloadLen; i += 16) {
					/* Print UUID in standard format (big-endian display) */
					printf("    UUID128: ");
					printf("%02x%02x%02x%02x-",
						payload[i + 15], payload[i + 14],
						payload[i + 13], payload[i + 12]);
					printf("%02x%02x-", payload[i + 11], payload[i + 10]);
					printf("%02x%02x-", payload[i + 9], payload[i + 8]);
					printf("%02x%02x-", payload[i + 7], payload[i + 6]);
					printf("%02x%02x%02x%02x%02x%02x",
						payload[i + 5], payload[i + 4],
						payload[i + 3], payload[i + 2],
						payload[i + 1], payload[i + 0]);
					printf("%s\n", adType == 0x06 ? " (incomplete)" : "");
				}
				break;
			}

			case 0x02:	/* Incomplete 16-bit UUIDs */
			case 0x03:	/* Complete 16-bit UUIDs */
			{
				for (uint8 i = 0; i + 2 <= payloadLen; i += 2) {
					uint16 uuid = payload[i] | (payload[i + 1] << 8);
					printf("    UUID16: 0x%04x%s\n", uuid,
						adType == 0x02 ? " (incomplete)" : "");
				}
				break;
			}

			case 0xFF:	/* Manufacturer Specific Data */
			{
				if (payloadLen >= 2) {
					uint16 companyId = payload[0] | (payload[1] << 8);
					printf("    Manufacturer: 0x%04x, data[%d]:",
						companyId, payloadLen - 2);
					for (uint8 i = 2; i < payloadLen && i < 18; i++)
						printf(" %02x", payload[i]);
					if (payloadLen > 18)
						printf("...");
					printf("\n");
				}
				break;
			}

			default:
				printf("    AD type 0x%02x, len %d\n", adType, payloadLen);
				break;
		}

		offset += 1 + fieldLen;
	}
}


int
main(int argc, char** argv)
{
	int duration = 10;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
			duration = atoi(argv[++i]);
			if (duration < 1) duration = 1;
			if (duration > 300) duration = 300;
		} else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
			printf("Usage: ble_scan [--duration <seconds>]\n");
			printf("  Scans for BLE devices and prints advertising data.\n");
			printf("  Default duration: 10 seconds (max 300)\n");
			return 0;
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			return 1;
		}
	}

	/* We need a BApplication for BMessenger to work */
	BApplication app("application/x-vnd.Haiku-ble_scan");

	printf("Starting BLE scan for %d seconds...\n", duration);

	status_t status = BleDevice::StartScan();
	if (status != B_OK) {
		fprintf(stderr, "Failed to start scan: %s\n", strerror(status));
		return 1;
	}

	sleep(duration);

	/* Fetch accumulated results from the server */
	BMessenger messenger(BLUETOOTH_SIGNATURE);
	if (!messenger.IsValid()) {
		fprintf(stderr, "Cannot connect to bluetooth_server\n");
		return 1;
	}

	BMessage request(BT_MSG_LE_SCAN_RESULTS_GET);
	BMessage reply;
	status = messenger.SendMessage(&request, &reply);
	if (status != B_OK) {
		fprintf(stderr, "Failed to get scan results: %s\n", strerror(status));
		BleDevice::StopScan();
		return 1;
	}

	/* Stop scanning */
	BleDevice::StopScan();

	/* Parse and print results */
	int32 count = 0;
	reply.FindInt32("count", &count);

	printf("\n--- BLE Scan Results (%d devices) ---\n\n", (int)count);

	BMessage device;
	for (int32 i = 0; reply.FindMessage("device", i, &device) == B_OK; i++) {
		const void* addrData;
		ssize_t addrSize;
		int8 addressType = 0;
		int8 rssi = 0;
		int8 eventType = 0;

		device.FindInt8("address_type", &addressType);
		device.FindInt8("rssi", &rssi);
		device.FindInt8("event_type", &eventType);

		BString addrStr = "??:??:??:??:??:??";
		if (device.FindData("bdaddr", B_ANY_TYPE, &addrData, &addrSize) == B_OK
			&& addrSize == sizeof(bdaddr_t)) {
			bdaddr_t addr;
			memcpy(&addr, addrData, sizeof(bdaddr_t));
			addrStr = bdaddrUtils::ToString(addr);
		}

		printf("[%d] %s  (%s, %s)  RSSI: %d dBm\n",
			(int)(i + 1),
			addrStr.String(),
			AddressTypeString(addressType),
			EventTypeString(eventType),
			rssi);

		const void* advData;
		ssize_t advLen;
		if (device.FindData("adv_data", B_ANY_TYPE, &advData, &advLen) == B_OK
			&& advLen > 0) {
			ParseAdvertisingData((const uint8*)advData, advLen);
		}

		printf("\n");
	}

	if (count == 0)
		printf("No BLE devices found.\n");

	return 0;
}
