/*
 * bt_query - Quick test to query bluetooth_server via Bluetooth Kit
 */
#include <stdio.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/bdaddrUtils.h>

using namespace Bluetooth;

int main()
{
	printf("Querying bluetooth_server...\n");

	uint32 count = LocalDevice::GetLocalDeviceCount();
	printf("Local devices: %" B_PRIu32 "\n", count);

	if (count == 0) {
		printf("No local Bluetooth devices found.\n");
		return 1;
	}

	LocalDevice* device = LocalDevice::GetLocalDevice();
	if (device == NULL) {
		printf("Failed to acquire local device.\n");
		return 1;
	}

	printf("Device ID: %" B_PRId32 "\n", device->ID());

	bdaddr_t addr = device->GetBluetoothAddress();
	printf("BD Address: %s\n", bdaddrUtils::ToString(addr).String());

	BString name = device->GetFriendlyName();
	printf("Friendly name: %s\n", name.String());

	DeviceClass devClass = device->GetDeviceClass();
	printf("Device class: 0x%06" B_PRIx32 "\n", (uint32)devClass.Record());

	int disc = device->GetDiscoverable();
	printf("Discoverable: %d\n", disc);

	return 0;
}
