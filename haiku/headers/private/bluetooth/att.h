/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _ATT_H_
#define _ATT_H_

#include <SupportDefs.h>


/* ATT Protocol PDU Opcodes */
#define ATT_OP_ERROR_RSP					0x01
#define ATT_OP_EXCHANGE_MTU_REQ				0x02
#define ATT_OP_EXCHANGE_MTU_RSP				0x03
#define ATT_OP_FIND_INFO_REQ				0x04
#define ATT_OP_FIND_INFO_RSP				0x05
#define ATT_OP_FIND_BY_TYPE_VALUE_REQ		0x06
#define ATT_OP_FIND_BY_TYPE_VALUE_RSP		0x07
#define ATT_OP_READ_BY_TYPE_REQ				0x08
#define ATT_OP_READ_BY_TYPE_RSP				0x09
#define ATT_OP_READ_REQ						0x0A
#define ATT_OP_READ_RSP						0x0B
#define ATT_OP_READ_BLOB_REQ				0x0C
#define ATT_OP_READ_BLOB_RSP				0x0D
#define ATT_OP_READ_MULTIPLE_REQ			0x0E
#define ATT_OP_READ_MULTIPLE_RSP			0x0F
#define ATT_OP_READ_BY_GROUP_TYPE_REQ		0x10
#define ATT_OP_READ_BY_GROUP_TYPE_RSP		0x11
#define ATT_OP_WRITE_REQ					0x12
#define ATT_OP_WRITE_RSP					0x13
#define ATT_OP_WRITE_CMD					0x52
#define ATT_OP_PREPARE_WRITE_REQ			0x16
#define ATT_OP_PREPARE_WRITE_RSP			0x17
#define ATT_OP_EXECUTE_WRITE_REQ			0x18
#define ATT_OP_EXECUTE_WRITE_RSP			0x19
#define ATT_OP_HANDLE_VALUE_NTF				0x1B
#define ATT_OP_HANDLE_VALUE_IND				0x1D
#define ATT_OP_HANDLE_VALUE_CFM				0x1E


/* ATT Error Codes */
#define ATT_ERR_INVALID_HANDLE				0x01
#define ATT_ERR_READ_NOT_PERMITTED			0x02
#define ATT_ERR_WRITE_NOT_PERMITTED			0x03
#define ATT_ERR_INVALID_PDU					0x04
#define ATT_ERR_INSUFFICIENT_AUTHN			0x05
#define ATT_ERR_REQUEST_NOT_SUPPORTED		0x06
#define ATT_ERR_INVALID_OFFSET				0x07
#define ATT_ERR_INSUFFICIENT_AUTHZ			0x08
#define ATT_ERR_PREPARE_QUEUE_FULL			0x09
#define ATT_ERR_ATTRIBUTE_NOT_FOUND			0x0A
#define ATT_ERR_ATTRIBUTE_NOT_LONG			0x0B
#define ATT_ERR_INSUFFICIENT_KEY_SIZE		0x0C
#define ATT_ERR_INVALID_ATTRIBUTE_LENGTH	0x0D
#define ATT_ERR_UNLIKELY_ERROR				0x0E
#define ATT_ERR_INSUFFICIENT_ENCRYPTION		0x0F
#define ATT_ERR_UNSUPPORTED_GROUP_TYPE		0x10
#define ATT_ERR_INSUFFICIENT_RESOURCES		0x11


/* ATT MTU Limits */
#define ATT_DEFAULT_LE_MTU					23
#define ATT_MAX_MTU							517


/* Well-known GATT UUIDs (16-bit) */
#define GATT_UUID_PRIMARY_SERVICE			0x2800
#define GATT_UUID_SECONDARY_SERVICE			0x2801
#define GATT_UUID_INCLUDE					0x2802
#define GATT_UUID_CHARACTERISTIC			0x2803
#define GATT_UUID_CCC						0x2902	/* Client Characteristic Configuration */
#define GATT_UUID_SCC						0x2903	/* Server Characteristic Configuration */
#define GATT_UUID_CHAR_USER_DESC			0x2901
#define GATT_UUID_CHAR_FORMAT				0x2904


/* CCC Descriptor bit values */
#define GATT_CCC_NOTIFY						0x0001
#define GATT_CCC_INDICATE					0x0002


/* Characteristic Properties */
#define GATT_CHAR_PROP_BROADCAST			0x01
#define GATT_CHAR_PROP_READ					0x02
#define GATT_CHAR_PROP_WRITE_NO_RSP			0x04
#define GATT_CHAR_PROP_WRITE				0x08
#define GATT_CHAR_PROP_NOTIFY				0x10
#define GATT_CHAR_PROP_INDICATE				0x20
#define GATT_CHAR_PROP_AUTH_SIGNED_WRITE	0x40
#define GATT_CHAR_PROP_EXTENDED				0x80


/* ATT PDU Structures */

struct att_error_rsp {
	uint8	opcode;
	uint8	request_opcode;
	uint16	handle;
	uint8	error_code;
} __attribute__ ((packed));

struct att_exchange_mtu_req {
	uint8	opcode;
	uint16	client_mtu;
} __attribute__ ((packed));

struct att_exchange_mtu_rsp {
	uint8	opcode;
	uint16	server_mtu;
} __attribute__ ((packed));

struct att_find_info_req {
	uint8	opcode;
	uint16	start_handle;
	uint16	end_handle;
} __attribute__ ((packed));

struct att_find_info_rsp {
	uint8	opcode;
	uint8	format;		/* 1 = 16-bit UUID, 2 = 128-bit UUID */
	/* handle-UUID pairs follow */
} __attribute__ ((packed));

struct att_read_by_type_req {
	uint8	opcode;
	uint16	start_handle;
	uint16	end_handle;
	/* 2 or 16 byte UUID follows */
} __attribute__ ((packed));

struct att_read_by_type_rsp {
	uint8	opcode;
	uint8	length;		/* size of each attribute handle-value pair */
	/* attribute data list follows */
} __attribute__ ((packed));

struct att_read_by_group_type_req {
	uint8	opcode;
	uint16	start_handle;
	uint16	end_handle;
	/* 2 or 16 byte UUID follows */
} __attribute__ ((packed));

struct att_read_by_group_type_rsp {
	uint8	opcode;
	uint8	length;		/* size of each attribute data group */
	/* attribute data list follows */
} __attribute__ ((packed));

struct att_read_req {
	uint8	opcode;
	uint16	handle;
} __attribute__ ((packed));

struct att_read_rsp {
	uint8	opcode;
	/* attribute value follows */
} __attribute__ ((packed));

struct att_write_req {
	uint8	opcode;
	uint16	handle;
	/* attribute value follows */
} __attribute__ ((packed));

struct att_write_rsp {
	uint8	opcode;
} __attribute__ ((packed));

struct att_write_cmd {
	uint8	opcode;
	uint16	handle;
	/* attribute value follows */
} __attribute__ ((packed));

struct att_handle_value_ntf {
	uint8	opcode;
	uint16	handle;
	/* attribute value follows */
} __attribute__ ((packed));

struct att_handle_value_ind {
	uint8	opcode;
	uint16	handle;
	/* attribute value follows */
} __attribute__ ((packed));

struct att_handle_value_cfm {
	uint8	opcode;
} __attribute__ ((packed));


#endif /* _ATT_H_ */
