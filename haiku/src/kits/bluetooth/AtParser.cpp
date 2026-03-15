/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * AtParser — AT command parser for HFP 1.7.
 */

#include "AtParser.h"

#include <stdio.h>
#include <string.h>


bool
AtParser::Parse(const char* line, Command& outCmd)
{
	outCmd.type = AT_CMD_UNKNOWN;
	outCmd.fullLine = line;
	outCmd.argument = "";

	if (line == NULL || line[0] == '\0')
		return false;

	// Skip leading whitespace
	while (*line == ' ' || *line == '\t')
		line++;

	// Unsolicited responses from AG
	if (strncmp(line, "OK", 2) == 0) {
		outCmd.type = AT_CMD_OK;
		return true;
	}

	if (strncmp(line, "ERROR", 5) == 0) {
		outCmd.type = AT_CMD_ERROR;
		return true;
	}

	if (strncmp(line, "+CME ERROR:", 11) == 0) {
		outCmd.type = AT_CMD_CME_ERROR;
		outCmd.argument = line + 11;
		return true;
	}

	if (strncmp(line, "RING", 4) == 0) {
		outCmd.type = AT_CMD_RING;
		return true;
	}

	// AT commands from HF
	if (strncmp(line, "AT", 2) != 0 && strncmp(line, "at", 2) != 0)
		return false;

	line += 2;

	// ATA — answer call
	if (*line == 'A' || *line == 'a') {
		outCmd.type = AT_CMD_ATA;
		return true;
	}

	// ATD<number>; — dial
	if (*line == 'D' || *line == 'd') {
		outCmd.type = AT_CMD_ATD;
		outCmd.argument = line + 1;
		// Strip trailing ';' if present
		if (outCmd.argument.EndsWith(";"))
			outCmd.argument.Truncate(outCmd.argument.Length() - 1);
		return true;
	}

	// AT+ commands
	if (*line != '+')
		return false;

	line++;

	if (strncmp(line, "BRSF=", 5) == 0) {
		outCmd.type = AT_CMD_BRSF;
		outCmd.argument = line + 5;
		return true;
	}

	if (strncmp(line, "CIND=?", 6) == 0) {
		outCmd.type = AT_CMD_CIND_TEST;
		return true;
	}

	if (strncmp(line, "CIND?", 5) == 0) {
		outCmd.type = AT_CMD_CIND_READ;
		return true;
	}

	if (strncmp(line, "CMER=", 5) == 0) {
		outCmd.type = AT_CMD_CMER;
		outCmd.argument = line + 5;
		return true;
	}

	if (strncmp(line, "CHLD=?", 6) == 0) {
		outCmd.type = AT_CMD_CHLD_TEST;
		return true;
	}

	if (strncmp(line, "CHLD=", 5) == 0) {
		outCmd.type = AT_CMD_CHLD;
		outCmd.argument = line + 5;
		return true;
	}

	if (strncmp(line, "CLIP=", 5) == 0) {
		outCmd.type = AT_CMD_CLIP;
		outCmd.argument = line + 5;
		return true;
	}

	if (strncmp(line, "VGS=", 4) == 0) {
		outCmd.type = AT_CMD_VGS;
		outCmd.argument = line + 4;
		return true;
	}

	if (strncmp(line, "VGM=", 4) == 0) {
		outCmd.type = AT_CMD_VGM;
		outCmd.argument = line + 4;
		return true;
	}

	if (strncmp(line, "CHUP", 4) == 0) {
		outCmd.type = AT_CMD_CHUP;
		return true;
	}

	if (strncmp(line, "BTRH?", 5) == 0) {
		outCmd.type = AT_CMD_BTRH_READ;
		return true;
	}

	if (strncmp(line, "COPS?", 5) == 0) {
		outCmd.type = AT_CMD_COPS_READ;
		return true;
	}

	if (strncmp(line, "COPS=", 5) == 0) {
		outCmd.type = AT_CMD_COPS_SET;
		outCmd.argument = line + 5;
		return true;
	}

	if (strncmp(line, "CLCC", 4) == 0) {
		outCmd.type = AT_CMD_CLCC;
		return true;
	}

	if (strncmp(line, "BIA=", 4) == 0) {
		outCmd.type = AT_CMD_BIA;
		outCmd.argument = line + 4;
		return true;
	}

	if (strncmp(line, "NREC=", 5) == 0) {
		outCmd.type = AT_CMD_NREC;
		outCmd.argument = line + 5;
		return true;
	}

	if (strncmp(line, "BINP=", 5) == 0) {
		outCmd.type = AT_CMD_BINP;
		outCmd.argument = line + 5;
		return true;
	}

	if (strncmp(line, "BVRA=", 5) == 0) {
		outCmd.type = AT_CMD_BVRA;
		outCmd.argument = line + 5;
		return true;
	}

	return false;
}


BString
AtParser::FormatBrsf(uint32 features)
{
	BString result;
	result.SetToFormat("\r\n+BRSF:%u\r\n", (unsigned)features);
	return result;
}


BString
AtParser::FormatCindTest()
{
	return BString("\r\n+CIND:"
		"(\"service\",(0,1)),"
		"(\"call\",(0,1)),"
		"(\"callsetup\",(0-3)),"
		"(\"signal\",(0-5)),"
		"(\"roam\",(0,1)),"
		"(\"battchg\",(0-5)),"
		"(\"callheld\",(0-2))"
		"\r\n");
}


BString
AtParser::FormatCindRead(uint8 service, uint8 call, uint8 callsetup,
	uint8 signal, uint8 roam, uint8 battchg, uint8 callheld)
{
	BString result;
	result.SetToFormat("\r\n+CIND:%u,%u,%u,%u,%u,%u,%u\r\n",
		service, call, callsetup, signal, roam, battchg, callheld);
	return result;
}


BString
AtParser::FormatChldTest()
{
	return BString("\r\n+CHLD:(0,1,2,3)\r\n");
}


BString
AtParser::FormatOK()
{
	return BString("\r\nOK\r\n");
}


BString
AtParser::FormatError()
{
	return BString("\r\nERROR\r\n");
}


BString
AtParser::FormatCmeError(int code)
{
	BString result;
	result.SetToFormat("\r\n+CME ERROR:%d\r\n", code);
	return result;
}


BString
AtParser::FormatRing()
{
	return BString("\r\nRING\r\n");
}


BString
AtParser::FormatClip(const char* number, int type)
{
	BString result;
	result.SetToFormat("\r\n+CLIP:\"%s\",%d\r\n", number, type);
	return result;
}


BString
AtParser::FormatVgs(uint8 level)
{
	BString result;
	result.SetToFormat("\r\n+VGS:%u\r\n", level);
	return result;
}


BString
AtParser::FormatVgm(uint8 level)
{
	BString result;
	result.SetToFormat("\r\n+VGM:%u\r\n", level);
	return result;
}
