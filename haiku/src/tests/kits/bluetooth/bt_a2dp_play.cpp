/*
 * bt_a2dp_play — Play audio file to Bluetooth speaker via A2DP
 *
 * Decodes audio using ffmpeg (pipe) and streams PCM via A2dpSource.
 *
 * Usage: bt_a2dp_play <BD_ADDR> <audio_file>
 */

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <OS.h>

#include <bluetooth/A2dpSource.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/bdaddrUtils.h>

using namespace Bluetooth;

static volatile bool sQuit = false;

static void
SignalHandler(int)
{
	sQuit = true;
}


int
main(int argc, char** argv)
{
	if (argc < 3) {
		printf("Usage: %s <BD_ADDR> <audio_file>\n", argv[0]);
		printf("Example: %s 3C:8D:20:14:56:70 music.mp3\n", argv[0]);
		return 1;
	}

	signal(SIGINT, SignalHandler);
	signal(SIGTERM, SignalHandler);

	BApplication app("application/x-vnd.DenteBlu-A2dpPlay");

	bdaddr_t remote = bdaddrUtils::FromString(argv[1]);
	const char* filePath = argv[2];

	printf("A2DP Player: %s → %s\n\n", filePath, argv[1]);

	/* Connect A2DP */
	A2dpSource source;

	printf("Connecting...\n");
	status_t err = source.Connect(remote);
	if (err != B_OK) {
		printf("Connect failed: %s\n", strerror(err));
		_exit(1);
	}

	err = source.StartStream();
	if (err != B_OK) {
		printf("StartStream failed: %s\n", strerror(err));
		source.Disconnect();
		_exit(1);
	}

	printf("Connected! Starting playback...\n");

	/* Decode audio file to raw PCM via ffmpeg pipe
	 * Output: 44100Hz, stereo, signed 16-bit little-endian */
	char cmd[1024];
	snprintf(cmd, sizeof(cmd),
		"ffmpeg -i '%s' -f s16le -acodec pcm_s16le "
		"-ar 44100 -ac 2 - 2>/dev/null", filePath);

	FILE* pipe = popen(cmd, "r");
	if (pipe == NULL) {
		printf("Failed to run ffmpeg: %s\n", strerror(errno));
		source.Disconnect();
		_exit(1);
	}

	/* Stream PCM to A2DP */
	size_t spf = source.SamplesPerFrame();
	if (spf == 0) spf = 128;

	/* Send 5 frames per call for good throughput */
	size_t chunkSamples = spf * 5;
	uint8 channels = 2;
	size_t bufSize = chunkSamples * channels * sizeof(int16);
	int16* pcm = (int16*)malloc(bufSize);

	uint64 totalSamples = 0;
	size_t bytesRead;

	while (!sQuit) {
		bytesRead = fread(pcm, 1, bufSize, pipe);
		if (bytesRead == 0)
			break; /* EOF */

		/* Pad last chunk with silence if needed */
		if (bytesRead < bufSize)
			memset((uint8*)pcm + bytesRead, 0, bufSize - bytesRead);

		size_t samples = bytesRead / (channels * sizeof(int16));
		if (samples == 0) samples = chunkSamples;

		err = source.SendAudio(pcm, samples);
		if (err != B_OK) {
			printf("\nSendAudio failed: %s\n", strerror(err));
			break;
		}

		totalSamples += samples;

		/* Progress */
		if ((totalSamples % (44100 * 5)) < chunkSamples) {
			printf("\r  Playing... %lu:%02lu",
				(unsigned long)(totalSamples / 44100 / 60),
				(unsigned long)(totalSamples / 44100 % 60));
			fflush(stdout);
		}
	}

	printf("\n\nPlayback finished (%lu samples, %.1fs)\n",
		(unsigned long)totalSamples,
		(double)totalSamples / 44100.0);

	free(pcm);
	pclose(pipe);

	source.Disconnect();
	printf("Disconnected.\n");
	_exit(0);
}
