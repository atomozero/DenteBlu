/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * avdtp.h — Audio/Video Distribution Transport Protocol constants.
 * Based on Bluetooth Core Spec Vol 3, Part A (AVDTP).
 */
#ifndef _AVDTP_H_
#define _AVDTP_H_

#include <SupportDefs.h>


/* AVDTP signal identifiers */
#define AVDTP_DISCOVER				0x01
#define AVDTP_GET_CAPABILITIES		0x02
#define AVDTP_SET_CONFIGURATION		0x03
#define AVDTP_GET_CONFIGURATION		0x04
#define AVDTP_RECONFIGURE			0x05
#define AVDTP_OPEN					0x06
#define AVDTP_START					0x07
#define AVDTP_CLOSE					0x08
#define AVDTP_SUSPEND				0x09
#define AVDTP_ABORT					0x0A
#define AVDTP_SECURITY_CONTROL		0x0B
#define AVDTP_GET_ALL_CAPABILITIES	0x0C
#define AVDTP_DELAY_REPORT			0x0D

/* Message types */
#define AVDTP_MSG_TYPE_COMMAND		0x00
#define AVDTP_MSG_TYPE_GENERAL_REJECT 0x01
#define AVDTP_MSG_TYPE_RESPONSE_ACCEPT 0x02
#define AVDTP_MSG_TYPE_RESPONSE_REJECT 0x03

/* Packet types */
#define AVDTP_PKT_TYPE_SINGLE		0x00
#define AVDTP_PKT_TYPE_START		0x01
#define AVDTP_PKT_TYPE_CONTINUE		0x02
#define AVDTP_PKT_TYPE_END			0x03

/* Service categories for capabilities */
#define AVDTP_MEDIA_TRANSPORT		0x01
#define AVDTP_REPORTING				0x02
#define AVDTP_RECOVERY				0x03
#define AVDTP_CONTENT_PROTECTION	0x04
#define AVDTP_HEADER_COMPRESSION	0x05
#define AVDTP_MULTIPLEXING			0x06
#define AVDTP_MEDIA_CODEC			0x07
#define AVDTP_DELAY_REPORTING		0x08

/* Media types */
#define AVDTP_MEDIA_TYPE_AUDIO		0x00
#define AVDTP_MEDIA_TYPE_VIDEO		0x01
#define AVDTP_MEDIA_TYPE_MULTIMEDIA	0x02

/* Codec types for audio */
#define AVDTP_CODEC_SBC				0x00
#define AVDTP_CODEC_MPEG12			0x01
#define AVDTP_CODEC_MPEG24_AAC		0x02
#define AVDTP_CODEC_ATRAC			0x04
#define AVDTP_CODEC_VENDOR			0xFF

/* Stream Endpoint type */
#define AVDTP_SEP_SOURCE			0x00
#define AVDTP_SEP_SINK				0x01

/* Error codes */
#define AVDTP_ERR_BAD_HEADER		0x01
#define AVDTP_ERR_BAD_LENGTH		0x11
#define AVDTP_ERR_BAD_ACP_SEID		0x12
#define AVDTP_ERR_SEP_IN_USE		0x13
#define AVDTP_ERR_SEP_NOT_IN_USE	0x14
#define AVDTP_ERR_BAD_SERV_CATEGORY	0x17
#define AVDTP_ERR_BAD_PAYLOAD_FORMAT 0x18
#define AVDTP_ERR_NOT_SUPPORTED_COMMAND 0x19
#define AVDTP_ERR_INVALID_CAPABILITIES 0x1A
#define AVDTP_ERR_BAD_STATE			0x31


/*
 * AVDTP signal packet header (single packet):
 *   byte 0: [7:4] transaction label, [3:2] packet type, [1:0] message type
 *   byte 1: signal identifier
 *
 * For single packets, the full header is 2 bytes.
 */
struct avdtp_header {
	uint8	byte0;		/* txLabel:4, pktType:2, msgType:2 */
	uint8	signal_id;
} __attribute__ ((packed));

#define AVDTP_HEADER_SIZE	2

#define AVDTP_GET_TX_LABEL(b)		(((b) >> 4) & 0x0F)
#define AVDTP_GET_PKT_TYPE(b)		(((b) >> 2) & 0x03)
#define AVDTP_GET_MSG_TYPE(b)		((b) & 0x03)

#define AVDTP_SET_HEADER(txLabel, pktType, msgType) \
	((((txLabel) & 0x0F) << 4) | (((pktType) & 0x03) << 2) \
		| ((msgType) & 0x03))


/* SBC codec information element (4 bytes in capability) */
struct sbc_codec_info {
	uint8	byte0;	/* sampling freq (4 bits) | channel mode (4 bits) */
	uint8	byte1;	/* block length (4 bits) | subbands (2 bits) | alloc (2 bits) */
	uint8	min_bitpool;
	uint8	max_bitpool;
} __attribute__ ((packed));

/* SBC sampling frequencies (byte0 upper nibble) */
#define SBC_FREQ_16000		0x80
#define SBC_FREQ_32000		0x40
#define SBC_FREQ_44100		0x20
#define SBC_FREQ_48000		0x10

/* SBC channel modes (byte0 lower nibble) */
#define SBC_CHANNEL_MONO			0x08
#define SBC_CHANNEL_DUAL			0x04
#define SBC_CHANNEL_STEREO			0x02
#define SBC_CHANNEL_JOINT_STEREO	0x01

/* SBC block lengths (byte1 upper nibble) */
#define SBC_BLOCK_4			0x80
#define SBC_BLOCK_8			0x40
#define SBC_BLOCK_12		0x20
#define SBC_BLOCK_16		0x10

/* SBC subbands (byte1 bits 3-2) */
#define SBC_SUBBANDS_4		0x08
#define SBC_SUBBANDS_8		0x04

/* SBC allocation method (byte1 bits 1-0) */
#define SBC_ALLOC_SNR		0x02
#define SBC_ALLOC_LOUDNESS	0x01


/* Stream Endpoint discovery info (from AVDTP_DISCOVER response) */
struct avdtp_sep_info {
	uint8	seid_info;	/* [7:2] SEID, [1] inUse, [0] reserved */
	uint8	media_info;	/* [7:4] media type, [3] TSEP (0=SRC,1=SNK), [2:0] reserved */
} __attribute__ ((packed));

#define AVDTP_SEP_SEID(s)		(((s)->seid_info >> 2) & 0x3F)
#define AVDTP_SEP_IN_USE(s)		(((s)->seid_info >> 1) & 0x01)
#define AVDTP_SEP_MEDIA_TYPE(s)	(((s)->media_info >> 4) & 0x0F)
#define AVDTP_SEP_TSEP(s)		(((s)->media_info >> 3) & 0x01)


/* RTP header for media packets (12 bytes) */
struct rtp_header {
	uint8	byte0;		/* V:2, P:1, X:1, CC:4 */
	uint8	byte1;		/* M:1, PT:7 */
	uint16	seq_number;
	uint32	timestamp;
	uint32	ssrc;
} __attribute__ ((packed));

#define RTP_HEADER_SIZE		12

/* A2DP media payload header (1 byte for SBC) */
struct a2dp_sbc_media_header {
	uint8	byte0;		/* [7:4] num_frames, [3] RFA, [2] L, [1] S, [0] F/L */
} __attribute__ ((packed));

#define A2DP_SBC_NUM_FRAMES(b)	(((b) >> 4) & 0x0F)


#endif /* _AVDTP_H_ */
