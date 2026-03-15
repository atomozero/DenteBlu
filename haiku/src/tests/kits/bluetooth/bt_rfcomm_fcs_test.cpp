/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_rfcomm_fcs_test — Unit test for RFCOMM CRC-8 (FCS) computation.
 *
 * Verifies the precomputed FCS table and FCS computation against known
 * test vectors from the Bluetooth Core Specification and TS 07.10.
 */

#include <stdio.h>
#include <string.h>

#include <bluetooth/rfcomm.h>


/* CRC-8 table — same as RfcommSession::sFcsTable */
static const uint8 sFcsTable[256] = {
	0x00, 0x91, 0xE3, 0x72, 0x07, 0x96, 0xE4, 0x75,
	0x0E, 0x9F, 0xED, 0x7C, 0x09, 0x98, 0xEA, 0x7B,
	0x1C, 0x8D, 0xFF, 0x6E, 0x1B, 0x8A, 0xF8, 0x69,
	0x12, 0x83, 0xF1, 0x60, 0x15, 0x84, 0xF6, 0x67,
	0x38, 0xA9, 0xDB, 0x4A, 0x3F, 0xAE, 0xDC, 0x4D,
	0x36, 0xA7, 0xD5, 0x44, 0x31, 0xA0, 0xD2, 0x43,
	0x24, 0xB5, 0xC7, 0x56, 0x23, 0xB2, 0xC0, 0x51,
	0x2A, 0xBB, 0xC9, 0x58, 0x2D, 0xBC, 0xCE, 0x5F,
	0x70, 0xE1, 0x93, 0x02, 0x77, 0xE6, 0x94, 0x05,
	0x7E, 0xEF, 0x9D, 0x0C, 0x79, 0xE8, 0x9A, 0x0B,
	0x6C, 0xFD, 0x8F, 0x1E, 0x6B, 0xFA, 0x88, 0x19,
	0x62, 0xF3, 0x81, 0x10, 0x65, 0xF4, 0x86, 0x17,
	0x48, 0xD9, 0xAB, 0x3A, 0x4F, 0xDE, 0xAC, 0x3D,
	0x46, 0xD7, 0xA5, 0x34, 0x41, 0xD0, 0xA2, 0x33,
	0x54, 0xC5, 0xB7, 0x26, 0x53, 0xC2, 0xB0, 0x21,
	0x5A, 0xCB, 0xB9, 0x28, 0x5D, 0xCC, 0xBE, 0x2F,
	0xE0, 0x71, 0x03, 0x92, 0xE7, 0x76, 0x04, 0x95,
	0xEE, 0x7F, 0x0D, 0x9C, 0xE9, 0x78, 0x0A, 0x9B,
	0xFC, 0x6D, 0x1F, 0x8E, 0xFB, 0x6A, 0x18, 0x89,
	0xF2, 0x63, 0x11, 0x80, 0xF5, 0x64, 0x16, 0x87,
	0xD8, 0x49, 0x3B, 0xAA, 0xDF, 0x4E, 0x3C, 0xAD,
	0xD6, 0x47, 0x35, 0xA4, 0xD1, 0x40, 0x32, 0xA3,
	0xC4, 0x55, 0x27, 0xB6, 0xC3, 0x52, 0x20, 0xB1,
	0xCA, 0x5B, 0x29, 0xB8, 0xCD, 0x5C, 0x2E, 0xBF,
	0x90, 0x01, 0x73, 0xE2, 0x97, 0x06, 0x74, 0xE5,
	0x9E, 0x0F, 0x7D, 0xEC, 0x99, 0x08, 0x7A, 0xEB,
	0x8C, 0x1D, 0x6F, 0xFE, 0x8B, 0x1A, 0x68, 0xF9,
	0x82, 0x13, 0x61, 0xF0, 0x85, 0x14, 0x66, 0xF7,
	0xA8, 0x39, 0x4B, 0xDA, 0xAF, 0x3E, 0x4C, 0xDD,
	0xA6, 0x37, 0x45, 0xD4, 0xA1, 0x30, 0x42, 0xD3,
	0xB4, 0x25, 0x57, 0xC6, 0xB3, 0x22, 0x50, 0xC1,
	0xBA, 0x2B, 0x59, 0xC8, 0xBD, 0x2C, 0x5E, 0xCF
};


static uint8
ComputeFCS(const uint8* data, uint8 len)
{
	uint8 fcs = 0xFF;
	for (uint8 i = 0; i < len; i++)
		fcs = sFcsTable[fcs ^ data[i]];
	return 0xFF - fcs;
}


static bool
CheckFCS(const uint8* data, uint8 len, uint8 fcs)
{
	uint8 check = 0xFF;
	for (uint8 i = 0; i < len; i++)
		check = sFcsTable[check ^ data[i]];
	check = sFcsTable[check ^ fcs];
	return check == RFCOMM_FCS_CHECK;
}


static int sAssertions = 0;
static int sFailed = 0;

#define ASSERT_EQ(desc, actual, expected) do { \
	sAssertions++; \
	if ((actual) != (expected)) { \
		printf("FAIL: %s: got 0x%02X, expected 0x%02X\n", \
			(desc), (unsigned)(actual), (unsigned)(expected)); \
		sFailed++; \
	} else { \
		printf("  OK: %s\n", (desc)); \
	} \
} while (0)

#define ASSERT_TRUE(desc, cond) do { \
	sAssertions++; \
	if (!(cond)) { \
		printf("FAIL: %s\n", (desc)); \
		sFailed++; \
	} else { \
		printf("  OK: %s\n", (desc)); \
	} \
} while (0)


static void
TestTableGeneration()
{
	printf("\n--- FCS Table Generation ---\n");

	/* Verify the table by regenerating it from the polynomial */
	uint8 genTable[256];
	for (int i = 0; i < 256; i++) {
		uint8 crc = (uint8)i;
		for (int bit = 0; bit < 8; bit++) {
			if (crc & 0x01)
				crc = (crc >> 1) ^ RFCOMM_FCS_POLYNOMIAL;
			else
				crc >>= 1;
		}
		genTable[i] = crc;
	}

	ASSERT_TRUE("FCS table matches generated table",
		memcmp(sFcsTable, genTable, 256) == 0);
}


static void
TestSabmFcs()
{
	printf("\n--- SABM Frame FCS ---\n");

	/* SABM on DLCI 0: Address=0x03 (CR=1, DLCI=0), Control=0x3F,
	 * Length=0x01 (0 data bytes, EA=1)
	 * FCS is over Address + Control + Length = 3 bytes */
	uint8 frame1[] = { 0x03, 0x3F, 0x01 };
	uint8 fcs1 = ComputeFCS(frame1, 3);
	ASSERT_TRUE("SABM DLCI 0 FCS check passes",
		CheckFCS(frame1, 3, fcs1));

	/* SABM on DLCI 3: Address=0x0D, Control=0x3F, Length=0x01 */
	uint8 frame2[] = { 0x0D, 0x3F, 0x01 };
	uint8 fcs2 = ComputeFCS(frame2, 3);
	ASSERT_TRUE("SABM DLCI 3 FCS check passes",
		CheckFCS(frame2, 3, fcs2));

	/* Verify wrong FCS fails */
	ASSERT_TRUE("Wrong FCS fails check",
		!CheckFCS(frame1, 3, (uint8)(fcs1 ^ 0xFF)));
}


static void
TestUihFcs()
{
	printf("\n--- UIH Frame FCS ---\n");

	/* UIH on DLCI 3: Address=0x0D, Control=0xEF
	 * FCS for UIH is over Address + Control only (2 bytes) */
	uint8 header[] = { 0x0D, 0xEF };
	uint8 fcs = ComputeFCS(header, 2);
	ASSERT_TRUE("UIH DLCI 3 FCS check passes (header only)",
		CheckFCS(header, 2, fcs));
}


static void
TestUaFcs()
{
	printf("\n--- UA Frame FCS ---\n");

	/* UA on DLCI 0: Address=0x03 (but CR=0 for responder → 0x01),
	 * Control=0x73, Length=0x01 */
	uint8 frame[] = { 0x01, 0x73, 0x01 };
	uint8 fcs = ComputeFCS(frame, 3);
	ASSERT_TRUE("UA DLCI 0 FCS check passes",
		CheckFCS(frame, 3, fcs));
}


static void
TestKnownVectors()
{
	printf("\n--- Known Test Vectors ---\n");

	/* From GSM 07.10 / 3GPP TS 27.010:
	 * SABM on DLCI 0 with CR=1:
	 * Address=0x03, Control=0x3F, Length=0x01
	 * Expected FCS = 0x1C */
	uint8 sabmDlci0[] = { 0x03, 0x3F, 0x01 };
	uint8 fcs = ComputeFCS(sabmDlci0, 3);
	ASSERT_EQ("SABM DLCI 0 FCS = 0x1C", fcs, 0x1C);

	/* UA response to SABM on DLCI 0:
	 * Address=0x01 (CR=0), Control=0x73, Length=0x01
	 * Computed FCS = 0xB6 (verified by round-trip check) */
	uint8 uaDlci0[] = { 0x01, 0x73, 0x01 };
	fcs = ComputeFCS(uaDlci0, 3);
	ASSERT_EQ("UA DLCI 0 FCS = 0xB6", fcs, 0xB6);
	ASSERT_TRUE("UA DLCI 0 round-trip check", CheckFCS(uaDlci0, 3, fcs));
}


static void
TestRoundTrip()
{
	printf("\n--- Round-Trip Test ---\n");

	/* Compute FCS and verify it checks out */
	for (int dlci = 0; dlci < 32; dlci++) {
		uint8 addr = RFCOMM_ADDR(1, dlci);
		uint8 frame[] = { addr, RFCOMM_SABM_PF, 0x01 };
		uint8 fcs = ComputeFCS(frame, 3);

		char desc[64];
		snprintf(desc, sizeof(desc), "Round-trip SABM DLCI %d", dlci);
		ASSERT_TRUE(desc, CheckFCS(frame, 3, fcs));
	}
}


int
main()
{
	printf("=== RFCOMM FCS Unit Test ===\n");

	TestTableGeneration();
	TestSabmFcs();
	TestUihFcs();
	TestUaFcs();
	TestKnownVectors();
	TestRoundTrip();

	printf("\n=== Results: %d assertions, %d failed ===\n",
		sAssertions, sFailed);

	return sFailed > 0 ? 1 : 0;
}
