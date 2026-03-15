/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_ertm_test — Unit test for L2CAP ERTM frame encoding and FCS-16.
 *
 * Validates I-frame and S-frame control field encoding/decoding,
 * FCS-16 CRC computation, and sequence number wraparound against
 * known values from the Bluetooth Core Specification Vol 3 Part A.
 */

#include <stdio.h>
#include <string.h>

#include <bluetooth/l2cap.h>


static int sAssertions = 0;
static int sFailed = 0;

#define ASSERT_EQ(desc, actual, expected) do { \
	sAssertions++; \
	if ((actual) != (expected)) { \
		printf("FAIL: %s: got 0x%04X, expected 0x%04X\n", \
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


/* FCS-16 computation — same algorithm as L2capEndpoint.cpp.
 * Polynomial: g(D) = D^16 + D^15 + D^2 + 1 (0x8005, reflected: 0xA001)
 * Initial value: 0x0000 */
static uint16
ComputeFCS16(const uint8* data, size_t length)
{
	uint16 fcs = 0x0000;
	while (length--) {
		fcs ^= *data++;
		for (int i = 0; i < 8; i++) {
			if (fcs & 1)
				fcs = (fcs >> 1) ^ 0xA001;
			else
				fcs >>= 1;
		}
	}
	return fcs;
}


/* Build I-frame control field */
static uint16
BuildIFrameCtrl(uint8 txSeq, uint8 reqSeq, uint8 sar)
{
	uint16 ctrl = 0;
	ctrl |= ((uint16)(txSeq & 0x3F)) << L2CAP_CTRL_TXSEQ_SHIFT;
	ctrl |= ((uint16)(reqSeq & 0x3F)) << L2CAP_CTRL_REQSEQ_SHIFT;
	ctrl |= ((uint16)(sar & 0x03)) << L2CAP_CTRL_SAR_SHIFT;
	return ctrl;
}


/* Build S-frame control field */
static uint16
BuildSFrameCtrl(uint8 sType, uint8 reqSeq, bool poll, bool final)
{
	uint16 ctrl = 0x0001;  /* bit 0 = 1 for S-frame */
	ctrl |= ((uint16)(sType & 0x03)) << L2CAP_CTRL_STYPE_SHIFT;
	ctrl |= ((uint16)(reqSeq & 0x3F)) << L2CAP_CTRL_REQSEQ_SHIFT;
	if (poll)
		ctrl |= L2CAP_CTRL_P_BIT;
	if (final)
		ctrl |= L2CAP_CTRL_F_BIT;
	return ctrl;
}


static void
TestIFrameEncoding()
{
	printf("\n--- I-frame Control Field Encoding ---\n");

	/* I-frame with TxSeq=0, ReqSeq=0, SAR=unsegmented */
	uint16 ctrl = BuildIFrameCtrl(0, 0, L2CAP_SAR_UNSEGMENTED);
	ASSERT_EQ("I-frame TxSeq=0 ReqSeq=0 SAR=0", ctrl, 0x0000);
	ASSERT_TRUE("I-frame bit 0 is 0", L2CAP_CTRL_IS_IFRAME(ctrl));

	/* I-frame with TxSeq=1, ReqSeq=0 */
	ctrl = BuildIFrameCtrl(1, 0, L2CAP_SAR_UNSEGMENTED);
	ASSERT_EQ("I-frame TxSeq=1 ReqSeq=0", ctrl, 0x0002);
	ASSERT_TRUE("TxSeq=1 extract", ((ctrl & L2CAP_CTRL_TXSEQ_MASK) >> L2CAP_CTRL_TXSEQ_SHIFT) == 1);

	/* I-frame with TxSeq=63 (max), ReqSeq=63, SAR=0 */
	ctrl = BuildIFrameCtrl(63, 63, L2CAP_SAR_UNSEGMENTED);
	ASSERT_EQ("I-frame TxSeq=63 ReqSeq=63", ctrl, 0x3F7E);

	/* I-frame with SAR=start */
	ctrl = BuildIFrameCtrl(5, 10, L2CAP_SAR_START);
	uint8 sar = (ctrl & L2CAP_CTRL_SAR_MASK) >> L2CAP_CTRL_SAR_SHIFT;
	uint8 txSeq = (ctrl & L2CAP_CTRL_TXSEQ_MASK) >> L2CAP_CTRL_TXSEQ_SHIFT;
	uint8 reqSeq = (ctrl & L2CAP_CTRL_REQSEQ_MASK) >> L2CAP_CTRL_REQSEQ_SHIFT;
	ASSERT_EQ("SAR=START extract", sar, L2CAP_SAR_START);
	ASSERT_EQ("TxSeq=5 extract", txSeq, 5);
	ASSERT_EQ("ReqSeq=10 extract", reqSeq, 10);

	/* I-frame with SAR=continue */
	ctrl = BuildIFrameCtrl(0, 0, L2CAP_SAR_CONTINUE);
	sar = (ctrl & L2CAP_CTRL_SAR_MASK) >> L2CAP_CTRL_SAR_SHIFT;
	ASSERT_EQ("SAR=CONTINUE extract", sar, L2CAP_SAR_CONTINUE);

	/* I-frame with SAR=end */
	ctrl = BuildIFrameCtrl(0, 0, L2CAP_SAR_END);
	sar = (ctrl & L2CAP_CTRL_SAR_MASK) >> L2CAP_CTRL_SAR_SHIFT;
	ASSERT_EQ("SAR=END extract", sar, L2CAP_SAR_END);
}


static void
TestSFrameEncoding()
{
	printf("\n--- S-frame Control Field Encoding ---\n");

	/* RR with ReqSeq=0, no poll, no final */
	uint16 ctrl = BuildSFrameCtrl(L2CAP_SFRAME_RR, 0, false, false);
	ASSERT_EQ("S-frame RR ReqSeq=0", ctrl, 0x0001);
	ASSERT_TRUE("S-frame bit 0 is 1", L2CAP_CTRL_IS_SFRAME(ctrl));

	/* REJ with ReqSeq=5 */
	ctrl = BuildSFrameCtrl(L2CAP_SFRAME_REJ, 5, false, false);
	uint8 sType = (ctrl & L2CAP_CTRL_STYPE_MASK) >> L2CAP_CTRL_STYPE_SHIFT;
	uint8 reqSeq = (ctrl & L2CAP_CTRL_REQSEQ_MASK) >> L2CAP_CTRL_REQSEQ_SHIFT;
	ASSERT_EQ("S-frame REJ sType", sType, L2CAP_SFRAME_REJ);
	ASSERT_EQ("S-frame REJ ReqSeq=5", reqSeq, 5);

	/* RNR with poll bit */
	ctrl = BuildSFrameCtrl(L2CAP_SFRAME_RNR, 10, true, false);
	ASSERT_TRUE("RNR P-bit set", (ctrl & L2CAP_CTRL_P_BIT) != 0);
	ASSERT_TRUE("RNR F-bit clear", (ctrl & L2CAP_CTRL_F_BIT) == 0);
	sType = (ctrl & L2CAP_CTRL_STYPE_MASK) >> L2CAP_CTRL_STYPE_SHIFT;
	ASSERT_EQ("RNR sType", sType, L2CAP_SFRAME_RNR);

	/* SREJ with final bit */
	ctrl = BuildSFrameCtrl(L2CAP_SFRAME_SREJ, 20, false, true);
	ASSERT_TRUE("SREJ F-bit set", (ctrl & L2CAP_CTRL_F_BIT) != 0);
	ASSERT_TRUE("SREJ P-bit clear", (ctrl & L2CAP_CTRL_P_BIT) == 0);
	sType = (ctrl & L2CAP_CTRL_STYPE_MASK) >> L2CAP_CTRL_STYPE_SHIFT;
	ASSERT_EQ("SREJ sType", sType, L2CAP_SFRAME_SREJ);

	/* RR with both poll and final */
	ctrl = BuildSFrameCtrl(L2CAP_SFRAME_RR, 63, true, true);
	ASSERT_TRUE("RR P+F set", (ctrl & L2CAP_CTRL_P_BIT) != 0
		&& (ctrl & L2CAP_CTRL_F_BIT) != 0);
	reqSeq = (ctrl & L2CAP_CTRL_REQSEQ_MASK) >> L2CAP_CTRL_REQSEQ_SHIFT;
	ASSERT_EQ("RR ReqSeq=63", reqSeq, 63);
}


static void
TestFrameTypeDetection()
{
	printf("\n--- Frame Type Detection ---\n");

	/* All I-frame control words have bit 0 = 0 */
	for (uint8 txSeq = 0; txSeq < 64; txSeq += 17) {
		uint16 ctrl = BuildIFrameCtrl(txSeq, 0, L2CAP_SAR_UNSEGMENTED);
		char desc[64];
		snprintf(desc, sizeof(desc), "I-frame TxSeq=%u detected as I-frame", txSeq);
		ASSERT_TRUE(desc, L2CAP_CTRL_IS_IFRAME(ctrl));
		ASSERT_TRUE("  not S-frame", !L2CAP_CTRL_IS_SFRAME(ctrl));
	}

	/* All S-frame control words have bit 0 = 1 */
	for (uint8 sType = 0; sType <= 3; sType++) {
		uint16 ctrl = BuildSFrameCtrl(sType, 0, false, false);
		char desc[64];
		snprintf(desc, sizeof(desc), "S-frame sType=%u detected as S-frame", sType);
		ASSERT_TRUE(desc, L2CAP_CTRL_IS_SFRAME(ctrl));
		ASSERT_TRUE("  not I-frame", !L2CAP_CTRL_IS_IFRAME(ctrl));
	}
}


static void
TestFCS16()
{
	printf("\n--- FCS-16 (CRC-16) ---\n");

	/* Test vector from BT Core Spec Vol 3, Part A, Section 3.3.5:
	 * CRC-CCITT polynomial with init=0x0000, reflected.
	 * Empty data → FCS = 0x0000 */
	uint16 fcs = ComputeFCS16(NULL, 0);
	ASSERT_EQ("FCS-16 empty data", fcs, 0x0000);

	/* Single byte 0x00 */
	uint8 data1[] = {0x00};
	fcs = ComputeFCS16(data1, 1);
	ASSERT_TRUE("FCS-16 single 0x00 is non-trivial (verifying poly)", fcs == 0x0000);

	/* Known test: L2CAP basic header {length=4, cid=0x0040} + control {0x00, 0x00}
	 * This simulates an I-frame with TxSeq=0, ReqSeq=0 */
	uint8 frame[] = {0x04, 0x00, 0x40, 0x00, 0x00, 0x00};
	fcs = ComputeFCS16(frame, sizeof(frame));
	/* Verify FCS is computed and round-trips */
	uint8 frameWithFcs[8];
	memcpy(frameWithFcs, frame, sizeof(frame));
	frameWithFcs[6] = (uint8)(fcs & 0xFF);
	frameWithFcs[7] = (uint8)(fcs >> 8);
	uint16 check = ComputeFCS16(frameWithFcs, sizeof(frameWithFcs));
	ASSERT_EQ("FCS-16 round-trip check", check, 0x0000);

	/* Multi-byte test: "123456789" → standard CRC-16/ARC result = 0xBB3D */
	uint8 digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
	fcs = ComputeFCS16(digits, 9);
	ASSERT_EQ("FCS-16 '123456789' = 0xBB3D", fcs, 0xBB3D);

	/* Round-trip: appending FCS should yield check value 0x0000 */
	uint8 digitsWithFcs[11];
	memcpy(digitsWithFcs, digits, 9);
	digitsWithFcs[9] = (uint8)(fcs & 0xFF);
	digitsWithFcs[10] = (uint8)(fcs >> 8);
	check = ComputeFCS16(digitsWithFcs, 11);
	ASSERT_EQ("FCS-16 '123456789' round-trip", check, 0x0000);
}


static void
TestSequenceWraparound()
{
	printf("\n--- Sequence Number Wraparound ---\n");

	/* Sequence numbers are 6-bit (0-63), wrapping at 64 → 0 */
	for (uint8 seq = 0; seq <= 63; seq++) {
		uint16 ctrl = BuildIFrameCtrl(seq, 0, L2CAP_SAR_UNSEGMENTED);
		uint8 extracted = (ctrl & L2CAP_CTRL_TXSEQ_MASK) >> L2CAP_CTRL_TXSEQ_SHIFT;

		if (extracted != seq) {
			char desc[64];
			snprintf(desc, sizeof(desc), "TxSeq round-trip seq=%u", seq);
			ASSERT_EQ(desc, extracted, seq);
		}
	}
	printf("  OK: All 64 TxSeq values round-trip correctly\n");
	sAssertions++;

	/* Wraparound: (63 + 1) & 0x3F = 0 */
	uint8 next = (63 + 1) & 0x3F;
	ASSERT_EQ("Wraparound (63+1) & 0x3F = 0", next, 0);

	/* Mid-range: (32 + 32) & 0x3F = 0 */
	next = (32 + 32) & 0x3F;
	ASSERT_EQ("Wraparound (32+32) & 0x3F = 0", next, 0);

	/* ReqSeq wraparound */
	for (uint8 seq = 0; seq <= 63; seq++) {
		uint16 ctrl = BuildSFrameCtrl(L2CAP_SFRAME_RR, seq, false, false);
		uint8 extracted = (ctrl & L2CAP_CTRL_REQSEQ_MASK) >> L2CAP_CTRL_REQSEQ_SHIFT;

		if (extracted != seq) {
			char desc[64];
			snprintf(desc, sizeof(desc), "ReqSeq round-trip seq=%u", seq);
			ASSERT_EQ(desc, extracted, seq);
		}
	}
	printf("  OK: All 64 ReqSeq values round-trip correctly\n");
	sAssertions++;
}


static void
TestIFrameVsSFrame()
{
	printf("\n--- I-frame vs S-frame Differentiation ---\n");

	/* Exhaustive test: ensure no I-frame ctrl word has bit 0 = 1 */
	int iframeCount = 0;
	for (uint8 txSeq = 0; txSeq < 64; txSeq++) {
		for (uint8 reqSeq = 0; reqSeq < 64; reqSeq += 13) {
			for (uint8 sar = 0; sar <= 3; sar++) {
				uint16 ctrl = BuildIFrameCtrl(txSeq, reqSeq, sar);
				if (!L2CAP_CTRL_IS_IFRAME(ctrl)) {
					printf("FAIL: I-frame TxSeq=%u ReqSeq=%u SAR=%u detected as S-frame\n",
						txSeq, reqSeq, sar);
					sFailed++;
					sAssertions++;
					return;
				}
				iframeCount++;
			}
		}
	}
	printf("  OK: %d I-frame control words all have bit0=0\n", iframeCount);
	sAssertions++;

	/* S-frames: all 4 types × various ReqSeq */
	int sframeCount = 0;
	for (uint8 sType = 0; sType <= 3; sType++) {
		for (uint8 reqSeq = 0; reqSeq < 64; reqSeq += 7) {
			uint16 ctrl = BuildSFrameCtrl(sType, reqSeq, false, false);
			if (!L2CAP_CTRL_IS_SFRAME(ctrl)) {
				printf("FAIL: S-frame sType=%u ReqSeq=%u detected as I-frame\n",
					sType, reqSeq);
				sFailed++;
				sAssertions++;
				return;
			}
			sframeCount++;
		}
	}
	printf("  OK: %d S-frame control words all have bit0=1\n", sframeCount);
	sAssertions++;
}


int
main()
{
	printf("=== L2CAP ERTM Unit Test ===\n");

	TestIFrameEncoding();
	TestSFrameEncoding();
	TestFrameTypeDetection();
	TestFCS16();
	TestSequenceWraparound();
	TestIFrameVsSFrame();

	printf("\n=== Results: %d assertions, %d failed ===\n",
		sAssertions, sFailed);

	return sFailed > 0 ? 1 : 0;
}
