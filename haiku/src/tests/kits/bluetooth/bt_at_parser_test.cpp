/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * bt_at_parser_test — Unit tests for the HFP AT command parser.
 * Standalone (no libbluetooth.so required).
 */

#include <stdio.h>
#include <string.h>

#include "AtParser.h"


static int sTestCount = 0;
static int sPassCount = 0;


static void
Check(bool condition, const char* label)
{
	sTestCount++;
	if (condition) {
		sPassCount++;
		printf("  PASS: %s\n", label);
	} else {
		printf("  FAIL: %s\n", label);
	}
}


static void
TestParseBrsf()
{
	printf("--- Parse AT+BRSF ---\n");

	AtParser::Command cmd;
	bool ok = AtParser::Parse("AT+BRSF=31", cmd);
	Check(ok, "AT+BRSF=31 parsed");
	Check(cmd.type == AtParser::AT_CMD_BRSF, "type is BRSF");
	Check(cmd.argument == "31", "argument is '31'");

	ok = AtParser::Parse("AT+BRSF=0", cmd);
	Check(ok, "AT+BRSF=0 parsed");
	Check(cmd.argument == "0", "argument is '0'");
}


static void
TestParseCind()
{
	printf("--- Parse AT+CIND ---\n");

	AtParser::Command cmd;
	bool ok = AtParser::Parse("AT+CIND=?", cmd);
	Check(ok, "AT+CIND=? parsed");
	Check(cmd.type == AtParser::AT_CMD_CIND_TEST, "type is CIND_TEST");

	ok = AtParser::Parse("AT+CIND?", cmd);
	Check(ok, "AT+CIND? parsed");
	Check(cmd.type == AtParser::AT_CMD_CIND_READ, "type is CIND_READ");
}


static void
TestParseCmer()
{
	printf("--- Parse AT+CMER ---\n");

	AtParser::Command cmd;
	bool ok = AtParser::Parse("AT+CMER=3,0,0,1", cmd);
	Check(ok, "AT+CMER=3,0,0,1 parsed");
	Check(cmd.type == AtParser::AT_CMD_CMER, "type is CMER");
	Check(cmd.argument == "3,0,0,1", "argument is '3,0,0,1'");
}


static void
TestParseChld()
{
	printf("--- Parse AT+CHLD ---\n");

	AtParser::Command cmd;
	bool ok = AtParser::Parse("AT+CHLD=?", cmd);
	Check(ok, "AT+CHLD=? parsed");
	Check(cmd.type == AtParser::AT_CMD_CHLD_TEST, "type is CHLD_TEST");

	ok = AtParser::Parse("AT+CHLD=1", cmd);
	Check(ok, "AT+CHLD=1 parsed");
	Check(cmd.type == AtParser::AT_CMD_CHLD, "type is CHLD");
	Check(cmd.argument == "1", "argument is '1'");
}


static void
TestParseCallControl()
{
	printf("--- Parse Call Control ---\n");

	AtParser::Command cmd;

	bool ok = AtParser::Parse("ATD+1234567890;", cmd);
	Check(ok, "ATD parsed");
	Check(cmd.type == AtParser::AT_CMD_ATD, "type is ATD");
	Check(cmd.argument == "+1234567890", "argument has number (';' stripped)");

	ok = AtParser::Parse("ATA", cmd);
	Check(ok, "ATA parsed");
	Check(cmd.type == AtParser::AT_CMD_ATA, "type is ATA");

	ok = AtParser::Parse("AT+CHUP", cmd);
	Check(ok, "AT+CHUP parsed");
	Check(cmd.type == AtParser::AT_CMD_CHUP, "type is CHUP");
}


static void
TestParseVolume()
{
	printf("--- Parse Volume ---\n");

	AtParser::Command cmd;

	bool ok = AtParser::Parse("AT+VGS=10", cmd);
	Check(ok, "AT+VGS=10 parsed");
	Check(cmd.type == AtParser::AT_CMD_VGS, "type is VGS");
	Check(cmd.argument == "10", "argument is '10'");

	ok = AtParser::Parse("AT+VGM=5", cmd);
	Check(ok, "AT+VGM=5 parsed");
	Check(cmd.type == AtParser::AT_CMD_VGM, "type is VGM");
	Check(cmd.argument == "5", "argument is '5'");
}


static void
TestParseResponses()
{
	printf("--- Parse Responses ---\n");

	AtParser::Command cmd;

	bool ok = AtParser::Parse("OK", cmd);
	Check(ok, "OK parsed");
	Check(cmd.type == AtParser::AT_CMD_OK, "type is OK");

	ok = AtParser::Parse("ERROR", cmd);
	Check(ok, "ERROR parsed");
	Check(cmd.type == AtParser::AT_CMD_ERROR, "type is ERROR");

	ok = AtParser::Parse("+CME ERROR:30", cmd);
	Check(ok, "+CME ERROR:30 parsed");
	Check(cmd.type == AtParser::AT_CMD_CME_ERROR, "type is CME_ERROR");
	Check(cmd.argument == "30", "argument is '30'");

	ok = AtParser::Parse("RING", cmd);
	Check(ok, "RING parsed");
	Check(cmd.type == AtParser::AT_CMD_RING, "type is RING");
}


static void
TestParseClip()
{
	printf("--- Parse CLIP ---\n");

	AtParser::Command cmd;

	bool ok = AtParser::Parse("AT+CLIP=1", cmd);
	Check(ok, "AT+CLIP=1 parsed");
	Check(cmd.type == AtParser::AT_CMD_CLIP, "type is CLIP");
	Check(cmd.argument == "1", "argument is '1'");
}


static void
TestParseMisc()
{
	printf("--- Parse Miscellaneous ---\n");

	AtParser::Command cmd;

	bool ok = AtParser::Parse("AT+BTRH?", cmd);
	Check(ok, "AT+BTRH? parsed");
	Check(cmd.type == AtParser::AT_CMD_BTRH_READ, "type is BTRH_READ");

	ok = AtParser::Parse("AT+COPS?", cmd);
	Check(ok, "AT+COPS? parsed");
	Check(cmd.type == AtParser::AT_CMD_COPS_READ, "type is COPS_READ");

	ok = AtParser::Parse("AT+COPS=3,0", cmd);
	Check(ok, "AT+COPS=3,0 parsed");
	Check(cmd.type == AtParser::AT_CMD_COPS_SET, "type is COPS_SET");

	ok = AtParser::Parse("AT+CLCC", cmd);
	Check(ok, "AT+CLCC parsed");
	Check(cmd.type == AtParser::AT_CMD_CLCC, "type is CLCC");

	ok = AtParser::Parse("AT+NREC=0", cmd);
	Check(ok, "AT+NREC=0 parsed");
	Check(cmd.type == AtParser::AT_CMD_NREC, "type is NREC");

	ok = AtParser::Parse("AT+BVRA=1", cmd);
	Check(ok, "AT+BVRA=1 parsed");
	Check(cmd.type == AtParser::AT_CMD_BVRA, "type is BVRA");
}


static void
TestParseUnknown()
{
	printf("--- Parse Unknown ---\n");

	AtParser::Command cmd;

	bool ok = AtParser::Parse("AT+GARBAGE=blah", cmd);
	Check(!ok, "AT+GARBAGE returns false (unrecognized)");
	Check(cmd.type == AtParser::AT_CMD_UNKNOWN, "type is UNKNOWN");

	ok = AtParser::Parse("", cmd);
	Check(!ok, "empty string returns false");
}


static void
TestFormatBrsf()
{
	printf("--- Format AT+BRSF ---\n");

	// FormatBrsf is the AG response format: \r\n+BRSF:<features>\r\n
	BString s = AtParser::FormatBrsf(31);
	Check(s == "\r\n+BRSF:31\r\n", "FormatBrsf(31) AG response");

	s = AtParser::FormatBrsf(0);
	Check(s == "\r\n+BRSF:0\r\n", "FormatBrsf(0) AG response");
}


static void
TestFormatResponses()
{
	printf("--- Format Responses ---\n");

	BString s = AtParser::FormatOK();
	Check(s == "\r\nOK\r\n", "FormatOK");

	s = AtParser::FormatError();
	Check(s == "\r\nERROR\r\n", "FormatError");

	s = AtParser::FormatCmeError(30);
	Check(s == "\r\n+CME ERROR:30\r\n", "FormatCmeError(30)");

	s = AtParser::FormatRing();
	Check(s == "\r\nRING\r\n", "FormatRing");
}


static void
TestFormatIndicators()
{
	printf("--- Format Indicators ---\n");

	BString s = AtParser::FormatCindTest();
	// Should contain indicator names
	Check(s.FindFirst("service") >= 0, "CIND test has 'service'");
	Check(s.FindFirst("call") >= 0, "CIND test has 'call'");
	Check(s.FindFirst("signal") >= 0, "CIND test has 'signal'");

	s = AtParser::FormatCindRead(1, 0, 0, 3, 0, 5, 0);
	// Should contain +CIND: and the indicator values
	Check(s.FindFirst("+CIND:") >= 0, "CIND read has '+CIND:'");
	Check(s.FindFirst("1,0,0,3,0,5,0") >= 0,
		"CIND read has '1,0,0,3,0,5,0'");
}


static void
TestFormatVolume()
{
	printf("--- Format Volume ---\n");

	BString s = AtParser::FormatVgs(10);
	Check(s == "\r\n+VGS:10\r\n", "FormatVgs(10)");

	s = AtParser::FormatVgm(5);
	Check(s == "\r\n+VGM:5\r\n", "FormatVgm(5)");
}


int
main()
{
	printf("=== AT Parser Unit Tests ===\n\n");

	TestParseBrsf();
	TestParseCind();
	TestParseCmer();
	TestParseChld();
	TestParseCallControl();
	TestParseVolume();
	TestParseResponses();
	TestParseClip();
	TestParseMisc();
	TestParseUnknown();
	TestFormatBrsf();
	TestFormatResponses();
	TestFormatIndicators();
	TestFormatVolume();

	printf("\n=== Results: %d/%d passed ===\n",
		sPassCount, sTestCount);

	return (sPassCount == sTestCount) ? 0 : 1;
}
