/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef _BTHCI_EVENT_LE_H_
#define _BTHCI_EVENT_LE_H_

#include <bluetooth/bluetooth.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_event.h>


/* LE Meta Event */
#define HCI_EVENT_LE_META					0x3E

struct hci_ev_le_meta {
	uint8	subevent;
} __attribute__ ((packed));


/* --- LE Connection Complete (subevent 0x01) --- */
#define HCI_LE_SUBEVENT_CONN_COMPLETE		0x01
struct hci_ev_le_conn_complete {
	uint8		status;
	uint16		handle;
	uint8		role;
	uint8		peer_address_type;
	bdaddr_t	peer_address;
	uint16		interval;
	uint16		latency;
	uint16		supervision_timeout;
	uint8		master_clock_accuracy;
} __attribute__ ((packed));


/* --- LE Advertising Report (subevent 0x02) --- */
#define HCI_LE_SUBEVENT_ADVERTISING_REPORT	0x02

/* Single advertising report entry (variable-length) */
struct hci_ev_le_advertising_info {
	uint8		event_type;
	uint8		address_type;
	bdaddr_t	address;
	uint8		data_length;
	/* uint8	data[data_length]; */
	/* int8	rssi; (follows data) */
} __attribute__ ((packed));

/* Advertising report event types */
#define HCI_LE_ADV_REPORT_IND				0x00
#define HCI_LE_ADV_REPORT_DIRECT_IND		0x01
#define HCI_LE_ADV_REPORT_SCAN_IND			0x02
#define HCI_LE_ADV_REPORT_NONCONN_IND		0x03
#define HCI_LE_ADV_REPORT_SCAN_RSP			0x04


/* --- LE Connection Update Complete (subevent 0x03) --- */
#define HCI_LE_SUBEVENT_CONN_UPDATE_COMPLETE	0x03
struct hci_ev_le_conn_update_complete {
	uint8	status;
	uint16	handle;
	uint16	interval;
	uint16	latency;
	uint16	supervision_timeout;
} __attribute__ ((packed));


/* --- LE Read Remote Features Complete (subevent 0x04) --- */
#define HCI_LE_SUBEVENT_READ_REMOTE_FEATURES_COMPLETE	0x04
struct hci_ev_le_read_remote_features_complete {
	uint8	status;
	uint16	handle;
	uint8	features[8];
} __attribute__ ((packed));


/* --- LE Long Term Key Request (subevent 0x05) --- */
#define HCI_LE_SUBEVENT_LTK_REQUEST		0x05
struct hci_ev_le_ltk_request {
	uint16	handle;
	uint8	random[8];
	uint16	ediv;
} __attribute__ ((packed));


#endif /* _BTHCI_EVENT_LE_H_ */
