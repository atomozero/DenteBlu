/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _BTHCI_COMMAND_LE_H_
#define _BTHCI_COMMAND_LE_H_

#include <bluetooth/bluetooth.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_command.h>


/* LE Controller Commands (OGF 0x08) */
#define OGF_LE_CONTROL		0x08


/* --- OCF_LE_SET_EVENT_MASK (0x0001) --- */
#define OCF_LE_SET_EVENT_MASK			0x0001
struct hci_cp_le_set_event_mask {
	uint8	mask[8];
} __attribute__ ((packed));


/* --- OCF_LE_READ_BUFFER_SIZE (0x0002) --- */
#define OCF_LE_READ_BUFFER_SIZE			0x0002
struct hci_rp_le_read_buffer_size {
	uint8	status;
	uint16	le_mtu;
	uint8	le_max_pkt;
} __attribute__ ((packed));


/* --- OCF_LE_READ_LOCAL_FEATURES (0x0003) --- */
#define OCF_LE_READ_LOCAL_FEATURES		0x0003
struct hci_rp_le_read_local_features {
	uint8	status;
	uint8	features[8];
} __attribute__ ((packed));


/* --- OCF_LE_SET_RANDOM_ADDRESS (0x0005) --- */
#define OCF_LE_SET_RANDOM_ADDRESS		0x0005
struct hci_cp_le_set_random_address {
	bdaddr_t	bdaddr;
} __attribute__ ((packed));


/* --- OCF_LE_SET_ADV_PARAMS (0x0006) --- */
#define OCF_LE_SET_ADV_PARAMS			0x0006
struct hci_cp_le_set_adv_params {
	uint16	min_interval;
	uint16	max_interval;
	uint8	type;
	uint8	own_address_type;
	uint8	direct_address_type;
	bdaddr_t	direct_address;
	uint8	channel_map;
	uint8	filter_policy;
} __attribute__ ((packed));

/* Advertising types */
#define HCI_LE_ADV_IND				0x00
#define HCI_LE_ADV_DIRECT_IND_HIGH	0x01
#define HCI_LE_ADV_SCAN_IND		0x02
#define HCI_LE_ADV_NONCONN_IND		0x03
#define HCI_LE_ADV_DIRECT_IND_LOW	0x04


/* --- OCF_LE_SET_ADV_DATA (0x0008) --- */
#define OCF_LE_SET_ADV_DATA				0x0008
struct hci_cp_le_set_adv_data {
	uint8	length;
	uint8	data[31];
} __attribute__ ((packed));


/* --- OCF_LE_SET_ADV_ENABLE (0x000A) --- */
#define OCF_LE_SET_ADV_ENABLE			0x000A
struct hci_cp_le_set_adv_enable {
	uint8	enable;
} __attribute__ ((packed));


/* --- OCF_LE_SET_SCAN_PARAMS (0x000B) --- */
#define OCF_LE_SET_SCAN_PARAMS			0x000B
struct hci_cp_le_set_scan_params {
	uint8	type;
	uint16	interval;
	uint16	window;
	uint8	own_address_type;
	uint8	filter_policy;
} __attribute__ ((packed));

/* Scan types */
#define HCI_LE_SCAN_PASSIVE		0x00
#define HCI_LE_SCAN_ACTIVE		0x01


/* --- OCF_LE_SET_SCAN_ENABLE (0x000C) --- */
#define OCF_LE_SET_SCAN_ENABLE			0x000C
struct hci_cp_le_set_scan_enable {
	uint8	enable;
	uint8	filter_dup;
} __attribute__ ((packed));


/* --- OCF_LE_CREATE_CONN (0x000D) --- */
#define OCF_LE_CREATE_CONN				0x000D
struct hci_cp_le_create_conn {
	uint16	scan_interval;
	uint16	scan_window;
	uint8	filter_policy;
	uint8	peer_address_type;
	bdaddr_t	peer_address;
	uint8	own_address_type;
	uint16	conn_interval_min;
	uint16	conn_interval_max;
	uint16	conn_latency;
	uint16	supervision_timeout;
	uint16	min_ce_length;
	uint16	max_ce_length;
} __attribute__ ((packed));


/* --- OCF_LE_CREATE_CONN_CANCEL (0x000E) --- */
#define OCF_LE_CREATE_CONN_CANCEL		0x000E


/* --- OCF_LE_START_ENCRYPTION (0x0019) --- */
#define OCF_LE_START_ENCRYPTION			0x0019
struct hci_cp_le_start_encryption {
	uint16	handle;
	uint8	random[8];
	uint16	ediv;
	uint8	ltk[16];
} __attribute__ ((packed));


/* --- OCF_LE_LTK_REQUEST_REPLY (0x001A) --- */
#define OCF_LE_LTK_REQUEST_REPLY		0x001A
struct hci_cp_le_ltk_request_reply {
	uint16	handle;
	uint8	ltk[16];
} __attribute__ ((packed));

/* --- OCF_LE_LTK_REQUEST_NEG_REPLY (0x001B) --- */
#define OCF_LE_LTK_REQUEST_NEG_REPLY	0x001B
struct hci_cp_le_ltk_request_neg_reply {
	uint16	handle;
} __attribute__ ((packed));


/* --- OCF_LE_SET_DATA_LENGTH (0x0022) --- */
#define OCF_LE_SET_DATA_LENGTH			0x0022
struct hci_cp_le_set_data_length {
	uint16	handle;
	uint16	tx_octets;
	uint16	tx_time;
} __attribute__ ((packed));

struct hci_rp_le_set_data_length {
	uint8	status;
	uint16	handle;
} __attribute__ ((packed));


#endif /* _BTHCI_COMMAND_LE_H_ */
