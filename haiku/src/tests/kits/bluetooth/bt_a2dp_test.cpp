/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_a2dp_test — Integration test for A2DP Sink.
 *
 * Usage:
 *   bt_a2dp_test --discover <BD_ADDR>    Discover remote AVDTP endpoints
 *   bt_a2dp_test --stream <BD_ADDR>      Full A2DP sink with audio
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <OS.h>
#include <SoundPlayer.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/A2dpSink.h>

using Bluetooth::A2dpSink;


/* Ring buffer for audio data between A2DP callback and BSoundPlayer */
#define RING_BUF_SIZE	(48000 * 2 * 2)		/* ~1 second of 48kHz stereo */

static int16 sRingBuf[RING_BUF_SIZE / sizeof(int16)];
static volatile uint32 sRingWrite = 0;
static volatile uint32 sRingRead = 0;
static volatile uint32 sSampleRate = 44100;
static volatile uint8 sChannels = 2;


static void
AudioDataCallback(const int16* pcm, size_t sampleCount,
	uint32 sampleRate, uint8 channels, void* cookie)
{
	sSampleRate = sampleRate;
	sChannels = channels;

	size_t totalSamples = sampleCount * channels;
	uint32 ringSize = RING_BUF_SIZE / sizeof(int16);

	for (size_t i = 0; i < totalSamples; i++) {
		uint32 next = (sRingWrite + 1) % ringSize;
		if (next == sRingRead)
			break;		/* ring full, drop samples */
		sRingBuf[sRingWrite] = pcm[i];
		sRingWrite = next;
	}
}


static void
SoundPlayerCallback(void* cookie, void* buffer, size_t size,
	const media_raw_audio_format& format)
{
	int16* out = (int16*)buffer;
	size_t frames = size / (sizeof(int16) * sChannels);
	uint32 ringSize = RING_BUF_SIZE / sizeof(int16);

	for (size_t f = 0; f < frames; f++) {
		for (uint8 ch = 0; ch < sChannels; ch++) {
			if (sRingRead != sRingWrite) {
				out[f * sChannels + ch] = sRingBuf[sRingRead];
				sRingRead = (sRingRead + 1) % ringSize;
			} else {
				out[f * sChannels + ch] = 0;	/* silence */
			}
		}
	}
}


static void
PrintUsage(const char* prog)
{
	printf("Usage:\n"
		"  %s --discover <BD_ADDR>    Discover AVDTP endpoints\n"
		"  %s --stream <BD_ADDR>      A2DP sink with audio playback\n",
		prog, prog);
}


int
main(int argc, char** argv)
{
	if (argc < 3) {
		PrintUsage(argv[0]);
		return 1;
	}

	const char* mode = argv[1];
	bdaddr_t remote = bdaddrUtils::FromString(argv[2]);
	bdaddr_t null = bdaddrUtils::NullAddress();
	if (bdaddrUtils::Compare(remote, null)) {
		printf("Error: Invalid BD_ADDR '%s'\n", argv[2]);
		return 1;
	}

	BApplication app("application/x-vnd.Haiku-bt_a2dp_test");

	if (strcmp(mode, "--discover") == 0) {
		printf("A2DP: Connecting to %s for endpoint discovery...\n",
			argv[2]);

		A2dpSink sink;
		status_t err = sink.Connect(remote);
		if (err != B_OK) {
			printf("Connect failed: %s (0x%08" B_PRIx32 ")\n",
				strerror(err), err);
			return 1;
		}

		printf("Connected! Stream info: %u Hz, %u channels\n",
			sink.SampleRate(), sink.Channels());

		sink.Disconnect();
		printf("Disconnected.\n");
		return 0;

	} else if (strcmp(mode, "--stream") == 0) {
		printf("A2DP: Connecting to %s for audio streaming...\n",
			argv[2]);

		A2dpSink sink;
		sink.SetAudioCallback(AudioDataCallback, NULL);

		status_t err = sink.Connect(remote);
		if (err != B_OK) {
			printf("Connect failed: %s (0x%08" B_PRIx32 ")\n",
				strerror(err), err);
			return 1;
		}

		printf("Connected. Starting stream...\n");
		err = sink.StartStream();
		if (err != B_OK) {
			printf("StartStream failed: %s (0x%08" B_PRIx32 ")\n",
				strerror(err), err);
			sink.Disconnect();
			return 1;
		}

		/* Wait a moment for the first audio data to arrive */
		snooze(500000);

		/* Set up BSoundPlayer for audio output */
		media_raw_audio_format format;
		format.frame_rate = sSampleRate;
		format.channel_count = sChannels;
		format.format = media_raw_audio_format::B_AUDIO_SHORT;
		format.byte_order = B_MEDIA_HOST_ENDIAN;
		format.buffer_size = 4096;

		BSoundPlayer player(&format, "A2DP Audio",
			SoundPlayerCallback);
		player.Start();
		player.SetHasData(true);

		printf("Streaming audio (%u Hz, %u ch). "
			"Press Enter to stop...\n", sSampleRate, sChannels);
		getchar();

		player.Stop();
		sink.StopStream();
		sink.Disconnect();
		printf("Done.\n");
		return 0;

	} else {
		PrintUsage(argv[0]);
		return 1;
	}
}
