/*
 * Copyright 2007 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 * Copyright 2008 Mika Lindqvist, monni1995_at_gmail.com
 * All rights reserved. Distributed under the terms of the MIT License.
 */


#include "h2transactions.h"

#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_event.h>
#include <bluetooth/HCI/btHCI_acl.h>

#include <ByteOrder.h>
#include <kernel.h>
#include <malloc.h>
#include <string.h>

#include "h2debug.h"
#include "h2generic.h"
#include "h2upper.h"
#include "h2util.h"
#include "snet_buffer.h"


//#define DUMP_BUFFERS

/* KDL-surviving ring buffer trace for debugging USB buffer lifecycle */
#define BT_TRACE_RING_SIZE 64

struct bt_trace_entry {
	uint64		timestamp;
	const char*	op;
	void*		ptr;
	uint32		extra;
	status_t	status;
};

static bt_trace_entry sTraceRing[BT_TRACE_RING_SIZE];
static int32 sTraceIndex = 0;

static void
bt_trace(const char* op, void* ptr, uint32 extra, status_t status)
{
	int32 idx = atomic_add(&sTraceIndex, 1) % BT_TRACE_RING_SIZE;
	sTraceRing[idx].timestamp = system_time();
	sTraceRing[idx].op = op;
	sTraceRing[idx].ptr = ptr;
	sTraceRing[idx].extra = extra;
	sTraceRing[idx].status = status;
}


int
dump_bt_trace(int argc, char** argv)
{
	int32 current = sTraceIndex;
	int32 start = (current >= BT_TRACE_RING_SIZE)
		? (current - BT_TRACE_RING_SIZE) : 0;

	kprintf("BT USB trace (last %d entries):\n",
		(int)min_c(current, BT_TRACE_RING_SIZE));

	for (int32 i = start; i < current; i++) {
		bt_trace_entry& e = sTraceRing[i % BT_TRACE_RING_SIZE];
		kprintf("  [%" B_PRIu64 "] %-20s ptr=%p extra=0x%x status=%s\n",
			e.timestamp, e.op, e.ptr, e.extra, strerror(e.status));
	}
	return 0;
}


/* Forward declaration */

void acl_tx_complete(void* cookie, status_t status, void* data, size_t actual_len);
void acl_rx_complete(void* cookie, status_t status, void* data, size_t actual_len);
void command_complete(void* cookie, status_t status, void* data, size_t actual_len);
void event_complete(void* cookie, status_t status, void* data, size_t actual_len);


static status_t
assembly_rx(bt_usb_dev* bdev, bt_packet_t type, void* data, int count)
{
	bdev->stat.bytesRX += count;

	return btDevices->PostTransportPacket(bdev->hdev, type, data, count);

}


#if 0
#pragma mark --- RX Complete ---
#endif

void
event_complete(void* cookie, status_t status, void* data, size_t actual_len)
{
	bt_usb_dev* bdev = (bt_usb_dev*)cookie;
	// bt_usb_dev* bdev = fetch_device(cookie, 0); -> safer / slower option
	status_t error;

	bt_trace("evt_rx_enter", data, actual_len, status);

	if (bdev == NULL)
		return;

	if (status == B_CANCELED || status == B_DEV_CRC_ERROR)
		return;

	if (status != B_OK || actual_len == 0)
		goto resubmit;

	if (assembly_rx(bdev, BT_EVENT, data, actual_len) == B_OK) {
		bdev->stat.successfulTX++;
	} else {
		bdev->stat.errorRX++;
	}

resubmit:

	error = usb->queue_interrupt(bdev->intr_in_ep->handle, data,
		max_c(HCI_MAX_EVENT_SIZE, bdev->max_packet_size_intr_in),
		event_complete, bdev);

	if (error != B_OK) {
		reuse_room(&bdev->eventRoom, data);
		bdev->stat.rejectedRX++;
		ERROR("%s: RX event resubmittion failed %s\n", __func__,
			strerror(error));
	} else {
		bdev->stat.acceptedRX++;
	}
	bt_trace("evt_rx_exit", data, actual_len, status);
}


void
acl_rx_complete(void* cookie, status_t status, void* data, size_t actual_len)
{
	bt_usb_dev* bdev = (bt_usb_dev*)cookie;
	// bt_usb_dev* bdev = fetch_device(cookie, 0); -> safer / slower option
	status_t error;

	bt_trace("acl_rx_enter", data, actual_len, status);

	if (bdev == NULL)
		return;

	if (status == B_CANCELED || status == B_DEV_CRC_ERROR)
		return;

	if (status != B_OK || actual_len == 0)
		goto resubmit;

	if (assembly_rx(bdev, BT_ACL, data, actual_len) == B_OK) {
		bdev->stat.successfulRX++;
	} else {
		bdev->stat.errorRX++;
	}

resubmit:

	error = usb->queue_bulk(bdev->bulk_in_ep->handle, data,
		max_c(HCI_MAX_FRAME_SIZE, bdev->max_packet_size_bulk_in),
		acl_rx_complete, (void*) bdev);

	if (error != B_OK) {
		reuse_room(&bdev->aclRoom, data);
		bdev->stat.rejectedRX++;
		ERROR("%s: RX acl resubmittion failed %s\n", __func__, strerror(error));
	} else {
		bdev->stat.acceptedRX++;
	}
	bt_trace("acl_rx_exit", data, actual_len, status);
}


#if 0
#pragma mark --- RX ---
#endif

status_t
submit_rx_event(bt_usb_dev* bdev)
{
	size_t size = max_c(HCI_MAX_EVENT_SIZE, bdev->max_packet_size_intr_in);
	void* buf = alloc_room(&bdev->eventRoom, size);
	status_t status;

	if (buf == NULL)
		return ENOMEM;

	status = usb->queue_interrupt(bdev->intr_in_ep->handle,	buf, size,
		event_complete, (void*)bdev);

	if (status != B_OK) {
		reuse_room(&bdev->eventRoom, buf); // reuse allocated one
		bdev->stat.rejectedRX++;
	} else {
		bdev->stat.acceptedRX++;
		TRACE("%s: Accepted RX Event %d\n", __func__, bdev->stat.acceptedRX);
	}

	return status;
}


status_t
submit_rx_acl(bt_usb_dev* bdev)
{
	size_t size = max_c(HCI_MAX_FRAME_SIZE, bdev->max_packet_size_bulk_in);
	void* buf = alloc_room(&bdev->aclRoom, size);
	status_t status;

	if (buf == NULL)
		return ENOMEM;

	status = usb->queue_bulk(bdev->bulk_in_ep->handle, buf, size,
		acl_rx_complete, bdev);

	if (status != B_OK) {
		reuse_room(&bdev->aclRoom, buf); // reuse allocated
		bdev->stat.rejectedRX++;
	} else {
		bdev->stat.acceptedRX++;
	}

	return status;
}


status_t
submit_rx_sco(bt_usb_dev* bdev)
{
#ifdef BLUETOOTH_SUPPORTS_SCO
	if (bdev->iso_in_ep == NULL || bdev->sco_interface == NULL)
		return B_DEV_NOT_READY;

	// Activate the SCO alt setting to enable the isochronous endpoints.
	// Alt 0 has zero bandwidth; we need alt >= 1 for actual data.
	status_t err = usb->set_alt_interface(bdev->dev,
		bdev->sco_interface);
	if (err != B_OK) {
		ERROR("%s: failed to set SCO alt interface: %s\n",
			__func__, strerror(err));
		return err;
	}

	// Queue isochronous IN transfer for SCO audio data
	size_t size = bdev->max_packet_size_iso_in;
	if (size == 0)
		size = 49; // mSBC max frame size

	void* buf = malloc(size);
	if (buf == NULL)
		return B_NO_MEMORY;

	usb_iso_packet_descriptor descs[1];
	descs[0].request_length = size;

	err = usb->queue_isochronous(bdev->iso_in_ep->handle,
		buf, size, descs, 1,
		NULL, 0, // no starting frame preference
		(usb_callback_func)acl_rx_complete, bdev);
	if (err != B_OK) {
		ERROR("%s: SCO RX queue failed: %s\n", __func__,
			strerror(err));
		free(buf);
		return err;
	}

	TRACE("%s: SCO RX queued (%" B_PRIuSIZE " bytes)\n",
		__func__, size);
	return B_OK;
#else
	return B_NOT_SUPPORTED;
#endif
}


#if 0
#pragma mark --- TX Complete ---
#endif

void
command_complete(void* cookie, status_t status, void* data, size_t actual_len)
{
	snet_buffer* snbuf = (snet_buffer*)cookie;

	bt_trace("cmd_tx_enter", snbuf, actual_len, status);

	// Guard against double-completion (same EHCI race as acl_tx_complete).
	bt_usb_dev* bdev = (bt_usb_dev*)snb_atomic_take_cookie(snbuf);
	if (bdev == NULL) {
		bt_trace("cmd_tx_double", snbuf, 0, B_ERROR);
		return;
	}

	if (status == B_OK) {
		bdev->stat.successfulTX++;
		bdev->stat.bytesTX += actual_len;
	} else {
		bdev->stat.errorTX++;
	}

	snb_park(&bdev->snetBufferRecycleTrash, snbuf);
	bt_trace("cmd_tx_exit", snbuf, actual_len, status);

#ifdef BT_RESCHEDULING_AFTER_COMPLETITIONS
	schedTxProcessing(bdev);
#endif
}


void
acl_tx_complete(void* cookie, status_t status, void* data, size_t actual_len)
{
	net_buffer* nbuf = (net_buffer*)cookie;

	// Check if buffer was already freed before we even touch it.
	// If freed, all memory is 0xdeadbeef poison pattern.
	if ((uintptr_t)nbuf->link.next == 0xdeadbeefdeadbeefULL) {
		bt_trace("acl_tx_STALE!", nbuf, 0xdead, status);
		dprintf("bt acl_tx_complete: STALE buffer %p (already freed by "
			"someone else), skipping\n", nbuf);
		return;
	}

	bt_trace("acl_tx_enter", nbuf, nbuf->size, status);

	// Check our sentinel — we set msg_flags in submit_tx_acl.
	// 0xBD01 = direct pointer, 0xBD02 = malloc'd flat copy that needs free.
	bool flatCopy = (nbuf->msg_flags == 0xBD02);
	if (nbuf->msg_flags != 0xBD01 && nbuf->msg_flags != 0xBD02) {
		bt_trace("acl_tx_NOSENT!", nbuf, nbuf->msg_flags, status);
		dprintf("bt acl_tx_complete: buffer %p has wrong sentinel "
			"msg_flags=0x%" B_PRIx32 " (expected 0xBD01/02), skipping\n",
			nbuf, nbuf->msg_flags);
		return;
	}
	nbuf->msg_flags = 0; // clear sentinel

	// Guard against double-completion due to EHCI cancel/complete race.
	int32 oldType = atomic_get_and_set((int32*)&nbuf->type, 0);
	if (oldType == 0) {
		bt_trace("acl_tx_double", nbuf, 0, B_ERROR);
		return;
	}

	bt_usb_dev* bdev = fetch_device(NULL, oldType & 0xFF);
	if (bdev == NULL) {
		bt_trace("acl_tx_nodev", nbuf, oldType, B_ERROR);
		if (flatCopy)
			free(data);
		nb_destroy(nbuf);
		return;
	}

	if (status == B_OK) {
		bdev->stat.successfulTX++;
		bdev->stat.bytesTX += actual_len;
	} else {
		bdev->stat.errorTX++;
	}

	if (flatCopy)
		free(data);
	bt_trace("acl_tx_free", nbuf, nbuf->size, status);
	nb_destroy(nbuf);

#ifdef BT_RESCHEDULING_AFTER_COMPLETITIONS
	schedTxProcessing(bdev);
#endif
}


#if 0
#pragma mark --- TX ---
#endif

status_t
submit_tx_command(bt_usb_dev* bdev, snet_buffer* snbuf)
{
	uint8 bRequestType = bdev->ctrl_req;
	uint8 bRequest = 0;
	uint16 wIndex = 0;
	uint16 value = 0;
	uint16 wLength = B_HOST_TO_LENDIAN_INT16(snb_size(snbuf));
	status_t error;

	if (!GET_BIT(bdev->state, RUNNING)) {
		return B_DEV_NOT_READY;
	}

	// set cookie
	snb_set_cookie(snbuf, bdev);

	TRACE("%s: @%p\n", __func__, snb_get(snbuf));

	error = usb->queue_request(bdev->dev, bRequestType, bRequest,
		value, wIndex, wLength,	snb_get(snbuf),
		command_complete, (void*) snbuf);

	if (error != B_OK) {
		bdev->stat.rejectedTX++;
	} else {
		bdev->stat.acceptedTX++;
	}

	return error;
}


status_t
submit_tx_acl(bt_usb_dev* bdev, net_buffer* nbuf)
{
	status_t error;

	// set cookie
	SET_DEVICE(nbuf, bdev->hdev);

	if (!GET_BIT(bdev->state, RUNNING)) {
		return B_DEV_NOT_READY;
	}

	// Try direct (zero-copy) access first.
	void* dataPtr;
	bool flatCopy = false;
	status_t daccess = nb->direct_access(nbuf, 0, nbuf->size, &dataPtr);
	if (daccess != B_OK) {
		// Buffer is non-contiguous (e.g. after NetBufferPrepend).
		// Create a flat copy for USB DMA.
		dataPtr = nb_get_whole_buffer(nbuf);
		if (dataPtr == NULL) {
			ERROR("%s: failed to flatten %" B_PRIu32 "-byte buffer\n",
				__func__, nbuf->size);
			return B_NO_MEMORY;
		}
		flatCopy = true;
	}

	// Sentinel: 0xBD01 = direct pointer, 0xBD02 = malloc'd flat copy
	nbuf->msg_flags = flatCopy ? 0xBD02 : 0xBD01;

	bt_trace("acl_tx_submit", nbuf, nbuf->size, B_OK);

	error = usb->queue_bulk(bdev->bulk_out_ep->handle, dataPtr,
		nbuf->size, acl_tx_complete, (void*)nbuf);

	if (error != B_OK) {
		bdev->stat.rejectedTX++;
		if (flatCopy)
			free(dataPtr);
	} else {
		bdev->stat.acceptedTX++;
	}

	return error;
}


status_t
submit_tx_sco(bt_usb_dev* bdev)
{
	if (!GET_BIT(bdev->state, RUNNING))
		return B_DEV_NOT_READY;

#ifdef BLUETOOTH_SUPPORTS_SCO
	if (bdev->iso_out_ep == NULL)
		return B_DEV_NOT_READY;

	// SCO TX will be implemented when SCO connections are established.
	// The HFP/HSP profile layer will call this with audio frames.
	return B_NOT_SUPPORTED;
#else
	return B_NOT_SUPPORTED;
#endif
}
