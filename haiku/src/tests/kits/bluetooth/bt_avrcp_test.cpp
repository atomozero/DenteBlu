/*
 * Copyright 2026 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_avrcp_test — Test AVRCP Target: connect to headphones and
 * print button events until user presses Enter.
 *
 * Usage: bt_avrcp_test <BD_ADDR>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Application.h>

#include <bluetooth/AvrcpTarget.h>
#include <bluetooth/bdaddrUtils.h>


static const char*
OpName(Bluetooth::avrcp_op_id op)
{
	switch (op) {
		case Bluetooth::AVRCP_OP_PLAY:		return "PLAY";
		case Bluetooth::AVRCP_OP_STOP:		return "STOP";
		case Bluetooth::AVRCP_OP_PAUSE:		return "PAUSE";
		case Bluetooth::AVRCP_OP_FORWARD:	return "FORWARD";
		case Bluetooth::AVRCP_OP_BACKWARD:	return "BACKWARD";
		case Bluetooth::AVRCP_OP_VOLUME_UP:	return "VOLUME_UP";
		case Bluetooth::AVRCP_OP_VOLUME_DOWN: return "VOLUME_DOWN";
		default:							return "UNKNOWN";
	}
}


static void
ButtonHandler(Bluetooth::avrcp_op_id op, bool pressed, void* cookie)
{
	printf("  Button: %-12s %s\n", OpName(op),
		pressed ? "PRESSED" : "RELEASED");
}


static void
VolumeHandler(uint8 volume, void* cookie)
{
	printf("  Volume: %u (%.0f%%)\n", volume, volume * 100.0 / 127.0);
}


int
main(int argc, char* argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <BD_ADDR>\n", argv[0]);
		fprintf(stderr, "Example: %s CC:A7:C1:F2:52:65\n", argv[0]);
		return 1;
	}

	BApplication app("application/x-vnd.Haiku-BTAvrcpTest");

	bdaddr_t addr = bdaddrUtils::FromString(argv[1]);
	printf("AVRCP Target test: connecting to %s\n", argv[1]);

	Bluetooth::AvrcpTarget avrcp;
	avrcp.SetButtonCallback(ButtonHandler, NULL);
	avrcp.SetVolumeCallback(VolumeHandler, NULL);

	status_t err = avrcp.Connect(addr);
	if (err != B_OK) {
		fprintf(stderr, "Connect failed: %s\n", strerror(err));
		return 1;
	}

	printf("Connected. Press buttons on headphones.\n");
	printf("Press Enter to disconnect and exit.\n\n");

	/* Wait for Enter key */
	getchar();

	printf("Disconnecting...\n");
	avrcp.Disconnect();
	printf("Done.\n");

	return 0;
}
