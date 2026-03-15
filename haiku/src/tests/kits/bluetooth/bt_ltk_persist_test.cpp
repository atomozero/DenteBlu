/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * Test for LTK persistence in BluetoothKeyStore.
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


static void
TestLtkBasic()
{
	printf("Test: Basic LTK add/find\n");

	BluetoothKeyStore store;
	bdaddr_t addr = MakeAddr(0x11, 0x22, 0x33, 0x44, 0x55, 0x66);

	uint8 ltk[16];
	memset(ltk, 0xAA, 16);
	uint8 rand[8];
	memset(rand, 0, 8);  // SC: rand = 0

	store.AddLtk(addr, ltk, 0, rand);

	uint8 outLtk[16];
	uint16 outEdiv;
	uint8 outRand[8];

	bool found = store.FindLtk(addr, outLtk, &outEdiv, outRand);
	Check(found, "FindLtk returns true");
	Check(memcmp(outLtk, ltk, 16) == 0, "LTK data matches");
	Check(outEdiv == 0, "EDIV == 0 (Secure Connections)");
	Check(memcmp(outRand, rand, 8) == 0, "Rand == 0 (Secure Connections)");
}


static void
TestLtkPersistence()
{
	printf("Test: LTK Save/Load persistence\n");

	const char* tmpPath = "/tmp/bt_ltk_persist_test.keys";

	bdaddr_t addr = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF);
	uint8 ltk[16];
	for (int i = 0; i < 16; i++)
		ltk[i] = i;
	uint8 rand[8];
	memset(rand, 0, 8);

	// Save
	{
		BluetoothKeyStore store;
		store.AddLtk(addr, ltk, 0, rand);
		status_t status = store.Save(tmpPath);
		Check(status == B_OK, "Save succeeds");
	}

	// Load in new instance
	{
		BluetoothKeyStore store;
		status_t status = store.Load(tmpPath);
		Check(status == B_OK, "Load succeeds");

		uint8 outLtk[16];
		uint16 outEdiv;
		uint8 outRand[8];
		bool found = store.FindLtk(addr, outLtk, &outEdiv, outRand);
		Check(found, "LTK found after Load");
		Check(memcmp(outLtk, ltk, 16) == 0, "LTK data survives reboot");
		Check(outEdiv == 0, "EDIV survives reboot");
	}

	unlink(tmpPath);
}


static void
TestLtkAndLinkKeyCoexist()
{
	printf("Test: LTK and link key coexist for same address\n");

	BluetoothKeyStore store;
	bdaddr_t addr = MakeAddr(0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01);

	// Add both types for same address
	linkkey_t key;
	memset(key.l, 0x42, 16);
	store.AddLinkKey(addr, key, 0x04);

	uint8 ltk[16];
	memset(ltk, 0xBB, 16);
	uint8 rand[8];
	memset(rand, 0, 8);
	store.AddLtk(addr, ltk, 0, rand);

	// Both should be findable
	linkkey_t outKey;
	uint8 outType;
	Check(store.FindLinkKey(addr, &outKey, &outType),
		"Link key exists alongside LTK");

	uint8 outLtk[16];
	uint16 outEdiv;
	uint8 outRand[8];
	Check(store.FindLtk(addr, outLtk, &outEdiv, outRand),
		"LTK exists alongside link key");

	// RemoveLtk should not affect link key
	store.RemoveLtk(addr);
	Check(store.FindLinkKey(addr, &outKey, &outType),
		"Link key survives RemoveLtk");
	Check(!store.FindLtk(addr, outLtk, &outEdiv, outRand),
		"LTK gone after RemoveLtk");

	// Re-add LTK, remove link key
	store.AddLtk(addr, ltk, 0, rand);
	store.RemoveLinkKey(addr);
	Check(!store.FindLinkKey(addr, &outKey, &outType),
		"Link key gone after RemoveLinkKey");
	Check(store.FindLtk(addr, outLtk, &outEdiv, outRand),
		"LTK survives RemoveLinkKey");
}


static void
TestLtkNonScValues()
{
	printf("Test: LTK with non-zero EDIV and rand (legacy LE pairing)\n");

	BluetoothKeyStore store;
	bdaddr_t addr = MakeAddr(0x11, 0x22, 0x33, 0x44, 0x55, 0x66);

	uint8 ltk[16];
	memset(ltk, 0xDD, 16);
	uint16 ediv = 0x1234;
	uint8 rand[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

	store.AddLtk(addr, ltk, ediv, rand);

	uint8 outLtk[16];
	uint16 outEdiv;
	uint8 outRand[8];
	bool found = store.FindLtk(addr, outLtk, &outEdiv, outRand);
	Check(found, "Non-SC LTK found");
	Check(outEdiv == 0x1234, "EDIV preserved (0x1234)");
	Check(memcmp(outRand, rand, 8) == 0, "Rand preserved");
}


int
main()
{
	printf("=== LTK Persistence Tests ===\n\n");

	TestLtkBasic();
	TestLtkPersistence();
	TestLtkAndLinkKeyCoexist();
	TestLtkNonScValues();

	printf("\n=== Results: %d/%d passed ===\n", sPassCount, sTestCount);
	return (sPassCount == sTestCount) ? 0 : 1;
}
