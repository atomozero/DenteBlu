/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * Unit test for BluetoothKeyStore: link key and LTK persistence.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include "BluetoothKeyStore.h"

static int sTestCount = 0;
static int sPassCount = 0;


static void
Check(bool condition, const char* description)
{
	sTestCount++;
	if (condition) {
		sPassCount++;
		printf("  PASS: %s\n", description);
	} else {
		printf("  FAIL: %s\n", description);
	}
}


static bdaddr_t
MakeAddr(uint8 b5, uint8 b4, uint8 b3, uint8 b2, uint8 b1, uint8 b0)
{
	bdaddr_t addr;
	addr.b[0] = b0;
	addr.b[1] = b1;
	addr.b[2] = b2;
	addr.b[3] = b3;
	addr.b[4] = b4;
	addr.b[5] = b5;
	return addr;
}


static linkkey_t
MakeKey(uint8 fill)
{
	linkkey_t key;
	memset(key.l, fill, 16);
	return key;
}


static void
TestLinkKeyAddFind()
{
	printf("Test: AddLinkKey / FindLinkKey\n");

	BluetoothKeyStore store;
	bdaddr_t addr1 = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01);
	bdaddr_t addr2 = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02);
	bdaddr_t addr3 = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03);
	linkkey_t key1 = MakeKey(0x11);
	linkkey_t key2 = MakeKey(0x22);
	linkkey_t key3 = MakeKey(0x33);

	store.AddLinkKey(addr1, key1, 0x04);
	store.AddLinkKey(addr2, key2, 0x05);
	store.AddLinkKey(addr3, key3, 0x03);

	linkkey_t outKey;
	uint8 outType;

	bool found1 = store.FindLinkKey(addr1, &outKey, &outType);
	Check(found1, "FindLinkKey addr1 found");
	Check(memcmp(outKey.l, key1.l, 16) == 0, "FindLinkKey addr1 key matches");
	Check(outType == 0x04, "FindLinkKey addr1 type matches");

	bool found2 = store.FindLinkKey(addr2, &outKey, &outType);
	Check(found2, "FindLinkKey addr2 found");
	Check(memcmp(outKey.l, key2.l, 16) == 0, "FindLinkKey addr2 key matches");
	Check(outType == 0x05, "FindLinkKey addr2 type matches");

	bool found3 = store.FindLinkKey(addr3, &outKey, &outType);
	Check(found3, "FindLinkKey addr3 found");
	Check(memcmp(outKey.l, key3.l, 16) == 0, "FindLinkKey addr3 key matches");
	Check(outType == 0x03, "FindLinkKey addr3 type matches");
}


static void
TestLinkKeyNotFound()
{
	printf("Test: FindLinkKey for unknown address\n");

	BluetoothKeyStore store;
	bdaddr_t addr1 = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01);
	bdaddr_t unknown = MakeAddr(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
	linkkey_t key1 = MakeKey(0x11);

	store.AddLinkKey(addr1, key1, 0x04);

	linkkey_t outKey;
	uint8 outType;
	bool found = store.FindLinkKey(unknown, &outKey, &outType);
	Check(!found, "FindLinkKey unknown address returns false");
}


static void
TestLinkKeyRemove()
{
	printf("Test: RemoveLinkKey\n");

	BluetoothKeyStore store;
	bdaddr_t addr1 = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01);
	linkkey_t key1 = MakeKey(0x11);

	store.AddLinkKey(addr1, key1, 0x04);

	linkkey_t outKey;
	uint8 outType;
	Check(store.FindLinkKey(addr1, &outKey, &outType), "Key exists before remove");

	store.RemoveLinkKey(addr1);
	Check(!store.FindLinkKey(addr1, &outKey, &outType),
		"Key gone after RemoveLinkKey");
}


static void
TestLtkAddFind()
{
	printf("Test: AddLtk / FindLtk\n");

	BluetoothKeyStore store;
	bdaddr_t addr1 = MakeAddr(0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
	bdaddr_t addr2 = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF);

	uint8 ltk1[16], ltk2[16];
	memset(ltk1, 0xAA, 16);
	memset(ltk2, 0xBB, 16);
	uint8 rand1[8], rand2[8];
	memset(rand1, 0, 8);
	memset(rand2, 0x55, 8);

	store.AddLtk(addr1, ltk1, 0x0000, rand1);
	store.AddLtk(addr2, ltk2, 0x1234, rand2);

	uint8 outLtk[16];
	uint16 outEdiv;
	uint8 outRand[8];

	bool found1 = store.FindLtk(addr1, outLtk, &outEdiv, outRand);
	Check(found1, "FindLtk addr1 found");
	Check(memcmp(outLtk, ltk1, 16) == 0, "FindLtk addr1 ltk matches");
	Check(outEdiv == 0x0000, "FindLtk addr1 ediv matches");
	Check(memcmp(outRand, rand1, 8) == 0, "FindLtk addr1 rand matches");

	bool found2 = store.FindLtk(addr2, outLtk, &outEdiv, outRand);
	Check(found2, "FindLtk addr2 found");
	Check(memcmp(outLtk, ltk2, 16) == 0, "FindLtk addr2 ltk matches");
	Check(outEdiv == 0x1234, "FindLtk addr2 ediv matches");
	Check(memcmp(outRand, rand2, 8) == 0, "FindLtk addr2 rand matches");
}


static void
TestSaveLoad()
{
	printf("Test: Save / Load persistence\n");

	const char* tmpPath = "/tmp/bt_keystore_test.keys";

	// Phase 1: create store, add data, save
	{
		BluetoothKeyStore store;
		bdaddr_t addr1 = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01);
		bdaddr_t addr2 = MakeAddr(0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
		linkkey_t key1 = MakeKey(0x42);

		store.AddLinkKey(addr1, key1, 0x04);

		uint8 ltk[16];
		memset(ltk, 0xDE, 16);
		uint8 rand[8];
		memset(rand, 0, 8);
		store.AddLtk(addr2, ltk, 0, rand);

		status_t status = store.Save(tmpPath);
		Check(status == B_OK, "Save succeeds");
	}

	// Phase 2: new store, load, verify data survived
	{
		BluetoothKeyStore store;
		status_t status = store.Load(tmpPath);
		Check(status == B_OK, "Load succeeds");

		bdaddr_t addr1 = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01);
		linkkey_t outKey;
		uint8 outType;
		bool found = store.FindLinkKey(addr1, &outKey, &outType);
		Check(found, "Link key survives Save/Load");
		Check(outKey.l[0] == 0x42, "Link key data correct after Load");
		Check(outType == 0x04, "Link key type correct after Load");

		bdaddr_t addr2 = MakeAddr(0x11, 0x22, 0x33, 0x44, 0x55, 0x66);
		uint8 outLtk[16];
		uint16 outEdiv;
		uint8 outRand[8];
		found = store.FindLtk(addr2, outLtk, &outEdiv, outRand);
		Check(found, "LTK survives Save/Load");
		Check(outLtk[0] == 0xDE, "LTK data correct after Load");
		Check(outEdiv == 0, "LTK ediv correct after Load");
	}

	unlink(tmpPath);
}


static void
TestLoadNonexistent()
{
	printf("Test: Load nonexistent file\n");

	BluetoothKeyStore store;
	status_t status = store.Load("/tmp/nonexistent_bt_keys_12345");
	Check(status != B_OK, "Load nonexistent file returns error");

	// Should not crash, store should remain usable
	bdaddr_t addr = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01);
	linkkey_t key = MakeKey(0x11);
	store.AddLinkKey(addr, key, 0x04);

	linkkey_t outKey;
	uint8 outType;
	Check(store.FindLinkKey(addr, &outKey, &outType),
		"Store usable after failed Load");
}


static void
TestOverwrite()
{
	printf("Test: Overwrite existing key\n");

	BluetoothKeyStore store;
	bdaddr_t addr = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01);
	linkkey_t key1 = MakeKey(0x11);
	linkkey_t key2 = MakeKey(0x22);

	store.AddLinkKey(addr, key1, 0x04);
	store.AddLinkKey(addr, key2, 0x05);

	linkkey_t outKey;
	uint8 outType;
	bool found = store.FindLinkKey(addr, &outKey, &outType);
	Check(found, "Key found after overwrite");
	Check(memcmp(outKey.l, key2.l, 16) == 0, "Overwrite: second key prevails");
	Check(outType == 0x05, "Overwrite: second type prevails");
}


static void
TestLinkKeyAndLtkCoexist()
{
	printf("Test: Link key and LTK coexist for same address\n");

	BluetoothKeyStore store;
	bdaddr_t addr = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01);

	linkkey_t key = MakeKey(0x42);
	store.AddLinkKey(addr, key, 0x04);

	uint8 ltk[16];
	memset(ltk, 0xBB, 16);
	uint8 rand[8];
	memset(rand, 0, 8);
	store.AddLtk(addr, ltk, 0, rand);

	linkkey_t outKey;
	uint8 outType;
	Check(store.FindLinkKey(addr, &outKey, &outType),
		"Link key still present after AddLtk");

	uint8 outLtk[16];
	uint16 outEdiv;
	uint8 outRand[8];
	Check(store.FindLtk(addr, outLtk, &outEdiv, outRand),
		"LTK present alongside link key");

	// Remove LTK, link key should remain
	store.RemoveLtk(addr);
	Check(store.FindLinkKey(addr, &outKey, &outType),
		"Link key survives RemoveLtk");
	Check(!store.FindLtk(addr, outLtk, &outEdiv, outRand),
		"LTK gone after RemoveLtk");

	// Remove link key
	store.RemoveLinkKey(addr);
	Check(!store.FindLinkKey(addr, &outKey, &outType),
		"Link key gone after RemoveLinkKey");
}


int
main()
{
	printf("=== BluetoothKeyStore Unit Tests ===\n\n");

	TestLinkKeyAddFind();
	TestLinkKeyNotFound();
	TestLinkKeyRemove();
	TestLtkAddFind();
	TestSaveLoad();
	TestLoadNonexistent();
	TestOverwrite();
	TestLinkKeyAndLtkCoexist();

	printf("\n=== Results: %d/%d passed ===\n", sPassCount, sTestCount);
	return (sPassCount == sTestCount) ? 0 : 1;
}
