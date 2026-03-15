/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _BT_SMP_H_
#define _BT_SMP_H_

#include <SupportDefs.h>


/* SMP Command Codes */
#define SMP_CMD_PAIRING_REQ				0x01
#define SMP_CMD_PAIRING_RSP				0x02
#define SMP_CMD_PAIRING_CONFIRM			0x03
#define SMP_CMD_PAIRING_RANDOM			0x04
#define SMP_CMD_PAIRING_FAILED			0x05
#define SMP_CMD_ENCRYPTION_INFO			0x06
#define SMP_CMD_MASTER_IDENT				0x07
#define SMP_CMD_IDENTITY_INFO			0x08
#define SMP_CMD_IDENTITY_ADDR_INFO		0x09
#define SMP_CMD_SIGNING_INFO			0x0A
#define SMP_CMD_SECURITY_REQ			0x0B
#define SMP_CMD_PUBLIC_KEY				0x0C
#define SMP_CMD_DHKEY_CHECK				0x0D
#define SMP_CMD_KEYPRESS_NOTIFICATION	0x0E


/* IO Capabilities */
#define SMP_IO_DISPLAY_ONLY				0x00
#define SMP_IO_DISPLAY_YES_NO			0x01
#define SMP_IO_KEYBOARD_ONLY			0x02
#define SMP_IO_NO_INPUT_NO_OUTPUT		0x03
#define SMP_IO_KEYBOARD_DISPLAY			0x04


/* Authentication Requirements */
#define SMP_AUTH_BONDING				0x01
#define SMP_AUTH_MITM					0x04
#define SMP_AUTH_SC						0x08
#define SMP_AUTH_KEYPRESS				0x10
#define SMP_AUTH_CT2					0x20


/* Key Distribution Flags */
#define SMP_DIST_ENC_KEY				0x01
#define SMP_DIST_ID_KEY					0x02
#define SMP_DIST_SIGN					0x04
#define SMP_DIST_LINK_KEY				0x08


/* SMP Failure Reasons */
#define SMP_ERR_PASSKEY_ENTRY_FAILED	0x01
#define SMP_ERR_OOB_NOT_AVAILABLE		0x02
#define SMP_ERR_AUTH_REQUIREMENTS		0x03
#define SMP_ERR_CONFIRM_VALUE_FAILED	0x04
#define SMP_ERR_PAIRING_NOT_SUPPORTED	0x05
#define SMP_ERR_ENCRYPTION_KEY_SIZE		0x06
#define SMP_ERR_COMMAND_NOT_SUPPORTED	0x07
#define SMP_ERR_UNSPECIFIED_REASON		0x08
#define SMP_ERR_REPEATED_ATTEMPTS		0x09
#define SMP_ERR_INVALID_PARAMETERS		0x0A
#define SMP_ERR_DHKEY_CHECK_FAILED		0x0B
#define SMP_ERR_NUMERIC_COMPARISON		0x0C
#define SMP_ERR_KEY_REJECTED			0x0F


/* SMP PDU Structures */

struct smp_pairing_req {
	uint8	code;
	uint8	io_capability;
	uint8	oob_data_flag;
	uint8	auth_req;
	uint8	max_key_size;
	uint8	initiator_key_dist;
	uint8	responder_key_dist;
} __attribute__ ((packed));

typedef struct smp_pairing_req smp_pairing_rsp;

struct smp_pairing_confirm {
	uint8	code;
	uint8	confirm_value[16];
} __attribute__ ((packed));

struct smp_pairing_random {
	uint8	code;
	uint8	random_value[16];
} __attribute__ ((packed));

struct smp_pairing_failed {
	uint8	code;
	uint8	reason;
} __attribute__ ((packed));

struct smp_encryption_info {
	uint8	code;
	uint8	ltk[16];
} __attribute__ ((packed));

struct smp_master_ident {
	uint8	code;
	uint16	ediv;
	uint8	random[8];
} __attribute__ ((packed));

struct smp_identity_info {
	uint8	code;
	uint8	irk[16];
} __attribute__ ((packed));

struct smp_identity_addr_info {
	uint8	code;
	uint8	addr_type;
	uint8	bdaddr[6];
} __attribute__ ((packed));

struct smp_signing_info {
	uint8	code;
	uint8	csrk[16];
} __attribute__ ((packed));

struct smp_security_req {
	uint8	code;
	uint8	auth_req;
} __attribute__ ((packed));

struct smp_public_key {
	uint8	code;
	uint8	x[32];
	uint8	y[32];
} __attribute__ ((packed));

struct smp_dhkey_check {
	uint8	code;
	uint8	check[16];
} __attribute__ ((packed));


#endif /* _BT_SMP_H_ */
