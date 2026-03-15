/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * End-to-end test / CLI tool for Bluetooth key persistence.
 *
 * Usage:
 *   bt_key_e2e_test --list             List all stored keys
 *   bt_key_e2e_test --clear AA:BB:..   Remove keys for given address
 *   bt_key_e2e_test --add-test         Add a test key and verify persistence
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <bluetooth/bdaddrUtils.h>

#include "BluetoothKeyStore.h"


static void
PrintUsage(const char* progname)
{
	printf("Usage: %s [--list | --clear AA:BB:CC:DD:EE:FF | --add-test]\n",
		progname);
	printf("\n");
	printf("  --list       List all stored Bluetooth keys\n");
	printf("  --clear ADDR Remove all keys for the given BD address\n");
	printf("  --add-test   Add a test key, save, reload, and verify\n");
}


static void
ListKeys()
{
	BluetoothKeyStore store;
	status_t status = store.Load();
	if (status != B_OK) {
		printf("No key store found (status=%" B_PRId32 ")\n", status);
		return;
	}

	printf("Stored Bluetooth keys:\n");
	printf("-----------------------------------------------\n");

	const BMessage& keys = store.Keys();
	char* name;
	type_code type;
	int32 count;
	bool found = false;

	for (int32 i = 0;
		keys.GetInfo(B_RAW_TYPE, i, &name, &type, &count) == B_OK;
		i++) {

		if (strncmp(name, "lk:", 3) == 0) {
			const char* addrStr = name + 3;
			bdaddr_t addr = bdaddrUtils::FromString(addrStr);

			linkkey_t key;
			uint8 keyType;
			if (store.FindLinkKey(addr, &key, &keyType)) {
				printf("  [Link Key] %s  type=%d  key=", addrStr, keyType);
				for (int j = 0; j < 16; j++)
					printf("%02X", key.l[j]);
				printf("\n");
				found = true;
			}
		} else if (strncmp(name, "ltk:", 4) == 0) {
			const char* addrStr = name + 4;
			bdaddr_t addr = bdaddrUtils::FromString(addrStr);

			uint8 ltk[16];
			uint16 ediv;
			uint8 rand[8];
			if (store.FindLtk(addr, ltk, &ediv, rand)) {
				printf("  [LTK]      %s  ediv=%#06x  ltk=", addrStr, ediv);
				for (int j = 0; j < 16; j++)
					printf("%02X", ltk[j]);
				printf("  rand=");
				for (int j = 0; j < 8; j++)
					printf("%02X", rand[j]);
				printf("\n");
				found = true;
			}
		}
	}

	if (!found)
		printf("  (no keys stored)\n");
}


static void
ClearKey(const char* addrStr)
{
	BluetoothKeyStore store;
	status_t status = store.Load();
	if (status != B_OK) {
		printf("No key store found\n");
		return;
	}

	bdaddr_t addr = bdaddrUtils::FromString(addrStr);

	bool hadLk = store.FindLinkKey(addr, NULL, NULL);
	bool hadLtk = store.FindLtk(addr, NULL, NULL, NULL);

	store.RemoveLinkKey(addr);
	store.RemoveLtk(addr);

	status = store.Save();
	if (status != B_OK) {
		printf("Error saving key store: %" B_PRId32 "\n", status);
		return;
	}

	if (hadLk || hadLtk)
		printf("Removed keys for %s (link_key=%s, ltk=%s)\n", addrStr,
			hadLk ? "yes" : "no", hadLtk ? "yes" : "no");
	else
		printf("No keys found for %s\n", addrStr);
}


static void
AddTestKey()
{
	const char* tmpPath = "/tmp/bt_key_e2e_test.keys";

	printf("Phase 1: Create and save test keys\n");

	bdaddr_t testAddr = bdaddrUtils::FromString("DE:AD:BE:EF:00:01");

	linkkey_t testKey;
	memset(testKey.l, 0xAB, 16);

	uint8 testLtk[16];
	memset(testLtk, 0xCD, 16);
	uint8 testRand[8];
	memset(testRand, 0, 8);

	{
		BluetoothKeyStore store;
		store.AddLinkKey(testAddr, testKey, 0x04);
		store.AddLtk(testAddr, testLtk, 0, testRand);
		status_t status = store.Save(tmpPath);
		if (status != B_OK) {
			printf("  FAIL: Save returned %" B_PRId32 "\n", status);
			return;
		}
		printf("  Saved to %s\n", tmpPath);
	}

	printf("Phase 2: Load and verify\n");
	{
		BluetoothKeyStore store;
		status_t status = store.Load(tmpPath);
		if (status != B_OK) {
			printf("  FAIL: Load returned %" B_PRId32 "\n", status);
			unlink(tmpPath);
			return;
		}

		linkkey_t outKey;
		uint8 outType;
		if (!store.FindLinkKey(testAddr, &outKey, &outType)) {
			printf("  FAIL: Link key not found after reload\n");
			unlink(tmpPath);
			return;
		}

		if (memcmp(outKey.l, testKey.l, 16) != 0 || outType != 0x04) {
			printf("  FAIL: Link key data mismatch\n");
			unlink(tmpPath);
			return;
		}

		uint8 outLtk[16];
		uint16 outEdiv;
		uint8 outRand[8];
		if (!store.FindLtk(testAddr, outLtk, &outEdiv, outRand)) {
			printf("  FAIL: LTK not found after reload\n");
			unlink(tmpPath);
			return;
		}

		if (memcmp(outLtk, testLtk, 16) != 0 || outEdiv != 0) {
			printf("  FAIL: LTK data mismatch\n");
			unlink(tmpPath);
			return;
		}

		printf("  PASS: All test keys verified after Save/Load cycle\n");
	}

	unlink(tmpPath);
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		PrintUsage(argv[0]);
		return 1;
	}

	if (strcmp(argv[1], "--list") == 0) {
		ListKeys();
	} else if (strcmp(argv[1], "--clear") == 0) {
		if (argc < 3) {
			printf("Error: --clear requires a BD address argument\n");
			return 1;
		}
		ClearKey(argv[2]);
	} else if (strcmp(argv[1], "--add-test") == 0) {
		AddTestKey();
	} else {
		PrintUsage(argv[0]);
		return 1;
	}

	return 0;
}
