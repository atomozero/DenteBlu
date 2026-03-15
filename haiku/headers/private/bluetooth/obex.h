/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * OBEX (Object Exchange) protocol constants.
 * Based on IrDA OBEX specification v1.5 and Bluetooth GOEP v2.1.
 */
#ifndef _OBEX_H_
#define _OBEX_H_

#include <SupportDefs.h>


/* =========================================================================
 * OBEX Opcodes (request)
 * High bit (0x80) = "Final" bit — set for single-packet or last packet.
 * ========================================================================= */
#define OBEX_OP_CONNECT			0x80	/* always Final */
#define OBEX_OP_DISCONNECT		0x81	/* always Final */
#define OBEX_OP_PUT				0x02
#define OBEX_OP_PUT_FINAL		0x82
#define OBEX_OP_GET				0x03
#define OBEX_OP_GET_FINAL		0x83
#define OBEX_OP_SETPATH			0x85	/* always Final */
#define OBEX_OP_ACTION			0x86	/* always Final */
#define OBEX_OP_SESSION			0x87	/* always Final */
#define OBEX_OP_ABORT			0xFF	/* always Final */


/* =========================================================================
 * OBEX Response Codes
 * High bit (0x80) = "Final" bit — always set in responses.
 * ========================================================================= */
#define OBEX_RSP_CONTINUE		0x90	/* 100 Continue */
#define OBEX_RSP_SUCCESS		0xA0	/* 200 OK */
#define OBEX_RSP_CREATED		0xA1	/* 201 Created */
#define OBEX_RSP_ACCEPTED		0xA2	/* 202 Accepted */
#define OBEX_RSP_BAD_REQUEST	0xC0	/* 400 Bad Request */
#define OBEX_RSP_UNAUTHORIZED	0xC1	/* 401 Unauthorized */
#define OBEX_RSP_FORBIDDEN		0xC3	/* 403 Forbidden */
#define OBEX_RSP_NOT_FOUND		0xC4	/* 404 Not Found */
#define OBEX_RSP_NOT_ACCEPTABLE	0xC6	/* 406 Not Acceptable */
#define OBEX_RSP_CONFLICT		0xC9	/* 409 Conflict */
#define OBEX_RSP_GONE			0xCA	/* 410 Gone */
#define OBEX_RSP_LENGTH_REQUIRED 0xCB	/* 411 Length Required */
#define OBEX_RSP_PRECONDITION	0xCC	/* 412 Precondition Failed */
#define OBEX_RSP_ENTITY_TOO_LARGE 0xCD	/* 413 */
#define OBEX_RSP_NOT_IMPLEMENTED 0xD1	/* 501 Not Implemented */
#define OBEX_RSP_SERVICE_UNAVAIL 0xD3	/* 503 Service Unavailable */
#define OBEX_RSP_DATABASE_FULL	0xE0	/* Database Full */
#define OBEX_RSP_DATABASE_LOCKED 0xE1	/* Database Locked */

/* Check if a response code indicates an error (>= 0xC0) */
#define OBEX_IS_ERROR(code)		(((code) & 0xF0) >= 0xC0)


/* =========================================================================
 * OBEX Header IDs
 *
 * Encoding (bits 7-6 of HI):
 *   00 = null-terminated Unicode (length-prefixed, 2-byte length)
 *   01 = byte sequence (length-prefixed, 2-byte length)
 *   10 = 1-byte value
 *   11 = 4-byte value
 * ========================================================================= */

/* Unicode string headers (00xxxxxx) */
#define OBEX_HDR_NAME			0x01	/* Object name (Unicode, null-term) */
#define OBEX_HDR_DESCRIPTION	0x05	/* Description */

/* Byte sequence headers (01xxxxxx) */
#define OBEX_HDR_TYPE			0x42	/* Object type (ASCII, null-term) */
#define OBEX_HDR_TARGET			0x46	/* Target service UUID */
#define OBEX_HDR_BODY			0x48	/* Object body chunk */
#define OBEX_HDR_END_OF_BODY	0x49	/* Final object body chunk */
#define OBEX_HDR_WHO			0x4A	/* Identifies OBEX application */
#define OBEX_HDR_APP_PARAMS		0x4C	/* Application parameters */
#define OBEX_HDR_AUTH_CHALLENGE	0x4D	/* Authentication challenge */
#define OBEX_HDR_AUTH_RESPONSE	0x4E	/* Authentication response */
#define OBEX_HDR_OBJECT_CLASS	0x4F	/* OBEX Object class */

/* 1-byte value headers (10xxxxxx) */
#define OBEX_HDR_SRM			0x97	/* Single Response Mode */
#define OBEX_HDR_SRMP			0x98	/* SRM Parameters */

/* 4-byte value headers (11xxxxxx) */
#define OBEX_HDR_COUNT			0xC0	/* Number of objects */
#define OBEX_HDR_LENGTH			0xC3	/* Object length */
#define OBEX_HDR_CONNECTION_ID	0xCB	/* Connection identifier */

/* Header encoding type (bits 7-6) */
#define OBEX_HDR_ENCODING(hi)	((hi) >> 6)
#define OBEX_HDR_ENC_UNICODE	0	/* 2-byte length-prefixed Unicode */
#define OBEX_HDR_ENC_BYTES		1	/* 2-byte length-prefixed bytes */
#define OBEX_HDR_ENC_1BYTE		2	/* 1-byte immediate value */
#define OBEX_HDR_ENC_4BYTE		3	/* 4-byte immediate value */


/* =========================================================================
 * OBEX CONNECT packet structure
 *
 * Request:  [0x80][Length(2)][Version(1)][Flags(1)][MaxPktLen(2)][Headers...]
 * Response: [Code(1)][Length(2)][Version(1)][Flags(1)][MaxPktLen(2)][Headers...]
 * ========================================================================= */
#define OBEX_VERSION			0x10	/* OBEX 1.0 */
#define OBEX_CONNECT_HEADER_SIZE 7		/* opcode(1) + length(2) + ver(1) + flags(1) + maxpkt(2) */

/* Default maximum OBEX packet length.
 * Must be >= 255. Typical: 4096-65535. */
#define OBEX_DEFAULT_MAX_PACKET	4096


/* =========================================================================
 * PBAP (Phone Book Access Profile) constants
 * ========================================================================= */

/* PBAP Target UUID for OBEX Connect:
 * 796135F0-F0C5-11D8-0966-0800200C9A66 */
static const uint8 kPbapTargetUuid[16] = {
	0x79, 0x61, 0x35, 0xF0, 0xF0, 0xC5, 0x11, 0xD8,
	0x09, 0x66, 0x08, 0x00, 0x20, 0x0C, 0x9A, 0x66
};

/* PBAP Application Parameter Tag IDs (for OBEX_HDR_APP_PARAMS)
 * Per PBAP v1.2 specification, Table 6.4 */
#define PBAP_TAG_ORDER					0x01	/* 1 byte */
#define PBAP_TAG_SEARCH_VALUE			0x02	/* variable */
#define PBAP_TAG_SEARCH_ATTRIBUTE		0x03	/* 1 byte */
#define PBAP_TAG_MAX_LIST_COUNT			0x04	/* 2 bytes */
#define PBAP_TAG_LIST_START_OFFSET		0x05	/* 2 bytes */
#define PBAP_TAG_PROPERTY_SELECTOR		0x06	/* 8 bytes */
#define PBAP_TAG_FORMAT					0x07	/* 1 byte */
#define PBAP_TAG_PHONEBOOK_SIZE			0x08	/* 2 bytes */
#define PBAP_TAG_NEW_MISSED_CALLS		0x09	/* 1 byte */
#define PBAP_TAG_PRIMARY_VERSION_CTR	0x0A	/* 16 bytes (PBAP 1.2) */
#define PBAP_TAG_SECONDARY_VERSION_CTR	0x0B	/* 16 bytes (PBAP 1.2) */
#define PBAP_TAG_VCARD_SELECTOR			0x0C	/* 8 bytes (PBAP 1.2) */
#define PBAP_TAG_DATABASE_IDENTIFIER	0x0D	/* 16 bytes (PBAP 1.2) */
#define PBAP_TAG_VCARD_SELECTOR_OP		0x0E	/* 1 byte (PBAP 1.2) */
#define PBAP_TAG_RESET_NEW_MISSED_CALLS	0x0F	/* 1 byte (PBAP 1.2) */
#define PBAP_TAG_SUPPORTED_FEATURES		0x10	/* 4 bytes (PBAP 1.2) */

/* PBAP Format values */
#define PBAP_FORMAT_VCARD_21			0x00
#define PBAP_FORMAT_VCARD_30			0x01

/* PBAP phonebook paths */
#define PBAP_PATH_PHONEBOOK				"telecom/pb.vcf"
#define PBAP_PATH_INCOMING_CALLS		"telecom/ich.vcf"
#define PBAP_PATH_OUTGOING_CALLS		"telecom/och.vcf"
#define PBAP_PATH_MISSED_CALLS			"telecom/mch.vcf"
#define PBAP_PATH_COMBINED_CALLS		"telecom/cch.vcf"
#define PBAP_PATH_SIM_PHONEBOOK			"SIM1/telecom/pb.vcf"


#endif /* _OBEX_H_ */
