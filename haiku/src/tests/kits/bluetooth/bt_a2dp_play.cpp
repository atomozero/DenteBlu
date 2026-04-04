/*
 * bt_a2dp_play — Play an audio file to Bluetooth A2DP headphones/speaker.
 *
 * Uses Haiku's BMediaFile to decode any supported format (MP3, WAV, OGG, etc.)
 * and sends PCM via A2dpSource to the remote device.
 *
 * Usage:
 *   bt_a2dp_play <BD_ADDR> <audio_file>
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <Application.h>
#include <MediaFile.h>
#include <MediaTrack.h>
#include <OS.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/A2dpSource.h>

using Bluetooth::A2dpSource;

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
		printf("Example: %s CC:A7:C1:F2:52:65 /boot/home/music.mp3\n",
			argv[0]);
		return 1;
	}

	signal(SIGINT, SignalHandler);
	signal(SIGTERM, SignalHandler);

	bdaddr_t remote = bdaddrUtils::FromString(argv[1]);
	if (bdaddrUtils::Compare(remote, bdaddrUtils::NullAddress())) {
		printf("Error: Invalid BD_ADDR '%s'\n", argv[1]);
		return 1;
	}

	const char* filePath = argv[2];

	BApplication app("application/x-vnd.DenteBlu-A2dpPlay");

	/* Open audio file */
	entry_ref ref;
	if (get_ref_for_path(filePath, &ref) != B_OK) {
		printf("Error: File not found '%s'\n", filePath);
		return 1;
	}

	BMediaFile mediaFile(&ref);
	if (mediaFile.InitCheck() != B_OK) {
		printf("Error: Cannot open media file '%s'\n", filePath);
		return 1;
	}

	/* Find audio track */
	BMediaTrack* track = NULL;
	media_format format;
	int32 trackCount = mediaFile.CountTracks();

	for (int32 i = 0; i < trackCount; i++) {
		track = mediaFile.TrackAt(i);
		if (track == NULL)
			continue;
		track->EncodedFormat(&format);
		if (format.type == B_MEDIA_RAW_AUDIO
				|| format.type == B_MEDIA_ENCODED_AUDIO) {
			break;
		}
		mediaFile.ReleaseTrack(track);
		track = NULL;
	}

	if (track == NULL) {
		printf("Error: No audio track found in '%s'\n", filePath);
		return 1;
	}

	/* Connect A2DP first — we need the negotiated sample rate/channels
	 * before requesting the decoded format from the media kit */
	A2dpSource source;
	status_t err;
	printf("Audio: %s\n", filePath);
	printf("\nConnecting to %s...\n", argv[1]);

	for (int attempt = 0; attempt < 5 && !sQuit; attempt++) {
		err = source.Connect(remote);
		if (err == B_OK)
			break;
		printf("  Attempt %d failed: %s\n", attempt + 1, strerror(err));
		if (!sQuit)
			snooze(2000000);
	}

	if (err != B_OK) {
		printf("Connect failed.\n");
		return 1;
	}

	err = source.StartStream();
	if (err != B_OK) {
		printf("StartStream failed: %s\n", strerror(err));
		source.Disconnect();
		return 1;
	}

	/* Now request decoded format matching the negotiated A2DP parameters.
	 * This ensures the media kit's decoder outputs PCM at the same rate
	 * and channel count that the SBC encoder expects. */
	uint32 sampleRate = source.SampleRate();
	uint8 channels = source.Channels();

	media_format decodedFormat;
	memset(&decodedFormat, 0, sizeof(decodedFormat));
	decodedFormat.type = B_MEDIA_RAW_AUDIO;
	decodedFormat.u.raw_audio.format = media_raw_audio_format::B_AUDIO_SHORT;
	decodedFormat.u.raw_audio.frame_rate = sampleRate;
	decodedFormat.u.raw_audio.channel_count = channels;
	decodedFormat.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;

	err = track->DecodedFormat(&decodedFormat);
	if (err != B_OK) {
		printf("Error: Cannot set decoded format: %s\n", strerror(err));
		source.Disconnect();
		return 1;
	}

	/* Check what the decoder actually provides */
	uint32 decodedRate = (uint32)decodedFormat.u.raw_audio.frame_rate;
	uint8 decodedChannels = decodedFormat.u.raw_audio.channel_count;
	if (decodedRate != sampleRate || decodedChannels != channels) {
		printf("WARNING: Decoder provides %u Hz %u ch, "
			"A2DP negotiated %u Hz %u ch\n",
			decodedRate, decodedChannels, sampleRate, channels);
	}

	printf("Format: %u Hz, %u channels (A2DP: %u Hz)\n",
		decodedRate, decodedChannels, sampleRate);

	int64 totalFrames = track->CountFrames();
	float duration = (float)totalFrames / sampleRate;
	printf("Duration: %.1f seconds\n", duration);
	printf("Connected! Streaming audio...\n\n");

	/* Read and send audio */
	size_t spf = source.SamplesPerFrame();
	if (spf == 0) spf = 128;

	/* Read buffer: large enough for the decoder's preferred buffer size */
	size_t decoderBufSize = decodedFormat.u.raw_audio.buffer_size;
	if (decoderBufSize == 0)
		decoderBufSize = 4096;
	size_t bufSamples = decoderBufSize / (channels * sizeof(int16));
	if (bufSamples < spf) bufSamples = spf;
	size_t bufSize = bufSamples * channels * sizeof(int16);

	/* Send chunk: exactly 4 SBC frames per SendAudio call — same as
	 * the working tone test.  This produces 1 RTP packet per call
	 * with consistent ~11ms pacing.  Larger/variable chunks cause
	 * burst sending that some receivers (Google Home) reject. */
	size_t sendChunk = spf * 4;
	int16* pcmBuf = (int16*)malloc(bufSize);
	if (pcmBuf == NULL) {
		source.Disconnect();
		return 1;
	}

	int64 totalSent = 0;
	int lastPercent = -1;

	/* Auto-normalize: scan first buffer to find peak, then compute
	 * gain to reach ~80% of full scale.  This handles both 8-bit WAV
	 * files (samples are multiples of 256) and quiet recordings. */
	float gain = 1.0f;
	{
		int16 scanBuf[8192];
		int64 scanFrames = 4096;
		int64 savedPos = track->CurrentFrame();
		track->ReadFrames(scanBuf, &scanFrames);
		track->SeekToFrame(&savedPos);

		int16 peak = 0;
		for (int64 i = 0; i < scanFrames * channels; i++) {
			int16 abs = scanBuf[i] < 0 ? -scanBuf[i] : scanBuf[i];
			if (abs > peak) peak = abs;
		}
		if (peak > 0 && peak < 26000) {
			gain = 26000.0f / (float)peak;
			printf("Auto-gain: peak=%d, gain=%.1fx (+%.0f dB)\n",
				peak, gain, 20.0f * log10f(gain));
		}
	}

	/* Resampling: if the decoder provides a different rate than A2DP
	 * needs, we resample with linear interpolation */
	bool needResample = (decodedRate != sampleRate);
	double resampleRatio = needResample
		? (double)sampleRate / (double)decodedRate : 1.0;
	if (needResample) {
		printf("Resampling: %u Hz → %u Hz (ratio %.4f)\n",
			decodedRate, sampleRate, resampleRatio);
	}

	/* Intermediate buffer for resampled/processed audio */
	size_t procMaxSamples = (size_t)(bufSamples * resampleRatio) + 256;
	int16* procBuf = (int16*)malloc(procMaxSamples * channels * sizeof(int16));
	size_t procAvail = 0; /* samples available in procBuf */

	while (!sQuit) {
		/* Read from decoder when procBuf needs more data */
		if (procAvail < sendChunk) {
			int64 framesToRead = bufSamples;
			err = track->ReadFrames(pcmBuf, &framesToRead);
			if ((err != B_OK && err != (status_t)0x80004007)
					|| framesToRead <= 0)
				break;

			/* Apply gain with clipping */
			for (int64 i = 0; i < framesToRead * channels; i++) {
				int32 sample = (int32)(pcmBuf[i] * gain);
				if (sample > 32767) sample = 32767;
				if (sample < -32768) sample = -32768;
				pcmBuf[i] = (int16)sample;
			}

			/* Resample if needed */
			if (needResample) {
				size_t outSamples = (size_t)(framesToRead * resampleRatio);
				if (procAvail + outSamples > procMaxSamples)
					outSamples = procMaxSamples - procAvail;

				for (size_t i = 0; i < outSamples; i++) {
					double srcPos = (double)i / resampleRatio;
					size_t idx = (size_t)srcPos;
					double frac = srcPos - idx;
					if (idx + 1 >= (size_t)framesToRead)
						idx = (size_t)framesToRead - 2;
					for (uint8 ch = 0; ch < channels; ch++) {
						int32 s0 = pcmBuf[idx * channels + ch];
						int32 s1 = pcmBuf[(idx + 1) * channels + ch];
						procBuf[(procAvail + i) * channels + ch]
							= (int16)(s0 + (int32)((s1 - s0) * frac));
					}
				}
				procAvail += outSamples;
			} else {
				memcpy(procBuf + procAvail * channels,
					pcmBuf, framesToRead * channels * sizeof(int16));
				procAvail += framesToRead;
			}
		}

		/* Send exactly sendChunk samples (= 4 SBC frames) */
		size_t toSend = sendChunk;
		if (toSend > procAvail)
			toSend = procAvail;
		if (toSend == 0)
			break;

		/* Diagnostic: dump first samples */
		if (totalSent == 0) {
			fprintf(stderr, "First PCM samples: ");
			for (int d = 0; d < 16 && d < (int)(toSend * channels); d++)
				fprintf(stderr, "%d ", procBuf[d]);
			fprintf(stderr, "\n");
		}

		err = source.SendAudio(procBuf, toSend);
		if (err != B_OK) {
			printf("\nSendAudio failed: %s\n", strerror(err));
			break;
		}

		/* Shift remaining data in procBuf */
		procAvail -= toSend;
		if (procAvail > 0) {
			memmove(procBuf, procBuf + toSend * channels,
				procAvail * channels * sizeof(int16));
		}

		totalSent += toSend;

		/* Progress */
		int percent = (totalFrames > 0)
			? (int)(totalSent * 100 / totalFrames) : 0;
		if (percent != lastPercent) {
			printf("\r  Playing: %d%% [%.1fs / %.1fs]",
				percent,
				(float)totalSent / sampleRate,
				duration);
			fflush(stdout);
			lastPercent = percent;
		}
	}

	printf("\n\nDone. Sent %.1f seconds of audio.\n",
		(float)totalSent / sampleRate);

	free(pcmBuf);
	free(procBuf);
	source.StopStream();
	source.Disconnect();
	return 0;
}
