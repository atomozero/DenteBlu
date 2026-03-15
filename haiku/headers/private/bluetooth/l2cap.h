/*
 * Copyright 2024, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _L2CAP_H_
#define _L2CAP_H_

#include <bluetooth/bluetooth.h>


/* Channel IDs */
/*! These are unique for a unit. Thus the total number of channels that a unit
 * can have open simultaneously is (L2CAP_LAST_CID - L2CAP_FIRST_CID) = 65471.
 * (This does not depend on the number of connections.) */
#define L2CAP_NULL_CID			0x0000
#define L2CAP_SIGNALING_CID		0x0001
#define L2CAP_CONNECTIONLESS_CID 0x0002
	/* 0x0003: reserved */
#define L2CAP_ATT_CID			0x0004
#define L2CAP_LE_SIGNALING_CID	0x0005
#define L2CAP_SMP_CID			0x0006
	/* 0x0007-0x003f: reserved */
#define L2CAP_FIRST_CID		0x0040
#define L2CAP_LAST_CID			0xffff


/* Idents */
/*! Command idents are unique within a connection, since there is only one
 * L2CAP_SIGNALING_CID. Thus only (L2CAP_LAST_IDENT - L2CAP_FIRST_IDENT),
 * i.e. 254, commands can be pending simultaneously for a connection. */
#define L2CAP_NULL_IDENT		0x00
#define L2CAP_FIRST_IDENT		0x01
#define L2CAP_LAST_IDENT		0xff


/* MTU */
#define L2CAP_MTU_MINIMUM		48
#define L2CAP_MTU_DEFAULT		672
#define L2CAP_MTU_MAXIMUM		0xffff


/* Timeouts */
#define L2CAP_FLUSH_TIMEOUT_DEFAULT	0xffff /* always retransmit */
#define L2CAP_LINK_TIMEOUT_DEFAULT	0xffff


/* Protocol/Service Multiplexer (PSM) values */
#define L2CAP_PSM_ANY		0x0000	/* Any/Invalid PSM */
#define L2CAP_PSM_SDP		0x0001	/* Service Discovery Protocol */
#define L2CAP_PSM_RFCOMM	0x0003	/* RFCOMM protocol */
#define L2CAP_PSM_TCS_BIN	0x0005	/* Telephony Control Protocol */
#define L2CAP_PSM_TCS_BIN_CORDLESS 0x0007 /* TCS cordless */
#define L2CAP_PSM_BNEP		0x000F	/* BNEP */
#define L2CAP_PSM_HID_CTRL	0x0011	/* HID control */
#define L2CAP_PSM_HID_INT	0x0013	/* HID interrupt */
#define L2CAP_PSM_UPnP		0x0015	/* UPnP (ESDP) */
#define L2CAP_PSM_AVCTP		0x0017	/* AVCTP */
#define L2CAP_PSM_AVDTP		0x0019	/* AVDTP */
	/* < 0x1000: reserved */
	/* >= 0x1000: dynamically assigned */


typedef struct {
	uint16	length;	/* payload size */
	uint16	dcid;	/* destination channel ID */
} _PACKED l2cap_basic_header;


/* ERTM (Enhanced Retransmission Mode) frame definitions */

/* Control field type detection */
#define L2CAP_CTRL_IS_IFRAME(ctrl)	(((ctrl) & 0x0001) == 0)
#define L2CAP_CTRL_IS_SFRAME(ctrl)	(((ctrl) & 0x0001) == 1)

/* I-frame control field (16-bit standard format):
 *   bit 0:     0 (I-frame type)
 *   bits 1-6:  TxSeq (transmit sequence number, 0-63)
 *   bit 7:     R (retransmission disable)
 *   bits 8-13: ReqSeq (request sequence = next expected, 0-63)
 *   bits 14-15: SAR (segmentation and reassembly) */
#define L2CAP_CTRL_TXSEQ_SHIFT		1
#define L2CAP_CTRL_TXSEQ_MASK		0x007E
#define L2CAP_CTRL_R_BIT			0x0080
#define L2CAP_CTRL_REQSEQ_SHIFT		8
#define L2CAP_CTRL_REQSEQ_MASK		0x3F00
#define L2CAP_CTRL_SAR_SHIFT		14
#define L2CAP_CTRL_SAR_MASK		0xC000

#define L2CAP_SAR_UNSEGMENTED		0x00
#define L2CAP_SAR_START				0x01
#define L2CAP_SAR_END				0x02
#define L2CAP_SAR_CONTINUE			0x03

/* S-frame control field (16-bit standard format):
 *   bit 0:     1 (S-frame type)
 *   bit 1:     0 (reserved)
 *   bits 2-3:  S (supervisory function)
 *   bit 4:     P (poll)
 *   bits 5-6:  0 (reserved)
 *   bit 7:     F (final)
 *   bits 8-13: ReqSeq
 *   bits 14-15: 0 (reserved) */
#define L2CAP_CTRL_STYPE_SHIFT		2
#define L2CAP_CTRL_STYPE_MASK		0x000C
#define L2CAP_CTRL_P_BIT			0x0010
#define L2CAP_CTRL_F_BIT			0x0080

#define L2CAP_SFRAME_RR				0x00	/* Receiver Ready */
#define L2CAP_SFRAME_REJ			0x01	/* Reject */
#define L2CAP_SFRAME_RNR			0x02	/* Receiver Not Ready */
#define L2CAP_SFRAME_SREJ			0x03	/* Selective Reject */

/* FCS option values */
#define L2CAP_FCS_NONE				0x00
#define L2CAP_FCS_CRC16				0x01


/* Connectionless traffic ("CLT") */
typedef struct {
	/* dcid == L2CAP_CONNECTIONLESS_CID (0x2) */
	uint16	psm;
} _PACKED l2cap_connectionless_header;
#define L2CAP_CONNECTIONLESS_MTU_MAXIMUM (L2CAP_MTU_MAXIMUM - sizeof(l2cap_connectionless_header))


typedef struct {
	uint8	code;   /* command opcode */
#define L2CAP_IS_SIGNAL_REQ(code) (((code) & 1) == 0)
#define L2CAP_IS_SIGNAL_RSP(code) (((code) & 1) == 1)
	uint8	ident;  /* identifier to match request and response */
	uint16	length; /* command parameters length */
} _PACKED l2cap_command_header;

#define L2CAP_COMMAND_REJECT_RSP	0x01
typedef struct {
	enum : uint16 {
		REJECTED_NOT_UNDERSTOOD	= 0x0000,
		REJECTED_MTU_EXCEEDED	= 0x0001,
		REJECTED_INVALID_CID	= 0x0002,
		/* 0x0003-0xffff: reserved */
	}; uint16 reason;
	/* data may follow */
} _PACKED l2cap_command_reject;

typedef union {
	struct {
		uint16	mtu; /* actual signaling MTU */
	} _PACKED mtu_exceeded;
	struct {
		uint16	scid; /* source (local) CID */
		uint16	dcid; /* destination (remote) CID */
	} _PACKED invalid_cid;
} l2cap_command_reject_data;


#define L2CAP_CONNECTION_REQ	0x02
typedef struct {
	uint16	psm;
	uint16	scid; /* source channel ID */
} _PACKED l2cap_connection_req;

#define L2CAP_CONNECTION_RSP	0x03
typedef struct {
	uint16	dcid;   /* destination channel ID */
	uint16	scid;   /* source channel ID */
	enum : uint16 {
		RESULT_SUCCESS					= 0x0000,
		RESULT_PENDING					= 0x0001,
		RESULT_PSM_NOT_SUPPORTED		= 0x0002,
		RESULT_SECURITY_BLOCK			= 0x0003,
		RESULT_NO_RESOURCES				= 0x0004,
		RESULT_INVALID_SCID				= 0x0005,
		RESULT_SCID_ALREADY_ALLOCATED	= 0x0006,
		/* 0x0007-0xffff: reserved */
	}; uint16 result;
	enum : uint16 {
		NO_STATUS_INFO					= 0x0000,
		STATUS_AUTHENTICATION_PENDING	= 0x0001,
		STATUS_AUTHORIZATION_PENDING	= 0x0002,
		/* 0x0003-0xffff: reserved */
	}; uint16 status; /* only defined if result = pending */
} _PACKED l2cap_connection_rsp;


#define L2CAP_CONFIGURATION_REQ	0x04
typedef struct {
	uint16	dcid;  /* destination channel ID */
	uint16	flags;
	/* options may follow */
} _PACKED l2cap_configuration_req;

#define L2CAP_CONFIGURATION_RSP	0x05
typedef struct {
	uint16	scid;   /* source channel ID */
	uint16	flags;
#define L2CAP_CFG_FLAG_CONTINUATION		0x0001
	enum : uint16 {
		RESULT_SUCCESS				= 0x0000,
		RESULT_UNACCEPTABLE_PARAMS	= 0x0001,
		RESULT_REJECTED				= 0x0002,
		RESULT_UNKNOWN_OPTION		= 0x0003,
		RESULT_PENDING				= 0x0004,
		RESULT_FLOW_SPEC_REJECTED	= 0x0005,
		/* 0x0006-0xffff: reserved */
	}; uint16 result;
	/* options may follow */
} _PACKED l2cap_configuration_rsp;

typedef struct {
	enum : uint8 {
		OPTION_MTU				= 0x01,
		OPTION_FLUSH_TIMEOUT	= 0x02,
		OPTION_QOS				= 0x03,
		OPTION_RFC				= 0x04,
		OPTION_FCS				= 0x05,

		OPTION_HINT_BIT			= 0x80,
	}; uint8 type;
	uint8 length;
	/* value follows */
} _PACKED l2cap_configuration_option;

typedef struct {
	uint8 flags;				/* reserved for future use */
	uint8 service_type;			/* 1 = best effort */
	uint32 token_rate;			/* average bytes per second */
	uint32 token_bucket_size;	/* max burst bytes */
	uint32 peak_bandwidth;		/* bytes per second */
	uint32 access_latency;		/* microseconds */
	uint32 delay_variation;		/* microseconds */
} _PACKED l2cap_qos;

typedef struct {
	enum : uint8 {
		MODE_BASIC				= 0x00,
		MODE_RETRANSMISSION		= 0x01,
		MODE_FLOW_CONTROL		= 0x02,
		MODE_ERTM				= 0x03,
		MODE_STREAMING			= 0x04,
	}; uint8 mode;
	uint8 tx_window_size;
	uint8 max_transmit;
	uint16 retransmission_timeout;
	uint16 monitor_timeout;
	uint16 max_pdu_size;
} _PACKED l2cap_rfc;

typedef union {
	uint16		mtu;
	uint16		flush_timeout;
	l2cap_qos	qos;
	l2cap_rfc	rfc;
} l2cap_configuration_option_value;


#define L2CAP_DISCONNECTION_REQ	0x06
typedef struct {
	uint16	dcid; /* destination channel ID */
	uint16	scid; /* source channel ID */
} _PACKED l2cap_disconnection_req;

#define L2CAP_DISCONNECTION_RSP	0x07
typedef l2cap_disconnection_req l2cap_disconnection_rsp;


#define L2CAP_ECHO_REQ	0x08
#define L2CAP_ECHO_RSP	0x09

#define L2CAP_MAX_ECHO_SIZE \
	(L2CAP_MTU_MAXIMUM - sizeof(l2cap_command_header))


#define L2CAP_INFORMATION_REQ	0x0a
typedef struct {
	enum : uint16 {
		TYPE_CONNECTIONLESS_MTU	= 0x0001,
		TYPE_EXTENDED_FEATURES	= 0x0002,
		TYPE_FIXED_CHANNELS		= 0x0003,
			/* 0x0004-0xffff: reserved */
	}; uint16 type;
} _PACKED l2cap_information_req;

#define L2CAP_INFORMATION_RSP	0x0b
typedef struct {
	uint16	type;
	enum : uint16 {
		RESULT_SUCCESS			= 0x0000,
		RESULT_NOT_SUPPORTED	= 0x0001,
	}; uint16 result;
	/* data may follow */
} _PACKED l2cap_information_rsp;

typedef union {
	uint16 mtu;
	uint32 extended_features;
} _PACKED l2cap_information_rsp_data;


/* LE Connection Parameter Update (LE Signaling CID 0x0005) */
#define L2CAP_CONN_PARAM_UPDATE_REQ		0x12
typedef struct {
	uint16	interval_min;
	uint16	interval_max;
	uint16	latency;
	uint16	timeout;
} _PACKED l2cap_conn_param_update_req;

#define L2CAP_CONN_PARAM_UPDATE_RSP		0x13
typedef struct {
	enum : uint16 {
		RESULT_ACCEPTED	= 0x0000,
		RESULT_REJECTED	= 0x0001,
	}; uint16 result;
} _PACKED l2cap_conn_param_update_rsp;


#endif /* _L2CAP_H_ */
