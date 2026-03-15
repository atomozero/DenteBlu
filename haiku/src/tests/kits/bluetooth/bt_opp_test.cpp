/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_opp_test — OPP (Object Push Profile) integration test.
 *
 * Usage:
 *   bt_opp_test <BD_ADDR> --push <file>
 *   bt_opp_test <BD_ADDR> --push-text "Hello from Haiku"
 *   bt_opp_test <BD_ADDR> --channel <N> --push <file>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>

#include <bluetooth/OppClient.h>
#include <bluetooth/bdaddrUtils.h>


static void
Usage(const char* progName)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s <BD_ADDR> --push <file>\n"
		"  %s <BD_ADDR> --push-text \"message\"\n"
		"  %s <BD_ADDR> --channel <N> --push <file>\n"
		"\nOptions:\n"
		"  --push <file>       Push a file to the remote device\n"
		"  --push-text <msg>   Push a text message as text/plain\n"
		"  --channel <N>       Use RFCOMM channel N (skip SDP query)\n",
		progName, progName, progName);
}


int
main(int argc, char* argv[])
{
	if (argc < 3) {
		Usage(argv[0]);
		return 1;
	}

	// Parse BD_ADDR
	bdaddr_t remote = bdaddrUtils::FromString(argv[1]);
	if (bdaddrUtils::Compare(remote, bdaddrUtils::NullAddress())) {
		fprintf(stderr, "Invalid BD_ADDR: %s\n", argv[1]);
		return 1;
	}

	// Parse options
	const char* pushFile = NULL;
	const char* pushText = NULL;
	uint8 channel = 0;

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--push") == 0 && i + 1 < argc) {
			pushFile = argv[++i];
		} else if (strcmp(argv[i], "--push-text") == 0 && i + 1 < argc) {
			pushText = argv[++i];
		} else if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
			channel = (uint8)atoi(argv[++i]);
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[i]);
			Usage(argv[0]);
			return 1;
		}
	}

	if (pushFile == NULL && pushText == NULL) {
		fprintf(stderr, "Error: specify --push or --push-text\n");
		Usage(argv[0]);
		return 1;
	}

	// BApplication required for BMessenger IPC
	BApplication app("application/x-vnd.Haiku-bt_opp_test");

	Bluetooth::OppClient opp;

	printf("Connecting to %s (channel %u)...\n",
		bdaddrUtils::ToString(remote).String(), channel);

	status_t result = opp.Connect(remote, channel);
	if (result != B_OK) {
		fprintf(stderr, "OPP Connect failed: %s (0x%08" B_PRIx32 ")\n",
			strerror(result), result);
		return 1;
	}

	printf("OPP session established.\n");

	if (pushFile != NULL) {
		printf("Pushing file: %s\n", pushFile);
		result = opp.PushFile(pushFile);
	} else {
		printf("Pushing text: \"%s\"\n", pushText);
		result = opp.PushData("message.txt", "text/plain",
			(const uint8*)pushText, strlen(pushText));
	}

	if (result == B_OK)
		printf("Push successful!\n");
	else
		fprintf(stderr, "Push failed: %s (0x%08" B_PRIx32 ")\n",
			strerror(result), result);

	opp.Disconnect();
	printf("Disconnected.\n");

	return (result == B_OK) ? 0 : 1;
}
