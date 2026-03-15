/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_sbc_test — Unit tests for the SBC decoder.
 * Standalone (no libbluetooth.so required).
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "SbcDecoder.h"


static int sTestCount = 0;
static int sPassCount = 0;


static void
Check(bool condition, const char* label)
{
	sTestCount++;
	if (condition) {
		sPassCount++;
		printf("  PASS: %s\n", label);
	} else {
		printf("  FAIL: %s\n", label);
	}
}


static void
TestSyncwordDetection()
{
	printf("--- Syncword Detection ---\n");

	SbcDecoder decoder;
	int16 output[1024];

	/* Bad syncword */
	uint8 badFrame[] = {0x00, 0x00, 0x00, 0x00};
	ssize_t result = decoder.DecodeFrame(badFrame, sizeof(badFrame),
		output, 1024);
	Check(result < 0, "Bad syncword rejected");

	/* Too short */
	uint8 shortFrame[] = {0x9C};
	result = decoder.DecodeFrame(shortFrame, 1, output, 1024);
	Check(result < 0, "Too-short frame rejected");
}


static void
TestHeaderParsing()
{
	printf("--- Header Parsing ---\n");

	SbcDecoder decoder;
	int16 output[1024];

	/* Build a minimal valid SBC frame header:
	 * 0x9C = syncword
	 * byte1: freq=44.1kHz(10), blocks=16(11), mode=JointStereo(11),
	 *        alloc=Loudness(0), subbands=8(1)
	 *      = 0b10_11_11_0_1 = 0xBD
	 * byte2: bitpool = 32 = 0x20
	 * byte3: CRC (we'll set to 0 and the decoder should still try) */
	uint8 header[] = {0x9C, 0xBD, 0x20, 0x00};

	/* Frame will be too short for full decode, but header should parse */
	ssize_t result = decoder.DecodeFrame(header, sizeof(header),
		output, 1024);
	/* Will fail because frame too short, but header was parsed */
	Check(result < 0, "Incomplete frame returns error");

	/* After parsing header, check decoder state */
	Check(decoder.SampleRate() == 44100, "Sample rate 44100");
	Check(decoder.Channels() == 2, "Channels 2 (joint stereo)");
	Check(decoder.Subbands() == 8, "Subbands 8");
	Check(decoder.Blocks() == 16, "Blocks 16");
}


static void
TestFrameLengthCalculation()
{
	printf("--- Frame Length Calculation ---\n");

	SbcDecoder decoder;
	int16 output[1024];

	/* Parse a header first so FrameLength() is valid */
	/* Joint stereo, 44.1kHz, 16 blocks, 8 subbands, bitpool=53 */
	uint8 header[] = {0x9C, 0xBD, 0x35, 0x00};
	decoder.DecodeFrame(header, sizeof(header), output, 1024);

	/* For joint stereo: 4 + (blocks*bitpool + subbands + 4*subbands*2 + 7)/8
	 * = 4 + (16*53 + 8 + 64 + 7)/8 = 4 + (848 + 79)/8 = 4 + 115 = 119 */
	uint16 frameLen = decoder.FrameLength();
	printf("  Frame length for bitpool 53 = %u (expected ~119)\n",
		frameLen);
	Check(frameLen > 0, "Frame length > 0");
	Check(frameLen < 200, "Frame length < 200 (reasonable)");
}


static void
TestSilentFrame()
{
	printf("--- Silent Frame Decode ---\n");

	SbcDecoder decoder;

	/* Build a complete SBC frame of silence.
	 * Mono, 16kHz, 4 blocks, 4 subbands, SNR, bitpool=2
	 *
	 * byte1: freq=16kHz(00), blocks=4(00), mode=Mono(00),
	 *        alloc=SNR(1), subbands=4(0)
	 *      = 0b00_00_00_1_0 = 0x02
	 * byte2: bitpool = 2
	 *
	 * For mono: 4 + (4*1*2 + 4*4*1 + 7)/8 = 4 + (8+16+7)/8 = 4 + 3 = 7
	 * Actually framelength for mono = 4 + ceil((blocks * channels * bitpool
	 *   + 4 * subbands * channels) / 8)
	 * = 4 + ceil((4*1*2 + 4*4*1)/8) = 4 + ceil(24/8) = 4 + 3 = 7 */
	uint8 frame[32];
	memset(frame, 0, sizeof(frame));
	frame[0] = 0x9C;	/* syncword */
	frame[1] = 0x02;	/* 16kHz, 4 blocks, mono, SNR, 4 subbands */
	frame[2] = 0x02;	/* bitpool 2 */
	frame[3] = 0x00;	/* CRC placeholder */
	/* Scale factors and samples all zero */

	int16 output[256];
	memset(output, 0xFF, sizeof(output));

	ssize_t samples = decoder.DecodeFrame(frame, 32, output, 256);

	/* 4 blocks * 4 subbands = 16 samples */
	if (samples > 0) {
		printf("  Decoded %zd samples (expected 16)\n", samples);
		Check(samples == 16, "16 samples for 4 blocks * 4 subbands");
		Check(decoder.SampleRate() == 16000, "Sample rate 16000");
		Check(decoder.Channels() == 1, "Channels 1 (mono)");
	} else {
		printf("  Decode returned %zd\n", samples);
		Check(false, "Silent frame decode (returned error)");
	}
}


int
main()
{
	printf("=== SBC Decoder Unit Tests ===\n\n");

	TestSyncwordDetection();
	TestHeaderParsing();
	TestFrameLengthCalculation();
	TestSilentFrame();

	printf("\n=== Results: %d/%d passed ===\n",
		sPassCount, sTestCount);

	return (sPassCount == sTestCount) ? 0 : 1;
}
