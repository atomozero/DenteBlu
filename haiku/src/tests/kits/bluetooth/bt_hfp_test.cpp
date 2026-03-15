/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_hfp_test — Integration test for Hands-Free Profile client.
 *
 * Usage:
 *   bt_hfp_test <BD_ADDR>                 Connect + establish SLC
 *   bt_hfp_test <BD_ADDR> --channel <N>   Use explicit RFCOMM channel
 *   bt_hfp_test <BD_ADDR> --dial <number> Place a call
 *   bt_hfp_test <BD_ADDR> --at <command>  Send raw AT command
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>
#include <OS.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/HfpClient.h>

using Bluetooth::HfpClient;


static void
PrintUsage(const char* prog)
{
	printf("Usage:\n"
		"  %s <BD_ADDR>                   Connect + SLC\n"
		"  %s <BD_ADDR> --channel <N>     Use RFCOMM channel N\n"
		"  %s <BD_ADDR> --dial <number>   Place a call\n"
		"  %s <BD_ADDR> --at <command>    Send raw AT command\n",
		prog, prog, prog, prog);
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		PrintUsage(argv[0]);
		return 1;
	}

	// Parse BD_ADDR
	bdaddr_t remote = bdaddrUtils::FromString(argv[1]);
	bdaddr_t null = bdaddrUtils::NullAddress();
	if (bdaddrUtils::Compare(remote, null)) {
		printf("Error: Invalid BD_ADDR '%s'\n", argv[1]);
		return 1;
	}

	uint8 channel = 0;
	const char* dialNumber = NULL;
	const char* rawAt = NULL;

	// Parse options
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--channel") == 0 && i + 1 < argc) {
			channel = (uint8)atoi(argv[++i]);
		} else if (strcmp(argv[i], "--dial") == 0 && i + 1 < argc) {
			dialNumber = argv[++i];
		} else if (strcmp(argv[i], "--at") == 0 && i + 1 < argc) {
			rawAt = argv[++i];
		} else {
			PrintUsage(argv[0]);
			return 1;
		}
	}

	// BApplication needed for BMessage IPC with bluetooth_server
	BApplication app("application/x-vnd.Haiku-bt_hfp_test");

	printf("HFP Test: connecting to %s", argv[1]);
	if (channel > 0)
		printf(" on RFCOMM channel %u", channel);
	printf("...\n");

	HfpClient hfp;
	status_t err = hfp.Connect(remote, channel);
	if (err != B_OK) {
		printf("Connect failed: %s (0x%08" B_PRIx32 ")\n",
			strerror(err), err);
		return 1;
	}

	printf("RFCOMM connected. Establishing Service Level Connection...\n");

	err = hfp.EstablishServiceLevel();
	if (err != B_OK) {
		printf("SLC establishment failed: %s (0x%08" B_PRIx32 ")\n",
			strerror(err), err);
		hfp.Disconnect();
		return 1;
	}

	printf("SLC established! Remote AG features: 0x%04" B_PRIx32 "\n",
		hfp.RemoteFeatures());

	if (rawAt != NULL) {
		printf("Sending raw AT: %s\n", rawAt);
		char response[256];
		err = hfp.SendAt(rawAt, response, sizeof(response));
		if (err == B_OK) {
			printf("Response: %s\n", response);
		} else {
			printf("SendAt failed: %s (0x%08" B_PRIx32 ")\n",
				strerror(err), err);
		}
	}

	if (dialNumber != NULL) {
		printf("Dialing %s...\n", dialNumber);
		err = hfp.Dial(dialNumber);
		if (err != B_OK) {
			printf("Dial failed: %s (0x%08" B_PRIx32 ")\n",
				strerror(err), err);
		} else {
			printf("Dial command sent. Press Enter to hang up...\n");
			getchar();
			hfp.HangUp();
			printf("Hung up.\n");
		}
	} else if (rawAt == NULL) {
		// Just connected + SLC — wait for user to quit
		printf("SLC active. Press Enter to disconnect...\n");
		getchar();
	}

	hfp.Disconnect();
	printf("Disconnected.\n");
	return 0;
}
