/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _SDP_H_
#define _SDP_H_

#include <SupportDefs.h>


/* SDP PDU IDs */
#define SDP_ERROR_RSP						0x01
#define SDP_SERVICE_SEARCH_REQ				0x02
#define SDP_SERVICE_SEARCH_RSP				0x03
#define SDP_SERVICE_ATTR_REQ				0x04
#define SDP_SERVICE_ATTR_RSP				0x05
#define SDP_SERVICE_SEARCH_ATTR_REQ			0x06
#define SDP_SERVICE_SEARCH_ATTR_RSP			0x07


/* SDP Error Codes */
#define SDP_ERR_INVALID_VERSION				0x0001
#define SDP_ERR_INVALID_RECORD_HANDLE		0x0002
#define SDP_ERR_INVALID_SYNTAX				0x0003
#define SDP_ERR_INVALID_PDU_SIZE			0x0004
#define SDP_ERR_INVALID_CONTINUATION		0x0005
#define SDP_ERR_INSUFFICIENT_RESOURCES		0x0006


/* Data Element Type Descriptors (upper 5 bits of type descriptor byte) */
#define SDP_DE_NIL							0
#define SDP_DE_UINT							1
#define SDP_DE_INT							2
#define SDP_DE_UUID							3
#define SDP_DE_STRING						4
#define SDP_DE_BOOL							5
#define SDP_DE_SEQUENCE						6
#define SDP_DE_ALTERNATIVE					7
#define SDP_DE_URL							8


/* Data Element Size Descriptors (lower 3 bits of type descriptor byte) */
#define SDP_DE_SIZE_1						0	/* 1 byte (except NIL: 0 bytes) */
#define SDP_DE_SIZE_2						1	/* 2 bytes */
#define SDP_DE_SIZE_4						2	/* 4 bytes */
#define SDP_DE_SIZE_8						3	/* 8 bytes */
#define SDP_DE_SIZE_16						4	/* 16 bytes */
#define SDP_DE_SIZE_NEXT1					5	/* next 1 byte is size */
#define SDP_DE_SIZE_NEXT2					6	/* next 2 bytes are size */
#define SDP_DE_SIZE_NEXT4					7	/* next 4 bytes are size */


/* Data Element Header macro */
#define SDP_DE_HEADER(type, size_desc)		(((type) << 3) | (size_desc))


/* Well-known Attribute IDs */
#define SDP_ATTR_SERVICE_RECORD_HANDLE		0x0000
#define SDP_ATTR_SERVICE_CLASS_ID_LIST		0x0001
#define SDP_ATTR_SERVICE_RECORD_STATE		0x0002
#define SDP_ATTR_SERVICE_ID					0x0003
#define SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST	0x0004
#define SDP_ATTR_BROWSE_GROUP_LIST			0x0005
#define SDP_ATTR_LANGUAGE_BASE_ATTR_LIST	0x0006
#define SDP_ATTR_SERVICE_INFO_TIME_TO_LIVE	0x0007
#define SDP_ATTR_SERVICE_AVAILABILITY		0x0008
#define SDP_ATTR_PROFILE_DESCRIPTOR_LIST	0x0009
#define SDP_ATTR_SERVICE_NAME				0x0100
#define SDP_ATTR_SERVICE_DESCRIPTION		0x0101
#define SDP_ATTR_PROVIDER_NAME				0x0102
#define SDP_ATTR_SPECIFICATION_ID			0x0200
#define SDP_ATTR_VENDOR_ID					0x0201
#define SDP_ATTR_PRODUCT_ID					0x0202
#define SDP_ATTR_VERSION					0x0203
#define SDP_ATTR_PRIMARY_RECORD				0x0204
#define SDP_ATTR_VENDOR_ID_SOURCE			0x0205


/* Well-known UUID16 values */
#define SDP_UUID16_SDP						0x0001
#define SDP_UUID16_RFCOMM					0x0003
#define SDP_UUID16_L2CAP					0x0100
#define SDP_UUID16_SDP_SERVER				0x1000
#define SDP_UUID16_PUBLIC_BROWSE_ROOT		0x1002
#define SDP_UUID16_SERIAL_PORT				0x1101
#define SDP_UUID16_OBEX_OBJECT_PUSH			0x1105
#define SDP_UUID16_OBEX_FILE_TRANSFER		0x1106
#define SDP_UUID16_HFP_HF					0x111E
#define SDP_UUID16_HFP_AG					0x111F
#define SDP_UUID16_GENERIC_AUDIO			0x1203
#define SDP_UUID16_PBAP_PSE					0x112F
#define SDP_UUID16_PBAP_PCE					0x112E
#define SDP_UUID16_PBAP_PROFILE				0x1130
#define SDP_UUID16_A2DP_SOURCE				0x110A
#define SDP_UUID16_A2DP_SINK				0x110B
#define SDP_UUID16_AVRCP_TARGET				0x110C
#define SDP_UUID16_AVRCP					0x110E
#define SDP_UUID16_AVCTP					0x0017
#define SDP_UUID16_AVDTP					0x0019
#define SDP_UUID16_PNP_INFORMATION			0x1200


/* SDP PDU Header (5 bytes, network byte order) */
struct sdp_pdu_header {
	uint8	pdu_id;
	uint16	transaction_id;
	uint16	param_length;
} __attribute__ ((packed));

#define SDP_PDU_HEADER_SIZE		5


#endif /* _SDP_H_ */
