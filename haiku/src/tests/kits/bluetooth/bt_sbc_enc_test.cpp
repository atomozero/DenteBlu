/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_sbc_enc_test — Unit tests for the SBC encoder.
 * Tests encoding, roundtrip encode→decode, and various configurations.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "SbcDecoder.h"
#include "SbcEncoder.h"


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


/* ========================================================================= */

static void
TestConfigure()
{
	printf("--- Configure ---\n");

	SbcEncoder enc;

	/* Valid configurations */
	Check(enc.Configure(44100, 2, 16, 8, 3, 0, 32) == B_OK,
		"44.1kHz stereo joint 16/8 bp32");
	Check(enc.Configure(48000, 2, 16, 8, 3, 0, 53) == B_OK,
		"48kHz stereo joint 16/8 bp53");
	Check(enc.Configure(16000, 1, 4, 4, 0, 1, 2) == B_OK,
		"16kHz mono 4/4 SNR bp2");
	Check(enc.Configure(32000, 2, 8, 8, 2, 0, 45) == B_OK,
		"32kHz stereo 8/8 bp45");

	/* Invalid configurations */
	Check(enc.Configure(22050, 2, 16, 8, 3, 0, 32) != B_OK,
		"22050Hz rejected");
	Check(enc.Configure(44100, 2, 16, 6, 3, 0, 32) != B_OK,
		"6 subbands rejected");
	Check(enc.Configure(44100, 2, 10, 8, 3, 0, 32) != B_OK,
		"10 blocks rejected");
	Check(enc.Configure(44100, 1, 16, 8, 3, 0, 32) != B_OK,
		"1ch with joint stereo rejected");
}


static void
TestFrameLength()
{
	printf("--- Frame Length ---\n");

	SbcEncoder enc;

	/* Joint stereo: 4 + (blocks*bitpool + subbands + 4*subbands*2 + 7)/8 */
	enc.Configure(44100, 2, 16, 8, 3, 0, 53);
	uint16 fl = enc.FrameLength();
	/* = 4 + (16*53 + 8 + 64 + 7)/8 = 4 + 927/8 = 4 + 115 = 119 */
	printf("  Joint stereo bp53: %u (expect 119)\n", fl);
	Check(fl == 119, "Joint stereo frame length == 119");

	/* Mono: 4 + (blocks*1*bitpool + 4*subbands*1 + 7)/8 */
	enc.Configure(16000, 1, 4, 4, 0, 1, 2);
	fl = enc.FrameLength();
	/* = 4 + (4*1*2 + 4*4*1 + 7)/8 = 4 + 31/8 = 4 + 3 = 7 */
	printf("  Mono bp2: %u (expect 7)\n", fl);
	Check(fl == 7, "Mono frame length == 7");

	/* Stereo (non-joint): 4 + (blocks*bitpool + 4*subbands*2 + 7)/8 */
	enc.Configure(44100, 2, 16, 8, 2, 0, 32);
	fl = enc.FrameLength();
	/* = 4 + (16*32 + 64 + 7)/8 = 4 + 583/8 = 4 + 72 = 76 */
	printf("  Stereo bp32: %u (expect 76)\n", fl);
	Check(fl == 76, "Stereo frame length == 76");
}


static void
TestEncodeSilence()
{
	printf("--- Encode Silence ---\n");

	SbcEncoder enc;
	enc.Configure(44100, 2, 16, 8, 3, 0, 32);

	/* Silent input: all zeros */
	int16 pcm[16 * 8 * 2];
	memset(pcm, 0, sizeof(pcm));

	uint8 frame[256];
	ssize_t len = enc.EncodeFrame(pcm, frame, sizeof(frame));

	Check(len > 0, "Encode silence returns positive");
	Check(len == enc.FrameLength(), "Length matches FrameLength()");
	Check(frame[0] == 0x9C, "Syncword 0x9C");
	printf("  Encoded %zd bytes\n", len);
}


static void
TestHeaderEncoding()
{
	printf("--- Header Encoding ---\n");

	SbcEncoder enc;

	/* 44.1kHz, 16 blocks, joint stereo, loudness, 8 subbands, bp32 */
	enc.Configure(44100, 2, 16, 8, 3, 0, 32);

	int16 pcm[16 * 8 * 2];
	memset(pcm, 0, sizeof(pcm));
	uint8 frame[256];
	enc.EncodeFrame(pcm, frame, sizeof(frame));

	/* byte1: freq=44.1kHz(10), blocks=16(11), mode=JS(11),
	 *        alloc=Loudness(0), subbands=8(1) = 0xBD */
	printf("  byte1=0x%02X (expect 0xBD)\n", frame[1]);
	Check(frame[1] == 0xBD, "Header byte1 correct (0xBD)");

	/* byte2: bitpool = 32 = 0x20 */
	printf("  byte2=0x%02X (expect 0x20)\n", frame[2]);
	Check(frame[2] == 0x20, "Bitpool byte correct (0x20)");

	/* 48kHz, 8 blocks, mono, SNR, 4 subbands, bp10 */
	enc.Configure(48000, 1, 8, 4, 0, 1, 10);
	memset(pcm, 0, 8 * 4 * 1 * sizeof(int16));
	enc.EncodeFrame(pcm, frame, sizeof(frame));

	/* byte1: freq=48kHz(11), blocks=8(01), mode=mono(00),
	 *        alloc=SNR(1), subbands=4(0) = 0b11_01_00_1_0 = 0xD2 */
	printf("  byte1=0x%02X (expect 0xD2)\n", frame[1]);
	Check(frame[1] == 0xD2, "Header 48kHz/mono/SNR/4sb (0xD2)");
}


static void
TestRoundtripSilence()
{
	printf("--- Roundtrip Silence ---\n");

	SbcEncoder enc;
	SbcDecoder dec;

	enc.Configure(44100, 2, 16, 8, 3, 0, 32);

	int16 pcm_in[16 * 8 * 2];
	memset(pcm_in, 0, sizeof(pcm_in));

	uint8 frame[256];
	ssize_t encLen = enc.EncodeFrame(pcm_in, frame, sizeof(frame));
	Check(encLen > 0, "Encode succeeded");

	int16 pcm_out[16 * 8 * 2];
	memset(pcm_out, 0xFF, sizeof(pcm_out));
	ssize_t decSamples = dec.DecodeFrame(frame, encLen,
		pcm_out, sizeof(pcm_out) / sizeof(int16));
	Check(decSamples > 0, "Decode succeeded");

	/* All output should be near zero */
	int32 maxAbs = 0;
	for (int i = 0; i < decSamples * 2; i++) {
		int32 a = pcm_out[i];
		if (a < 0) a = -a;
		if (a > maxAbs) maxAbs = a;
	}
	printf("  Max abs output: %d (expect < 100)\n", (int)maxAbs);
	Check(maxAbs < 100, "Silence roundtrip max abs < 100");
}


static void
TestRoundtripSineWave()
{
	printf("--- Roundtrip Sine Wave ---\n");

	SbcEncoder enc;
	SbcDecoder dec;

	/* 44.1kHz, stereo (non-joint), 16 blocks, 8 subbands, bp53 */
	enc.Configure(44100, 2, 16, 8, 2, 0, 53);

	int samplesPerFrame = 16 * 8; /* blocks * subbands */
	int totalFrames = 8;

	/* Generate 1kHz sine wave at ~70% amplitude */
	double freq = 1000.0;
	double rate = 44100.0;
	double amplitude = 0.7 * 32767;

	int16 pcm_in[16 * 8 * 2]; /* stereo */
	int16 pcm_out[16 * 8 * 2];
	uint8 frame[256];
	ssize_t encLen = 0;

	/* Encode and decode multiple frames for warmup */
	for (int f = 0; f < totalFrames; f++) {
		for (int i = 0; i < samplesPerFrame; i++) {
			int globalSample = f * samplesPerFrame + i;
			int16 sample = (int16)(amplitude
				* sin(2.0 * M_PI * freq * globalSample / rate));
			pcm_in[i * 2 + 0] = sample;
			pcm_in[i * 2 + 1] = sample;
		}

		encLen = enc.EncodeFrame(pcm_in, frame, sizeof(frame));
		dec.DecodeFrame(frame, encLen,
			pcm_out, sizeof(pcm_out) / sizeof(int16));
	}

	/* Show last frame input vs output */
	printf("  Input[0..7]:    ");
	for (int i = 0; i < 8; i++)
		printf("%6d ", pcm_in[i * 2]);
	printf("\n  Output[0..7]:   ");
	for (int i = 0; i < 8; i++)
		printf("%6d ", pcm_out[i * 2]);
	printf("\n  Input[80..87]:  ");
	for (int i = 80; i < 88 && i < samplesPerFrame; i++)
		printf("%6d ", pcm_in[i * 2]);
	printf("\n  Output[80..87]: ");
	for (int i = 80; i < 88 && i < samplesPerFrame; i++)
		printf("%6d ", pcm_out[i * 2]);
	printf("\n");

	/* Compute SNR on last frame, trying different delay offsets
	 * to account for filter bank group delay (~72 samples).
	 * Compare output with a regenerated reference at the
	 * delayed position. */
	double bestSnr = -999;
	int bestDelay = 0;
	for (int delay = 0; delay < samplesPerFrame; delay++) {
		double sigP = 0, noiseP = 0;
		for (int i = 0; i < samplesPerFrame; i++) {
			/* Reference: the sine wave at (global_sample - delay) */
			int globalRef = (totalFrames - 1) * samplesPerFrame + i;
			int globalDel = globalRef - delay;
			double ref = amplitude
				* sin(2.0 * M_PI * freq * globalDel / rate);
			double decoded = pcm_out[i * 2];
			sigP += ref * ref;
			noiseP += (ref - decoded) * (ref - decoded);
		}
		double snr = 10.0 * log10(sigP / (noiseP + 1e-10));
		if (snr > bestSnr) {
			bestSnr = snr;
			bestDelay = delay;
		}
	}
	double snr = bestSnr;

	Check(encLen > 0, "Sine encode succeeded");
	Check(encLen == 118, "Frame length 118 (bp53 stereo)");

	printf("  SNR (frame %d, delay %d): %.1f dB (expect > 10 dB)\n",
		totalFrames, bestDelay, snr);
	Check(snr > 10.0, "Sine roundtrip SNR > 10 dB");
}


static void
TestRoundtripMono()
{
	printf("--- Roundtrip Mono ---\n");

	SbcEncoder enc;
	SbcDecoder dec;

	/* 16kHz mono, 4 blocks, 4 subbands, SNR, bp10 */
	enc.Configure(16000, 1, 4, 4, 0, 1, 10);

	int samplesPerFrame = 4 * 4;
	int16 pcm_in[16];

	/* Generate a ramp signal */
	for (int i = 0; i < samplesPerFrame; i++)
		pcm_in[i] = (int16)(i * 2000 - 15000);

	uint8 frame[64];
	ssize_t encLen = enc.EncodeFrame(pcm_in, frame, sizeof(frame));
	Check(encLen > 0, "Mono encode succeeded");
	Check(frame[0] == 0x9C, "Syncword correct");

	int16 pcm_out[64];
	ssize_t decSamples = dec.DecodeFrame(frame, encLen,
		pcm_out, 64);
	Check(decSamples == samplesPerFrame, "Mono decoded sample count");

	/* Check decoder reports correct parameters */
	Check(dec.SampleRate() == 16000, "Decoded rate 16000");
	Check(dec.Channels() == 1, "Decoded 1 channel");
}


static void
TestMultipleFrames()
{
	printf("--- Multiple Frames ---\n");

	SbcEncoder enc;
	SbcDecoder dec;

	/* Test that encoder state persists correctly across frames */
	enc.Configure(44100, 2, 16, 8, 3, 0, 32);

	int samplesPerFrame = 16 * 8;
	double freq = 440.0;
	double rate = 44100.0;
	double amplitude = 0.5 * 32767;

	uint8 frame[256];
	int16 pcm_in[16 * 8 * 2];
	int16 pcm_out[16 * 8 * 2];

	bool allOk = true;
	for (int f = 0; f < 5; f++) {
		for (int i = 0; i < samplesPerFrame; i++) {
			int globalSample = f * samplesPerFrame + i;
			int16 sample = (int16)(amplitude
				* sin(2.0 * M_PI * freq * globalSample / rate));
			pcm_in[i * 2 + 0] = sample;
			pcm_in[i * 2 + 1] = sample;
		}

		ssize_t encLen = enc.EncodeFrame(pcm_in, frame, sizeof(frame));
		if (encLen <= 0) {
			allOk = false;
			break;
		}

		ssize_t decSamples = dec.DecodeFrame(frame, encLen,
			pcm_out, sizeof(pcm_out) / sizeof(int16));
		if (decSamples != samplesPerFrame) {
			allOk = false;
			break;
		}
	}

	Check(allOk, "5 consecutive frames encode+decode OK");
}


static void
TestDualChannel()
{
	printf("--- Dual Channel ---\n");

	SbcEncoder enc;
	SbcDecoder dec;

	enc.Configure(44100, 2, 16, 8, 1, 0, 32);

	int samplesPerFrame = 16 * 8;
	int16 pcm_in[16 * 8 * 2];

	/* Left = sine, Right = different sine */
	for (int i = 0; i < samplesPerFrame; i++) {
		pcm_in[i * 2 + 0] = (int16)(10000
			* sin(2.0 * M_PI * 440 * i / 44100.0));
		pcm_in[i * 2 + 1] = (int16)(10000
			* sin(2.0 * M_PI * 880 * i / 44100.0));
	}

	uint8 frame[512];
	ssize_t encLen = enc.EncodeFrame(pcm_in, frame, sizeof(frame));
	Check(encLen > 0, "Dual channel encode succeeded");

	int16 pcm_out[16 * 8 * 2];
	ssize_t decSamples = dec.DecodeFrame(frame, encLen,
		pcm_out, sizeof(pcm_out) / sizeof(int16));
	Check(decSamples == samplesPerFrame, "Dual channel decode OK");
}


static void
TestBufferTooSmall()
{
	printf("--- Buffer Too Small ---\n");

	SbcEncoder enc;
	enc.Configure(44100, 2, 16, 8, 3, 0, 32);

	int16 pcm[16 * 8 * 2];
	memset(pcm, 0, sizeof(pcm));

	uint8 frame[4]; /* too small */
	ssize_t len = enc.EncodeFrame(pcm, frame, sizeof(frame));
	Check(len < 0, "Too small buffer rejected");
}


int
main()
{
	printf("=== SBC Encoder Unit Tests ===\n\n");

	TestConfigure();
	TestFrameLength();
	TestEncodeSilence();
	TestHeaderEncoding();
	TestRoundtripSilence();
	TestRoundtripSineWave();
	TestRoundtripMono();
	TestMultipleFrames();
	TestDualChannel();
	TestBufferTooSmall();

	printf("\n=== Results: %d/%d passed ===\n",
		sPassCount, sTestCount);

	return (sPassCount == sTestCount) ? 0 : 1;
}
