/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_a2dp_source_test — Integration test for A2DP Source.
 *
 * Usage:
 *   bt_a2dp_source_test <BD_ADDR>            Connect + discover endpoints
 *   bt_a2dp_source_test <BD_ADDR> --tone      Send 440Hz sine wave for 10s
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <OS.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/A2dpSource.h>

using Bluetooth::A2dpSource;


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


static void
PrintUsage(const char* prog)
{
	printf("Usage:\n"
		"  %s <BD_ADDR>          Connect + discover endpoints\n"
		"  %s <BD_ADDR> --tone   Send 440Hz sine wave (10s)\n",
		prog, prog);
}


static void
GenerateSine(int16* buf, size_t samples, uint8 channels,
	uint32 sampleRate, double freq, uint64 offset)
{
	for (size_t i = 0; i < samples; i++) {
		double t = (double)(offset + i) / (double)sampleRate;
		int16 val = (int16)(sin(2.0 * M_PI * freq * t) * 28000.0);
		for (uint8 ch = 0; ch < channels; ch++)
			buf[i * channels + ch] = val;
	}
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		PrintUsage(argv[0]);
		return 1;
	}

	bdaddr_t remote = bdaddrUtils::FromString(argv[1]);
	bdaddr_t null = bdaddrUtils::NullAddress();
	if (bdaddrUtils::Compare(remote, null)) {
		printf("Error: Invalid BD_ADDR '%s'\n", argv[1]);
		return 1;
	}

	bool toneMode = (argc >= 3 && strcmp(argv[2], "--tone") == 0);

	BApplication app("application/x-vnd.Haiku-bt_a2dp_source_test");

	A2dpSource source;

	printf("A2DP Source: Connecting to %s...\n", argv[1]);

	status_t err = source.Connect(remote);
	if (err != B_OK) {
		printf("Connect failed: %s (0x%08" B_PRIx32 ")\n",
			strerror(err), err);
		return 1;
	}

	printf("Connected! SBC frame=%u bytes, samples/frame=%u, "
		"%u Hz %u ch\n",
		source.FrameLength(), source.SamplesPerFrame(),
		source.SampleRate(), source.Channels());

	if (!toneMode) {
		printf("Discovery complete. Disconnecting.\n");
		source.Disconnect();
		return 0;
	}

	/* Tone mode: generate and send 440Hz sine wave for 10 seconds */
	printf("Starting 440Hz tone for 10 seconds...\n");

	err = source.StartStream();
	if (err != B_OK) {
		printf("StartStream failed: %s (0x%08" B_PRIx32 ")\n",
			strerror(err), err);
		source.Disconnect();
		return 1;
	}

	uint32 sampleRate = source.SampleRate();
	uint8 channels = source.Channels();
	uint16 samplesPerFrame = source.SamplesPerFrame();

	/* Send audio in chunks of samplesPerFrame * N */
	uint16 chunkSamples = samplesPerFrame * 4;
	int16* pcm = (int16*)malloc(chunkSamples * channels * sizeof(int16));
	if (pcm == NULL) {
		printf("Out of memory\n");
		source.StopStream();
		source.Disconnect();
		return 1;
	}

	uint64 totalSamples = (uint64)sampleRate * 10;
	uint64 sent = 0;

	while (sent < totalSamples) {
		size_t toSend = chunkSamples;
		if (sent + toSend > totalSamples)
			toSend = (size_t)(totalSamples - sent);

		GenerateSine(pcm, toSend, channels, sampleRate, 440.0, sent);

		err = source.SendAudio(pcm, toSend);
		if (err != B_OK) {
			printf("SendAudio failed: %s\n", strerror(err));
			break;
		}

		sent += toSend;

		/* Progress every second */
		if (sent % sampleRate < chunkSamples)
			printf("  %llu / %llu samples sent\n",
				(unsigned long long)sent,
				(unsigned long long)totalSamples);
	}

	free(pcm);
	source.StopStream();
	source.Disconnect();
	printf("Done.\n");

	return 0;
}
