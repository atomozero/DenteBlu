/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * Test for HCI LE command struct sizes and OCF values.
 */

#include <stdio.h>
#include <string.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_command_le.h>

#include <CommandManager.h>

static int sTestCount = 0;
static int sPassCount = 0;


static void
Check(bool condition, const char* description)
{
	sTestCount++;
	if (condition) {
		sPassCount++;
		printf("  PASS: %s\n", description);
	} else {
		printf("  FAIL: %s\n", description);
	}
}


static void
TestOcfValues()
{
	printf("Test: OCF values\n");

	Check(OCF_LE_START_ENCRYPTION == 0x0019,
		"OCF_LE_START_ENCRYPTION == 0x0019");
	Check(OCF_LE_LTK_REQUEST_REPLY == 0x001A,
		"OCF_LE_LTK_REQUEST_REPLY == 0x001A");
	Check(OCF_LE_LTK_REQUEST_NEG_REPLY == 0x001B,
		"OCF_LE_LTK_REQUEST_NEG_REPLY == 0x001B");
}


static void
TestStructSizes()
{
	printf("Test: Struct sizes\n");

	// hci_cp_le_start_encryption: handle(2) + random(8) + ediv(2) + ltk(16) = 28
	Check(sizeof(struct hci_cp_le_start_encryption) == 28,
		"hci_cp_le_start_encryption == 28 bytes");

	// hci_cp_le_ltk_request_reply: handle(2) + ltk(16) = 18
	Check(sizeof(struct hci_cp_le_ltk_request_reply) == 18,
		"hci_cp_le_ltk_request_reply == 18 bytes");

	// hci_cp_le_ltk_request_neg_reply: handle(2) = 2
	Check(sizeof(struct hci_cp_le_ltk_request_neg_reply) == 2,
		"hci_cp_le_ltk_request_neg_reply == 2 bytes");

	// Response structs
	Check(sizeof(struct hci_rp_le_ltk_request_reply) == 3,
		"hci_rp_le_ltk_request_reply == 3 bytes");
	Check(sizeof(struct hci_rp_le_ltk_request_neg_reply) == 3,
		"hci_rp_le_ltk_request_neg_reply == 3 bytes");
}


static void
TestBluetoothCommandTemplate()
{
	printf("Test: BluetoothCommand template construction\n");

	// Build LE LTK Request Reply
	{
		BluetoothCommand<typed_command(hci_cp_le_ltk_request_reply)>
			cmd(OGF_LE_CONTROL, OCF_LE_LTK_REQUEST_REPLY);

		cmd->handle = 0x0040;
		memset(cmd->ltk, 0xAB, 16);

		Check(cmd.Size() == HCI_COMMAND_HDR_SIZE + 18,
			"LTK_REQUEST_REPLY command size == 3 + 18");

		// Verify opcode in buffer
		const uint8* buf = (const uint8*)cmd.Data();
		uint16 opcode = buf[0] | (buf[1] << 8);
		Check(opcode == PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_LTK_REQUEST_REPLY),
			"LTK_REQUEST_REPLY opcode correct");
		Check(buf[2] == 18, "LTK_REQUEST_REPLY param length == 18");
	}

	// Build LE LTK Request Neg Reply
	{
		BluetoothCommand<typed_command(hci_cp_le_ltk_request_neg_reply)>
			cmd(OGF_LE_CONTROL, OCF_LE_LTK_REQUEST_NEG_REPLY);

		cmd->handle = 0x0040;

		Check(cmd.Size() == HCI_COMMAND_HDR_SIZE + 2,
			"LTK_REQUEST_NEG_REPLY command size == 3 + 2");

		const uint8* buf = (const uint8*)cmd.Data();
		uint16 opcode = buf[0] | (buf[1] << 8);
		Check(opcode == PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_LTK_REQUEST_NEG_REPLY),
			"LTK_REQUEST_NEG_REPLY opcode correct");
		Check(buf[2] == 2, "LTK_REQUEST_NEG_REPLY param length == 2");
	}

	// Build LE Start Encryption
	{
		BluetoothCommand<typed_command(hci_cp_le_start_encryption)>
			cmd(OGF_LE_CONTROL, OCF_LE_START_ENCRYPTION);

		cmd->handle = 0x0040;
		memset(cmd->random, 0, 8);
		cmd->ediv = 0;
		memset(cmd->ltk, 0xCC, 16);

		Check(cmd.Size() == HCI_COMMAND_HDR_SIZE + 28,
			"LE_START_ENCRYPTION command size == 3 + 28");

		const uint8* buf = (const uint8*)cmd.Data();
		Check(buf[2] == 28, "LE_START_ENCRYPTION param length == 28");
	}
}


int
main()
{
	printf("=== HCI LE Command Struct Tests ===\n\n");

	TestOcfValues();
	TestStructSizes();
	TestBluetoothCommandTemplate();

	printf("\n=== Results: %d/%d passed ===\n", sPassCount, sTestCount);
	return (sPassCount == sTestCount) ? 0 : 1;
}
