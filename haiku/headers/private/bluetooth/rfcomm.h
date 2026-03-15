/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * RFCOMM protocol constants and frame structures.
 * Based on Bluetooth Core Specification v5.4, Part F (RFCOMM with TS 07.10).
 */
#ifndef _RFCOMM_H_
#define _RFCOMM_H_

#include <SupportDefs.h>


/* Frame types (Control field, with P/F bit cleared) */
#define RFCOMM_SABM				0x2F	/* Set Asynchronous Balanced Mode */
#define RFCOMM_UA				0x63	/* Unnumbered Acknowledgement */
#define RFCOMM_DM				0x0F	/* Disconnected Mode */
#define RFCOMM_DISC				0x43	/* Disconnect */
#define RFCOMM_UIH				0xEF	/* Unnumbered Information with Header check */

/* P/F bit in control byte */
#define RFCOMM_PF				0x10

/* Frame type with P/F set (used for SABM, UA, DISC commands) */
#define RFCOMM_SABM_PF			(RFCOMM_SABM | RFCOMM_PF)	/* 0x3F */
#define RFCOMM_UA_PF			(RFCOMM_UA | RFCOMM_PF)		/* 0x73 */
#define RFCOMM_DM_PF			(RFCOMM_DM | RFCOMM_PF)		/* 0x1F */
#define RFCOMM_DISC_PF			(RFCOMM_DISC | RFCOMM_PF)	/* 0x53 */

/* Strip P/F bit for type comparison */
#define RFCOMM_FRAME_TYPE(ctrl)	((ctrl) & ~RFCOMM_PF)


/* Address byte:  [DLCI(6) | C/R(1) | EA(1)]
 *   bits 7..2 = DLCI
 *   bit  1    = C/R (Command/Response)
 *   bit  0    = EA  (Extension Address, always 1 for RFCOMM)
 */
#define RFCOMM_ADDR(cr, dlci)	(((dlci) << 2) | ((cr) << 1) | 0x01)
#define RFCOMM_GET_DLCI(addr)	(((addr) >> 2) & 0x3F)
#define RFCOMM_GET_CR(addr)		(((addr) >> 1) & 0x01)

/* DLCI encoding: DLCI = (server_channel << 1) | direction_bit
 *   direction_bit = 1 for initiator, 0 for responder
 */
#define RFCOMM_DLCI(dir, channel)	(((channel) << 1) | (dir))
#define RFCOMM_CHANNEL(dlci)		((dlci) >> 1)
#define RFCOMM_DIRECTION(dlci)		((dlci) & 0x01)

/* Maximum DLCI value (6 bits, but DLCI 0 is control channel) */
#define RFCOMM_MAX_DLCI			61


/* Length encoding in RFCOMM frames:
 *   If length <= 127: single byte, EA=1: [(len << 1) | 0x01]
 *   If length > 127:  two bytes, EA=0 first: [(lenLow << 1) | 0x00][lenHigh]
 */
#define RFCOMM_LEN_EA(b)		((b) & 0x01)
#define RFCOMM_LEN_1BYTE(len)	(((len) << 1) | 0x01)
#define RFCOMM_GET_LEN1(b)		((b) >> 1)
#define RFCOMM_LEN_2BYTE_LO(len) (((len) & 0x7F) << 1)
#define RFCOMM_LEN_2BYTE_HI(len) ((len) >> 7)


/* Multiplexer commands (sent as UIH on DLCI 0)
 *
 * MCC type byte layout: [T5 T4 T3 T2 T1 T0 C/R EA]
 *   bits 2-7: 6-bit command type
 *   bit  1:   C/R (1=Command, 0=Response)
 *   bit  0:   EA  (always 1)
 *
 * Type constants below are the 6-bit type value shifted into bits 2-7,
 * matching the BlueZ/Linux convention.
 */
#define RFCOMM_MCC_PN			0x80	/* DLC Parameter Negotiation */
#define RFCOMM_MCC_MSC			0xE0	/* Modem Status Command */
#define RFCOMM_MCC_RPN			0x90	/* Remote Port Negotiation */
#define RFCOMM_MCC_RLS			0x50	/* Remote Line Status */
#define RFCOMM_MCC_PSC			0x40	/* Power Saving Control */
#define RFCOMM_MCC_CLD			0xC0	/* Close Down (Multiplexer) */
#define RFCOMM_MCC_TEST			0x20	/* Test */
#define RFCOMM_MCC_FCON			0xA0	/* Flow Control On */
#define RFCOMM_MCC_FCOFF		0x60	/* Flow Control Off */
#define RFCOMM_MCC_NSC			0x10	/* Non-Supported Command */

/* MCC type byte: type (bits 2-7) | C/R (bit 1) | EA (bit 0) */
#define RFCOMM_MCC_CMD(type)	((type) | 0x02 | 0x01)
/* MCC type byte for response (C/R=0) */
#define RFCOMM_MCC_RSP(type)	((type) | 0x00 | 0x01)
/* Extract MCC type from type byte (mask out C/R and EA) */
#define RFCOMM_MCC_TYPE(b)		((b) & 0xFC)
/* Check if MCC is command (C/R bit set) */
#define RFCOMM_MCC_IS_CMD(b)	((b) & 0x02)


/* PN (Parameter Negotiation) command parameters — 8 bytes */
struct rfcomm_pn_params {
	uint8	dlci;			/* DLCI (6 bits) */
	uint8	flow_control;	/* 0x00 = no credit-based, 0xF0 = credit-based */
	uint8	priority;		/* 0-63 */
	uint8	ack_timer;		/* T1, not used in BT (set to 0) */
	uint16	mtu;			/* Maximum frame size (little-endian) */
	uint8	max_retrans;	/* N2, not used in BT (set to 0) */
	uint8	credits;		/* Initial credits (if credit-based flow) */
} __attribute__((packed));

#define RFCOMM_PN_SIZE			8


/* MSC (Modem Status Command) parameters — 2 or 3 bytes */
struct rfcomm_msc_params {
	uint8	dlci_addr;		/* DLCI address byte (same format as frame addr) */
	uint8	signals;		/* Modem signal bits (V.24) */
	/* Optional: uint8 break_signal; */
} __attribute__((packed));

/* Modem signal bits in MSC */
#define RFCOMM_MSC_FC			0x02	/* Flow Control (FC) */
#define RFCOMM_MSC_RTC			0x04	/* Ready To Communicate (DTR) */
#define RFCOMM_MSC_RTR			0x08	/* Ready To Receive (RTS) */
#define RFCOMM_MSC_IC			0x40	/* Incoming Call (Ring) */
#define RFCOMM_MSC_DV			0x80	/* Data Valid (DSR/DCD) */

/* Typical initial modem signals: DTR + RTS + DSR active */
#define RFCOMM_MSC_DEFAULT		(RFCOMM_MSC_RTC | RFCOMM_MSC_RTR | RFCOMM_MSC_DV)


/* Default MTU for RFCOMM frames.
 * TS 07.10 default is 127; we propose a large value and let the
 * remote side negotiate down via PN.  The actual frame size is
 * also limited by the L2CAP MTU (outgoing_mtu). */
#define RFCOMM_DEFAULT_MTU		4096

/* Maximum number of server channels (1-30) */
#define RFCOMM_MAX_CHANNELS		30

/* RFCOMM FCS polynomial: x^8 + x^2 + x + 1 (reversed: 0xE0) */
#define RFCOMM_FCS_POLYNOMIAL	0xE0

/* FCS check value (remainder when FCS is correct) */
#define RFCOMM_FCS_CHECK		0xCF


#endif /* _RFCOMM_H_ */
