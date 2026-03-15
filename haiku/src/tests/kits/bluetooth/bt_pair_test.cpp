/*
 * bt_pair_test - Set device discoverable and wait for pairing
 */
#include <stdio.h>
#include <unistd.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/DiscoveryAgent.h>
#include <bluetooth/bdaddrUtils.h>

#include <CommandManager.h>
#include <bluetoothserver_p.h>

#include <Application.h>
#include <Messenger.h>

using namespace Bluetooth;

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
	if (status == B_OK) {
		int8 bt_status;
		if (reply.FindInt8("status", &bt_status) == B_OK)
			return bt_status == BT_OK ? B_OK : B_ERROR;
	}
	return status;
}

int main(int argc, char** argv)
{
	printf("=== Bluetooth Pairing Test ===\n\n");

	uint32 count = LocalDevice::GetLocalDeviceCount();
	printf("Local devices: %" B_PRIu32 "\n", count);
	if (count == 0) {
		printf("No Bluetooth adapter found.\n");
		return 1;
	}

	LocalDevice* device = LocalDevice::GetLocalDevice();
	if (device == NULL) {
		printf("Failed to acquire device.\n");
		return 1;
	}

	printf("Device ID: %" B_PRId32 "\n", device->ID());

	bdaddr_t addr = device->GetBluetoothAddress();
	printf("BD Address: %s\n", bdaddrUtils::ToString(addr).String());

	BString name = device->GetFriendlyName();
	printf("Current name: %s\n", name.String());

	// Set a friendly name so the phone can recognize us
	BString newName("DenteBlu-Haiku");
	printf("\nSetting name to: %s\n", newName.String());
	status_t status = device->SetFriendlyName(newName);
	printf("SetFriendlyName result: %s\n", strerror(status));

	// Read back name
	name = device->GetFriendlyName();
	printf("Name now: %s\n", name.String());

	// Set device class: Major=Computer(0x01), Minor=Laptop(0x0C)
	// CoD bytes: [minor<<2 | format], [major], [service bits high]
	// 0x0C = minor 3 (Laptop) << 2 = 0x0C
	// Major class Computer = 0x01
	// Service class: none for now
	uint8 devClassBytes[3] = {0x0C, 0x01, 0x00};
	printf("\nSetting device class to Computer/Laptop (0x00010C)...\n");
	status = SetDeviceClass(device->ID(), devClassBytes);
	printf("SetDeviceClass result: %s\n", strerror(status));

	// Read back device class
	DeviceClass devClass = device->GetDeviceClass();
	printf("Device class: 0x%06" B_PRIx32 "\n", (uint32)devClass.Record());

	// Ensure discoverable
	int disc = device->GetDiscoverable();
	printf("\nDiscoverable mode: %d", disc);
	if (disc & 0x01)
		printf(" [Inquiry Scan]");
	if (disc & 0x02)
		printf(" [Page Scan]");
	printf("\n");

	if (disc != 3) {
		printf("Setting discoverable mode to 3 (inquiry+page scan)...\n");
		status = device->SetDiscoverable(3);
		printf("SetDiscoverable result: %s\n", strerror(status));
	}

	printf("\n=== Device is now discoverable as '%s' ===\n", newName.String());
	printf("=== BD Address: %s ===\n",
		bdaddrUtils::ToString(addr).String());
	printf("=== Device Class: Computer/Laptop ===\n");
	printf("\nSearch for this device from your phone's Bluetooth settings.\n");
	printf("Press Ctrl+C to stop.\n\n");

	// Keep running so the server stays alive and responsive
	while (true) {
		sleep(5);
		printf(".");
		fflush(stdout);
	}

	return 0;
}
