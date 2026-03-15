/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * AtParser — AT command parser for HFP 1.7.
 * Private to libbluetooth.so; not installed as a public header.
 */
#ifndef _AT_PARSER_H_
#define _AT_PARSER_H_

#include <String.h>


class AtParser {
public:
	enum CommandType {
		AT_CMD_UNKNOWN = 0,
		AT_CMD_BRSF,		// AT+BRSF=<features>
		AT_CMD_CIND_TEST,	// AT+CIND=?
		AT_CMD_CIND_READ,	// AT+CIND?
		AT_CMD_CMER,		// AT+CMER=<mode>,<keyp>,<disp>,<ind>
		AT_CMD_CHLD_TEST,	// AT+CHLD=?
		AT_CMD_CHLD,		// AT+CHLD=<n>
		AT_CMD_CLIP,		// AT+CLIP=<n>
		AT_CMD_VGS,		// AT+VGS=<level>
		AT_CMD_VGM,		// AT+VGM=<level>
		AT_CMD_ATD,			// ATD<number>;
		AT_CMD_ATA,			// ATA
		AT_CMD_CHUP,		// AT+CHUP
		AT_CMD_BTRH_READ,	// AT+BTRH?
		AT_CMD_COPS_READ,	// AT+COPS?
		AT_CMD_COPS_SET,	// AT+COPS=<mode>
		AT_CMD_CLCC,		// AT+CLCC
		AT_CMD_BIA,			// AT+BIA=<indrep>
		AT_CMD_NREC,		// AT+NREC=<n>
		AT_CMD_BINP,		// AT+BINP=<n>
		AT_CMD_BVRA,		// AT+BVRA=<n>
		AT_CMD_OK,			// OK
		AT_CMD_ERROR,		// ERROR
		AT_CMD_CME_ERROR,	// +CME ERROR:<n>
		AT_CMD_RING,		// RING
	};

	struct Command {
		CommandType		type;
		BString			fullLine;
		BString			argument;
	};

	static bool				Parse(const char* line, Command& outCmd);

	static BString			FormatBrsf(uint32 features);
	static BString			FormatCindTest();
	static BString			FormatCindRead(uint8 service, uint8 call,
								uint8 callsetup, uint8 signal,
								uint8 roam, uint8 battchg,
								uint8 callheld);
	static BString			FormatChldTest();
	static BString			FormatOK();
	static BString			FormatError();
	static BString			FormatCmeError(int code);
	static BString			FormatRing();
	static BString			FormatClip(const char* number, int type);
	static BString			FormatVgs(uint8 level);
	static BString			FormatVgm(uint8 level);
};


#endif /* _AT_PARSER_H_ */
