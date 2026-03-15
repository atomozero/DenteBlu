/*
 * bt_keystore_clean — Remove a specific key from the Bluetooth key store.
 * Usage: bt_keystore_clean <BD_ADDR>
 *   e.g. bt_keystore_clean 77:88:99:AA:BB:CC
 */

#include <stdio.h>
#include <string.h>

#include <Application.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/bdaddrUtils.h>

#include "BluetoothKeyStore.h"


int
main(int argc, char* argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <BD_ADDR>\n", argv[0]);
		fprintf(stderr, "  Removes a link key from the Bluetooth key store.\n");
		fprintf(stderr, "  Example: %s 77:88:99:AA:BB:CC\n", argv[0]);
		return 1;
	}

	bdaddr_t addr;
	unsigned int b[6];
	if (sscanf(argv[1], "%02X:%02X:%02X:%02X:%02X:%02X",
		&b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
		fprintf(stderr, "Invalid BD_ADDR: %s\n", argv[1]);
		return 1;
	}
	addr.b[5] = (uint8)b[0];
	addr.b[4] = (uint8)b[1];
	addr.b[3] = (uint8)b[2];
	addr.b[2] = (uint8)b[3];
	addr.b[1] = (uint8)b[4];
	addr.b[0] = (uint8)b[5];

	BluetoothKeyStore store;
	status_t status = store.Load();
	if (status != B_OK) {
		fprintf(stderr, "Failed to load key store: %s\n", strerror(status));
		return 1;
	}

	linkkey_t key;
	uint8 type;
	if (!store.FindLinkKey(addr, &key, &type)) {
		printf("No link key found for %s\n", argv[1]);
		return 0;
	}

	printf("Found link key for %s (type=%d). Removing...\n", argv[1], type);
	store.RemoveLinkKey(addr);

	status = store.Save();
	if (status != B_OK) {
		fprintf(stderr, "Failed to save key store: %s\n", strerror(status));
		return 1;
	}

	printf("Key removed and store saved.\n");
	return 0;
}
