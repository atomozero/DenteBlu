/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_hfp_ag_test — Test HFP Audio Gateway: listen for headphone
 * connection, handle SLC, print AT commands.
 *
 * Usage: bt_hfp_ag_test [timeout_seconds]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>

#include <bluetooth/HfpAudioGateway.h>


static void
DialHandler(const char* number, void* cookie)
{
	printf("  DIAL: %s\n", number);
}


static void
AnswerHandler(void* cookie)
{
	printf("  ANSWER\n");
}


static void
HangupHandler(void* cookie)
{
	printf("  HANGUP\n");
}


static void
SpeakerVolHandler(uint8 level, void* cookie)
{
	printf("  SPEAKER VOLUME: %u\n", level);
}


static void
MicVolHandler(uint8 level, void* cookie)
{
	printf("  MIC VOLUME: %u\n", level);
}


int
main(int argc, char* argv[])
{
	BApplication app("application/x-vnd.Haiku-BTHfpAgTest");

	bigtime_t timeout = 60000000;  /* 60 seconds default */
	if (argc >= 2)
		timeout = (bigtime_t)atoi(argv[1]) * 1000000;

	printf("HFP Audio Gateway test\n");
	printf("Listening for headphone connection (timeout %" B_PRId64 " s)...\n",
		timeout / 1000000);

	Bluetooth::HfpAudioGateway ag;
	ag.SetDialCallback(DialHandler, NULL);
	ag.SetAnswerCallback(AnswerHandler, NULL);
	ag.SetHangupCallback(HangupHandler, NULL);
	ag.SetSpeakerVolumeCallback(SpeakerVolHandler, NULL);
	ag.SetMicVolumeCallback(MicVolHandler, NULL);

	status_t err = ag.Listen(timeout);
	if (err != B_OK) {
		fprintf(stderr, "Listen failed: %s\n", strerror(err));
		return 1;
	}

	printf("Headphone connected.\n");

	/* Wait for SLC establishment */
	int retries = 0;
	while (!ag.IsSlcEstablished() && retries < 30) {
		snooze(500000);
		retries++;
	}

	if (ag.IsSlcEstablished()) {
		printf("SLC established! Remote features: 0x%08x\n",
			(unsigned)ag.RemoteFeatures());
	} else {
		printf("SLC not established after 15 seconds.\n");
	}

	printf("\nPress Enter to disconnect and exit.\n");
	getchar();

	printf("Disconnecting...\n");
	ag.Disconnect();
	printf("Done.\n");

	return 0;
}
