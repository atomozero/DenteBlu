/*
 * Copyright 2025 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Intel Bluetooth firmware loader for h2generic driver.
 *
 * Intel Bluetooth USB devices ship in "bootloader mode" (fw_variant 0x06)
 * and require firmware download before becoming operational HCI controllers
 * (fw_variant 0x23). This module implements the firmware loading sequence:
 *
 *   1. Read Intel Version (vendor cmd 0xFC05) — no HCI Reset first!
 *   2. If already operational, return success
 *   3. Read Boot Parameters (vendor cmd 0xFC0D)
 *   4. Build firmware filename from hw_variant + dev_revid
 *   5. Load .sfi firmware file from disk
 *   6. Send firmware commands extracted from the .sfi file
 *   7. Intel Reset (vendor cmd 0xFC01) to boot into firmware
 *   8. Verify device is now operational
 *
 * Firmware files must be placed in:
 *   /boot/system/non-packaged/data/firmware/intel/
 * or:
 *   /boot/system/data/firmware/intel/
 */


#include "btintel.h"

#include <fcntl.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <KernelExport.h>
#include <OS.h>

#include "h2debug.h"


// Timeouts
#define INTEL_CMD_TIMEOUT   2000000   // 2s for command TX
#define INTEL_EVT_TIMEOUT   5000000   // 5s for event RX
#define INTEL_BOOT_TIMEOUT  1500000   // 1.5s for firmware boot after reset


// Completion callback for synchronous USB operations
static void
intel_sync_callback(void* cookie, status_t status, void* data,
	size_t actual_len)
{
	intel_sync_ctx* ctx = (intel_sync_ctx*)cookie;
	ctx->status = status;
	ctx->actual_len = actual_len;
	release_sem(ctx->done);
}


// Send an HCI vendor command and wait for the command complete event.
// response/resp_size may be NULL/0 if no response payload is needed.
static status_t
btintel_send_cmd_sync(bt_usb_dev* bdev, uint16 opcode,
	const void* params, uint8 param_len,
	void* response, size_t resp_size, size_t* actual_resp)
{
	// Build HCI command packet: [opcode_lo][opcode_hi][param_len][params...]
	uint8 cmd[259]; // 3 header + max 256 params
	cmd[0] = opcode & 0xFF;
	cmd[1] = (opcode >> 8) & 0xFF;
	cmd[2] = param_len;
	if (param_len > 0 && params != NULL)
		memcpy(cmd + 3, params, param_len);

	// Prepare interrupt IN to receive command complete event
	intel_sync_ctx evt_ctx;
	evt_ctx.done = create_sem(0, "btintel_evt");
	if (evt_ctx.done < 0)
		return evt_ctx.done;
	evt_ctx.status = B_ERROR;
	evt_ctx.actual_len = 0;

	uint8 evt_buf[260];
	status_t err = usb->queue_interrupt(bdev->intr_in_ep->handle,
		evt_buf, sizeof(evt_buf), intel_sync_callback, &evt_ctx);
	if (err != B_OK) {
		delete_sem(evt_ctx.done);
		return err;
	}

	// Send command via USB control transfer
	intel_sync_ctx cmd_ctx;
	cmd_ctx.done = create_sem(0, "btintel_cmd");
	if (cmd_ctx.done < 0) {
		usb->cancel_queued_transfers(bdev->intr_in_ep->handle);
		delete_sem(evt_ctx.done);
		return cmd_ctx.done;
	}
	cmd_ctx.status = B_ERROR;
	cmd_ctx.actual_len = 0;

	err = usb->queue_request(bdev->dev, USB_TYPE_CLASS, 0, 0, 0,
		3 + param_len, cmd, intel_sync_callback, &cmd_ctx);
	if (err != B_OK) {
		usb->cancel_queued_transfers(bdev->intr_in_ep->handle);
		delete_sem(cmd_ctx.done);
		delete_sem(evt_ctx.done);
		return err;
	}

	// Wait for command TX completion
	err = acquire_sem_etc(cmd_ctx.done, 1, B_RELATIVE_TIMEOUT,
		INTEL_CMD_TIMEOUT);
	delete_sem(cmd_ctx.done);
	if (err != B_OK) {
		ERROR("btintel: command TX timeout for opcode 0x%04x\n", opcode);
		usb->cancel_queued_transfers(bdev->intr_in_ep->handle);
		delete_sem(evt_ctx.done);
		return err;
	}
	if (cmd_ctx.status != B_OK) {
		ERROR("btintel: command TX failed for opcode 0x%04x: %s\n",
			opcode, strerror(cmd_ctx.status));
		usb->cancel_queued_transfers(bdev->intr_in_ep->handle);
		delete_sem(evt_ctx.done);
		return cmd_ctx.status;
	}

	// Wait for event RX (command complete).
	// Intel controllers may send Vendor Specific Events (0xFF) during
	// firmware loading (bootloader status, validation results).  Skip
	// these and re-queue the interrupt transfer until we get the actual
	// Command Complete (0x0E) or a Command Status (0x0F), or timeout.
	int retries = 5;
retry_event:
	err = acquire_sem_etc(evt_ctx.done, 1, B_RELATIVE_TIMEOUT,
		INTEL_EVT_TIMEOUT);
	if (err != B_OK) {
		delete_sem(evt_ctx.done);
		ERROR("btintel: event RX timeout for opcode 0x%04x\n", opcode);
		usb->cancel_queued_transfers(bdev->intr_in_ep->handle);
		return err;
	}
	if (evt_ctx.status != B_OK) {
		delete_sem(evt_ctx.done);
		ERROR("btintel: event RX failed for opcode 0x%04x: %s\n",
			opcode, strerror(evt_ctx.status));
		return evt_ctx.status;
	}

	// If the event is not Command Complete, it may be a vendor event
	// or other asynchronous notification.  Log it and try again.
	if (evt_ctx.actual_len >= 1 && evt_buf[0] != 0x0E
		&& evt_buf[0] != 0x0F && retries-- > 0) {
		ERROR("btintel: skipping event 0x%02x (%" B_PRIuSIZE
			" bytes) while waiting for CC for opcode 0x%04x\n",
			evt_buf[0], evt_ctx.actual_len, opcode);
		for (size_t i = 0; i < evt_ctx.actual_len && i < 16; i++)
			ERROR("btintel:   evt[%" B_PRIuSIZE "]=0x%02x\n",
				i, evt_buf[i]);

		// Re-queue interrupt transfer for next event
		evt_ctx.status = B_ERROR;
		evt_ctx.actual_len = 0;
		err = usb->queue_interrupt(bdev->intr_in_ep->handle,
			evt_buf, sizeof(evt_buf), intel_sync_callback, &evt_ctx);
		if (err != B_OK) {
			delete_sem(evt_ctx.done);
			return err;
		}
		goto retry_event;
	}
	delete_sem(evt_ctx.done);

	// Parse HCI Command Complete event:
	//   [evt_code(1)][length(1)][num_cmd(1)][opcode(2)][response...]
	// We need at least 5 bytes for the header
	if (evt_ctx.actual_len < 5) {
		ERROR("btintel: event too short for opcode 0x%04x: "
			"%" B_PRIuSIZE " bytes (need >= 5)\n",
			opcode, evt_ctx.actual_len);
		// Dump what we got
		for (size_t i = 0; i < evt_ctx.actual_len && i < 16; i++)
			ERROR("btintel:   evt[%" B_PRIuSIZE "]=0x%02x\n",
				i, evt_buf[i]);
		return B_BAD_DATA;
	}

	// Verify event code is Command Complete (0x0E)
	if (evt_buf[0] != 0x0E) {
		ERROR("btintel: expected Command Complete (0x0E) for opcode "
			"0x%04x, got event 0x%02x\n", opcode, evt_buf[0]);
		return B_BAD_DATA;
	}

	// Verify opcode in CC matches what we sent
	uint16 cc_opcode = evt_buf[3] | (evt_buf[4] << 8);
	if (cc_opcode != opcode) {
		ERROR("btintel: CC opcode mismatch: sent 0x%04x, got 0x%04x\n",
			opcode, cc_opcode);
		return B_BAD_DATA;
	}

	if (response != NULL && resp_size > 0) {
		size_t payload = evt_ctx.actual_len - 5;
		// Only warn about short responses when the caller does NOT
		// use actual_resp (i.e. expects exactly resp_size bytes).
		// When actual_resp is provided, the caller handles variable-
		// length responses and doesn't need the warning.
		if (payload < resp_size && actual_resp == NULL) {
			ERROR("btintel: short response for opcode 0x%04x: "
				"%" B_PRIuSIZE " bytes (expected %" B_PRIuSIZE ")\n",
				opcode, payload, resp_size);
			// Dump raw event for diagnostics
			for (size_t i = 0; i < evt_ctx.actual_len && i < 20; i++)
				ERROR("btintel:   evt[%" B_PRIuSIZE "]=0x%02x\n",
					i, evt_buf[i]);
		}
		size_t copy = min_c(payload, resp_size);
		memcpy(response, evt_buf + 5, copy);
		if (actual_resp != NULL)
			*actual_resp = copy;
		// If payload is empty, this is likely "Unknown Command" with
		// just a status byte — return error
		if (payload == 0) {
			ERROR("btintel: empty response payload for opcode 0x%04x\n",
				opcode);
			return B_BAD_DATA;
		}
	} else {
		// No response buffer requested — still check the status byte
		// (first byte of payload) to detect command errors
		if (evt_ctx.actual_len >= 6 && evt_buf[5] != 0x00) {
			ERROR("btintel: command 0x%04x returned error status "
				"0x%02x\n", opcode, evt_buf[5]);
			return B_ERROR;
		}
		if (actual_resp != NULL)
			*actual_resp = 0;
	}

	return B_OK;
}


// Send an HCI command without waiting for a response event.
// Used for Intel Reset (0xFC01), where the chip reboots immediately.
static status_t
btintel_send_cmd_nosync(bt_usb_dev* bdev, uint16 opcode,
	const void* params, uint8 param_len)
{
	uint8 cmd[259];
	cmd[0] = opcode & 0xFF;
	cmd[1] = (opcode >> 8) & 0xFF;
	cmd[2] = param_len;
	if (param_len > 0 && params != NULL)
		memcpy(cmd + 3, params, param_len);

	intel_sync_ctx cmd_ctx;
	cmd_ctx.done = create_sem(0, "btintel_cmd");
	if (cmd_ctx.done < 0)
		return cmd_ctx.done;
	cmd_ctx.status = B_ERROR;
	cmd_ctx.actual_len = 0;

	status_t err = usb->queue_request(bdev->dev, USB_TYPE_CLASS, 0, 0, 0,
		3 + param_len, cmd, intel_sync_callback, &cmd_ctx);
	if (err != B_OK) {
		delete_sem(cmd_ctx.done);
		return err;
	}

	err = acquire_sem_etc(cmd_ctx.done, 1, B_RELATIVE_TIMEOUT,
		INTEL_CMD_TIMEOUT);
	delete_sem(cmd_ctx.done);

	if (err != B_OK)
		return err;
	return cmd_ctx.status;
}


// Drain any pending events from the interrupt endpoint.
// After Intel Reset, the chip may send a Boot Complete vendor event
// (and possibly others) that would confuse the next btintel_send_cmd_sync.
static void
btintel_drain_events(bt_usb_dev* bdev)
{
	// Drain up to 5 events (Boot Complete + any queued notifications)
	for (int attempt = 0; attempt < 5; attempt++) {
		intel_sync_ctx ctx;
		ctx.done = create_sem(0, "btintel_drain");
		if (ctx.done < 0)
			return;
		ctx.status = B_ERROR;
		ctx.actual_len = 0;

		uint8 buf[260];
		status_t err = usb->queue_interrupt(bdev->intr_in_ep->handle,
			buf, sizeof(buf), intel_sync_callback, &ctx);
		if (err != B_OK) {
			delete_sem(ctx.done);
			return;
		}

		// Short timeout — just drain whatever is pending
		err = acquire_sem_etc(ctx.done, 1, B_RELATIVE_TIMEOUT, 500000);
		if (err != B_OK) {
			// Timed out — nothing more pending, cancel and stop
			usb->cancel_queued_transfers(bdev->intr_in_ep->handle);
			delete_sem(ctx.done);
			return;
		}
		delete_sem(ctx.done);
		// Got an event — loop to check for more
	}
}


static status_t
btintel_read_boot_params(bt_usb_dev* bdev, intel_boot_params* params)
{
	return btintel_send_cmd_sync(bdev, INTEL_HCI_READ_BOOT_PARAMS,
		NULL, 0, params, sizeof(*params), NULL);
}


// Parse a TLV-format version response into intel_version_tlv struct.
// The data pointer should point to the command complete payload (after the
// 5-byte HCI CC header), starting with the status byte.
static status_t
btintel_parse_version_tlv(const uint8* data, size_t len,
	intel_version_tlv* ver)
{
	if (len < 1) {
		ERROR("btintel: TLV response empty\n");
		return B_BAD_DATA;
	}

	// First byte is status
	if (data[0] != 0x00) {
		ERROR("btintel: TLV version command returned status 0x%02x\n",
			data[0]);
		return B_ERROR;
	}

	// Skip status byte
	data++;
	len--;

	while (len > 0) {
		// Need at least 2 bytes for TLV header (type + len)
		if (len < 2) {
			ERROR("btintel: TLV truncated (%" B_PRIuSIZE
				" bytes remaining)\n", len);
			return B_BAD_DATA;
		}

		uint8 type = data[0];
		uint8 vlen = data[1];

		if (len < (size_t)(2 + vlen)) {
			ERROR("btintel: TLV entry type=0x%02x len=%d exceeds "
				"remaining data (%" B_PRIuSIZE ")\n",
				type, vlen, len);
			return B_BAD_DATA;
		}

		const uint8* val = data + 2;

		switch (type) {
			case INTEL_TLV_CNVI_TOP:
				if (vlen >= 4)
					memcpy(&ver->cnvi_top, val, 4);
				break;
			case INTEL_TLV_CNVR_TOP:
				if (vlen >= 4)
					memcpy(&ver->cnvr_top, val, 4);
				break;
			case INTEL_TLV_CNVI_BT:
				if (vlen >= 4)
					memcpy(&ver->cnvi_bt, val, 4);
				break;
			case INTEL_TLV_CNVR_BT:
				if (vlen >= 4)
					memcpy(&ver->cnvr_bt, val, 4);
				break;
			case INTEL_TLV_DEV_REV_ID:
				if (vlen >= 2)
					memcpy(&ver->dev_rev_id, val, 2);
				break;
			case INTEL_TLV_IMAGE_TYPE:
				if (vlen >= 1)
					ver->img_type = val[0];
				break;
			case INTEL_TLV_TIME_STAMP:
				if (vlen >= 2) {
					ver->min_fw_build_cw = val[0];
					ver->min_fw_build_yy = val[1];
					memcpy(&ver->timestamp, val, 2);
				}
				break;
			case INTEL_TLV_BUILD_TYPE:
				if (vlen >= 1)
					ver->build_type = val[0];
				break;
			case INTEL_TLV_BUILD_NUM:
				if (vlen >= 4) {
					ver->min_fw_build_nn = val[0];
					memcpy(&ver->build_num, val, 4);
				}
				break;
			case INTEL_TLV_SECURE_BOOT:
				if (vlen >= 1)
					ver->secure_boot = val[0];
				break;
			case INTEL_TLV_OTP_LOCK:
				if (vlen >= 1)
					ver->otp_lock = val[0];
				break;
			case INTEL_TLV_API_LOCK:
				if (vlen >= 1)
					ver->api_lock = val[0];
				break;
			case INTEL_TLV_DEBUG_LOCK:
				if (vlen >= 1)
					ver->debug_lock = val[0];
				break;
			case INTEL_TLV_MIN_FW:
				if (vlen >= 3) {
					ver->min_fw_build_nn = val[0];
					ver->min_fw_build_cw = val[1];
					ver->min_fw_build_yy = val[2];
				}
				break;
			case INTEL_TLV_LIMITED_CCE:
				if (vlen >= 1)
					ver->limited_cce = val[0];
				break;
			case INTEL_TLV_SBE_TYPE:
				if (vlen >= 1)
					ver->sbe_type = val[0];
				break;
			case INTEL_TLV_OTP_BDADDR:
				if (vlen >= 6)
					memcpy(ver->otp_bd_addr, val, 6);
				break;
			case INTEL_TLV_GIT_SHA1:
				if (vlen >= 4)
					memcpy(&ver->git_sha1, val, 4);
				break;
			case INTEL_TLV_FW_ID:
			{
				size_t copy = min_c((size_t)vlen,
					sizeof(ver->fw_id) - 1);
				memcpy(ver->fw_id, val, copy);
				ver->fw_id[copy] = '\0';
				break;
			}
			default:
				// Unknown TLV type — skip silently
				break;
		}

		data += 2 + vlen;
		len -= 2 + vlen;
	}

	return B_OK;
}


// Load a firmware file from disk. Caller must free(*data) on success.
static status_t
btintel_load_firmware_file(const char* filename, uint8** data, size_t* size)
{
	char path[B_PATH_NAME_LENGTH];
	int fd = -1;

	const char* dirs[] = {
		"/boot/system/non-packaged/data/firmware/intel",
		"/boot/system/data/firmware/intel"
	};

	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]) && fd < 0; i++) {
		snprintf(path, sizeof(path), "%s/%s", dirs[i], filename);
		fd = open(path, O_RDONLY);
	}

	if (fd < 0) {
		ERROR("btintel: firmware file '%s' not found\n", filename);
		return B_ENTRY_NOT_FOUND;
	}

	off_t file_size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);

	if (file_size <= 0 || file_size > 1024 * 1024) {
		ERROR("btintel: firmware file size invalid: %" B_PRIdOFF "\n",
			file_size);
		close(fd);
		return B_BAD_DATA;
	}

	*data = (uint8*)malloc(file_size);
	if (*data == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}

	ssize_t n = read(fd, *data, file_size);
	close(fd);

	if (n != file_size) {
		free(*data);
		*data = NULL;
		return B_IO_ERROR;
	}

	*size = file_size;
	ERROR("btintel: loaded firmware '%s' (%" B_PRIdOFF " bytes)\n",
		filename, file_size);
	return B_OK;
}


// Send data via Intel Secure Send (0xFC09).
// Splits data into 252-byte chunks, each prefixed with fragment_type byte.
// fragment_type: 0x00=RSA signature, 0x01=firmware data, 0x03=ECDSA signature
//
// Intel controllers do NOT respond to Secure Send with a standard Command
// Complete (0x0E) event.  Instead, they send Vendor Specific Events (0xFF)
// with sub-event 0x06 (Secure Send Result).  Therefore we use fire-and-forget
// mode: send each chunk via USB control transfer, wait only for USB TX
// completion, and do not wait for HCI events.  Any pending vendor events are
// drained after the entire firmware download + Intel Reset sequence.
static status_t
btintel_secure_send(bt_usb_dev* bdev, uint8 fragment_type,
	const uint8* data, size_t len)
{
	while (len > 0) {
		uint8 cmd_param[253]; // 1 byte type + max 252 bytes data
		size_t chunk = min_c(len, (size_t)252);

		cmd_param[0] = fragment_type;
		memcpy(cmd_param + 1, data, chunk);

		status_t err = btintel_send_cmd_nosync(bdev,
			INTEL_HCI_SECURE_SEND, cmd_param,
			(uint8)(chunk + 1));
		if (err != B_OK) {
			ERROR("btintel: Secure Send (type=0x%02x) TX failed "
				"at offset %" B_PRIuSIZE ": %s\n",
				fragment_type, len, strerror(err));
			return err;
		}

		data += chunk;
		len -= chunk;
	}
	return B_OK;
}


// Download firmware to the Intel controller.
// The .sfi file layout:
//   [8 bytes CSS preamble]
//   [644 bytes RSA signature header]
//   [optional 320 bytes ECDSA signature header, if CSS version 2]
//   [HCI command sequence — firmware payload]
//
// The signature headers are sent via Secure Send (0xFC09) for controller
// firmware authentication. The firmware payload commands are accumulated
// up to 0xFC09 boundaries and sent as Secure Send data fragments.
static status_t
btintel_download_firmware(bt_usb_dev* bdev, const uint8* fw_data,
	size_t fw_size)
{
	size_t offset = INTEL_CSS_HEADER_OFFSET;

	if (fw_size < offset + 4) {
		ERROR("btintel: firmware too small for CSS header\n");
		return B_BAD_DATA;
	}

	// Read CSS version to determine header sizes
	uint32 css_ver;
	memcpy(&css_ver, fw_data + offset, sizeof(css_ver));

	// Send RSA signature header via Secure Send (type=0x00)
	if (fw_size < offset + INTEL_CSS_RSA_HEADER_SIZE) {
		ERROR("btintel: firmware too small for RSA header\n");
		return B_BAD_DATA;
	}
	ERROR("btintel: sending RSA signature header (%d bytes)\n",
		INTEL_CSS_RSA_HEADER_SIZE);
	status_t err = btintel_secure_send(bdev, 0x00,
		fw_data + offset, INTEL_CSS_RSA_HEADER_SIZE);
	if (err != B_OK) {
		ERROR("btintel: RSA header send failed: %s\n", strerror(err));
		return err;
	}
	offset += INTEL_CSS_RSA_HEADER_SIZE;

	// CSS version 2: also send ECDSA signature header (type=0x03)
	if (css_ver == 0x00020000) {
		if (fw_size < offset + INTEL_CSS_ECDSA_HEADER_SIZE) {
			ERROR("btintel: firmware too small for ECDSA header\n");
			return B_BAD_DATA;
		}
		ERROR("btintel: sending ECDSA signature header (%d bytes)\n",
			INTEL_CSS_ECDSA_HEADER_SIZE);
		err = btintel_secure_send(bdev, 0x03,
			fw_data + offset, INTEL_CSS_ECDSA_HEADER_SIZE);
		if (err != B_OK) {
			ERROR("btintel: ECDSA header send failed: %s\n",
				strerror(err));
			return err;
		}
		offset += INTEL_CSS_ECDSA_HEADER_SIZE;
	}

	if (offset >= fw_size) {
		ERROR("btintel: firmware has no payload after headers "
			"(offset %" B_PRIuSIZE " >= size %" B_PRIuSIZE ")\n",
			offset, fw_size);
		return B_BAD_DATA;
	}

	ERROR("btintel: downloading firmware payload (%" B_PRIuSIZE
		" bytes)\n", fw_size - offset);

	// The firmware payload is a sequence of HCI commands:
	//   [opcode_lo][opcode_hi][param_len][params...]
	// Commands are accumulated into fragments delimited by Secure Send
	// commands (opcode 0xFC09). Each accumulated fragment is sent to the
	// controller via Secure Send with type=0x01 (firmware data).
	const uint8* frag_start = fw_data + offset;
	size_t frag_len = 0;
	uint32 frag_count = 0;

	while (offset + 3 <= fw_size) {
		uint16 cmd_opcode = fw_data[offset] | (fw_data[offset + 1] << 8);
		uint8 cmd_len = fw_data[offset + 2];

		if (offset + 3 + cmd_len > fw_size) {
			ERROR("btintel: firmware truncated at offset "
				"%" B_PRIuSIZE "\n", offset);
			return B_BAD_DATA;
		}

		frag_len += 3 + cmd_len;
		offset += 3 + cmd_len;

		// When we hit a Secure Send command (0xFC09) in the file,
		// send the accumulated fragment as firmware data
		if (cmd_opcode == INTEL_HCI_SECURE_SEND) {
			err = btintel_secure_send(bdev, 0x01,
				frag_start, frag_len);
			if (err != B_OK) {
				ERROR("btintel: firmware fragment #%" B_PRIu32
					" failed: %s\n",
					frag_count, strerror(err));
				return err;
			}
			frag_start = fw_data + offset;
			frag_len = 0;
			frag_count++;

			if (frag_count % 50 == 0)
				ERROR("btintel: sent %" B_PRIu32
					" firmware fragments...\n", frag_count);
		}
	}

	// Send any remaining data
	if (frag_len > 0) {
		err = btintel_secure_send(bdev, 0x01, frag_start, frag_len);
		if (err != B_OK) {
			ERROR("btintel: final firmware fragment failed: %s\n",
				strerror(err));
			return err;
		}
		frag_count++;
	}

	ERROR("btintel: firmware download complete (%" B_PRIu32
		" fragments), draining pending events\n", frag_count);

	// Drain vendor events (Secure Send Results) that accumulated during
	// the fire-and-forget download.  The chip sends 0xFF sub-event 0x06
	// for each stage (RSA header, ECDSA header, firmware fragments).
	btintel_drain_events(bdev);

	return B_OK;
}


// Load DDC (Device Default Configuration) file and send entries via
// HCI command 0xFC8B. Each DDC entry is:
//   [length(1)] [ddc_id(2, LE)] [value(length-2 bytes)]
// The entire entry (including length byte) is sent as the HCI parameter.
// Non-critical — failures are logged but do not stop device setup.
static status_t
btintel_load_ddc(bt_usb_dev* bdev, const char* ddc_name)
{
	uint8* ddc_data = NULL;
	size_t ddc_size = 0;
	status_t err = btintel_load_firmware_file(ddc_name, &ddc_data, &ddc_size);
	if (err != B_OK) {
		ERROR("btintel: DDC file '%s' not found (non-critical)\n",
			ddc_name);
		return err;
	}

	ERROR("btintel: applying DDC config '%s' (%" B_PRIuSIZE " bytes)\n",
		ddc_name, ddc_size);

	size_t offset = 0;
	uint32 entry_count = 0;

	while (offset < ddc_size) {
		if (offset + 1 > ddc_size)
			break;

		// length byte = number of bytes following (ddc_id + value)
		uint8 entry_len = ddc_data[offset];
		// Total entry size = 1 (length byte) + entry_len
		// Use size_t to avoid uint8 overflow (255+1=0 → infinite loop)
		size_t cmd_plen = (size_t)entry_len + 1;

		if (offset + cmd_plen > ddc_size) {
			ERROR("btintel: DDC entry #%" B_PRIu32
				" truncated at offset %" B_PRIuSIZE "\n",
				entry_count, offset);
			break;
		}

		if (entry_len < 2) {
			ERROR("btintel: DDC entry #%" B_PRIu32
				" too short (len=%d, need >= 2)\n",
				entry_count, entry_len);
			break;
		}

		err = btintel_send_cmd_sync(bdev, INTEL_HCI_WRITE_DDC,
			ddc_data + offset, (uint8)cmd_plen, NULL, 0, NULL);
		if (err != B_OK) {
			ERROR("btintel: DDC entry #%" B_PRIu32
				" failed: %s (continuing)\n",
				entry_count, strerror(err));
		}

		offset += cmd_plen;
		entry_count++;
	}

	free(ddc_data);
	ERROR("btintel: DDC config applied (%" B_PRIu32 " entries)\n",
		entry_count);
	return B_OK;
}


// Build TLV firmware/DDC filename into buf.
// Naming depends on chip generation and boot stage:
//   Blazar (hw_variant >= 0x1e) bootloader: ibt-<cnvi>-<cnvr>-iml.<suffix>
//   Blazar IML/operational with fw_id:      ibt-<cnvi>-<cnvr>-<fw_id>.<suffix>
//   Standard:                               ibt-<cnvi>-<cnvr>.<suffix>
static void
btintel_get_fw_name_tlv(const intel_version_tlv* ver, char* buf,
	size_t buf_size, const char* suffix)
{
	uint16 cnvi = INTEL_CNVX_TOP_PACK_SWAB(
		INTEL_CNVX_TOP_TYPE(ver->cnvi_top),
		INTEL_CNVX_TOP_STEP(ver->cnvi_top));
	uint16 cnvr = INTEL_CNVX_TOP_PACK_SWAB(
		INTEL_CNVX_TOP_TYPE(ver->cnvr_top),
		INTEL_CNVX_TOP_STEP(ver->cnvr_top));

	// Blazar generation chips use two-stage boot (IML intermediate loader)
	if (BTINTEL_HW_VARIANT(ver->cnvi_bt) >= 0x1e) {
		// Bootloader stage: load IML firmware
		if (ver->img_type == BTINTEL_IMG_BOOTLOADER) {
			snprintf(buf, buf_size, "ibt-%04x-%04x-iml.%s",
				cnvi, cnvr, suffix);
			return;
		}
		// IML/operational with firmware ID: include fw_id in name
		if (ver->fw_id[0] != '\0') {
			snprintf(buf, buf_size, "ibt-%04x-%04x-%s.%s",
				cnvi, cnvr, ver->fw_id, suffix);
			return;
		}
	}

	// Standard naming for non-Blazar or fallback
	snprintf(buf, buf_size, "ibt-%04x-%04x.%s", cnvi, cnvr, suffix);
}


// Helper: load firmware file, download via Secure Send, Intel Reset.
// After the reset the device will disconnect from USB and reconnect
// with operational firmware loaded.  We return B_SHUTTING_DOWN to
// signal the caller that the device is rebooting and will re-appear.
static status_t
btintel_download_and_reset(bt_usb_dev* bdev, const char* fw_name)
{
	// Load firmware file
	uint8* fw_data = NULL;
	size_t fw_size = 0;
	status_t err = btintel_load_firmware_file(fw_name, &fw_data, &fw_size);
	if (err != B_OK) {
		ERROR("btintel: firmware '%s' not found — device stays "
			"in current mode\n", fw_name);
		return err;
	}

	// Download firmware via Secure Send
	err = btintel_download_firmware(bdev, fw_data, fw_size);
	free(fw_data);
	if (err != B_OK) {
		ERROR("btintel: firmware download failed: %s\n",
			strerror(err));
		return err;
	}

	// Intel Reset to boot into the downloaded firmware.
	// After this the chip reboots its USB interface — the current
	// USB handle becomes invalid.  Do NOT try to communicate with
	// the device after sending this command.
	uint8 reset_params[] = {
		0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
	};
	btintel_send_cmd_nosync(bdev, INTEL_HCI_RESET,
		reset_params, sizeof(reset_params));

	ERROR("btintel: firmware sent, device rebooting into "
		"operational mode\n");

	// Return a distinct status to tell the caller the device is
	// rebooting — this is NOT an error, just a signal to stop
	// using the current USB handle.
	return B_SHUTTING_DOWN;
}


// Log TLV version info for diagnostics.
static void
btintel_log_version_tlv(const intel_version_tlv* ver, const char* label)
{
	ERROR("btintel: %s: img_type=0x%02x cnvi_top=0x%08" B_PRIx32
		" cnvr_top=0x%08" B_PRIx32 " cnvi_bt=0x%08" B_PRIx32
		" dev_rev_id=0x%04x\n",
		label, ver->img_type, ver->cnvi_top, ver->cnvr_top,
		ver->cnvi_bt, ver->dev_rev_id);
	ERROR("btintel: %s: hw_variant=0x%02x secure_boot=%d otp_lock=%d "
		"api_lock=%d debug_lock=%d limited_cce=%d\n",
		label, BTINTEL_HW_VARIANT(ver->cnvi_bt),
		ver->secure_boot, ver->otp_lock, ver->api_lock,
		ver->debug_lock, ver->limited_cce);
	if (ver->fw_id[0] != '\0')
		ERROR("btintel: %s: fw_id=%s\n", label, ver->fw_id);
}


// TLV device firmware setup — called for newer Intel chips that
// respond with TLV-format version data.
//
// Boot sequence depends on chip generation:
//   Non-Blazar (hw_variant < 0x1e):
//     Bootloader → operational firmware → Operational
//   Blazar (hw_variant >= 0x1e):
//     Bootloader → IML firmware → IML → operational firmware → Operational
static status_t
btintel_setup_tlv(bt_usb_dev* bdev, intel_version_tlv* ver)
{
	btintel_log_version_tlv(ver, "TLV setup");

	// Already operational — no firmware download needed
	if (ver->img_type == BTINTEL_IMG_OP) {
		ERROR("btintel: TLV device already operational\n");
		goto load_ddc;
	}

	// Bootloader mode — download firmware (stage 1)
	if (ver->img_type == BTINTEL_IMG_BOOTLOADER) {
		char fw_name[128];
		btintel_get_fw_name_tlv(ver, fw_name, sizeof(fw_name), "sfi");
		ERROR("btintel: TLV bootloader, loading firmware: %s\n",
			fw_name);

		status_t err = btintel_download_and_reset(bdev, fw_name);
		// B_SHUTTING_DOWN = firmware sent, device rebooting via USB.
		// The device will reconnect and device_added will be called
		// again.  At that point img_type should be operational.
		return err;
	}

	// IML mode — download operational firmware (stage 2, Blazar chips)
	if (ver->img_type == BTINTEL_IMG_IML) {
		char fw_name[128];
		btintel_get_fw_name_tlv(ver, fw_name, sizeof(fw_name), "sfi");
		ERROR("btintel: TLV IML stage, loading operational firmware: "
			"%s\n", fw_name);

		status_t err = btintel_download_and_reset(bdev, fw_name);
		return err;
	}

	// Unknown state
	ERROR("btintel: TLV unknown img_type 0x%02x\n", ver->img_type);
	return B_NOT_SUPPORTED;

load_ddc:
	{
		char ddc_name[128];
		btintel_get_fw_name_tlv(ver, ddc_name, sizeof(ddc_name), "ddc");
		btintel_load_ddc(bdev, ddc_name);
	}
	return B_OK;
}


status_t
btintel_setup(bt_usb_dev* bdev)
{
	ERROR("btintel: starting Intel firmware setup\n");

	// NOTE: Do NOT send HCI Reset here! The Intel bootloader does not
	// understand standard HCI commands and returns "Unknown Command".
	// Linux btusb also reads version first without resetting.

	// 1. Read Intel version — send with parameter 0xFF.
	// Legacy chips ignore the extra parameter, but TLV-generation chips
	// (AX201/AX211, Alder Lake+) require it and reject the command without.
	uint8 ver_param = 0xFF;
	uint8 raw_ver[256];
	size_t ver_actual = 0;
	status_t err = btintel_send_cmd_sync(bdev, INTEL_HCI_READ_VERSION,
		&ver_param, 1, raw_ver, sizeof(raw_ver), &ver_actual);
	if (err != B_OK) {
		ERROR("btintel: failed to read version: %s\n", strerror(err));
		return err;
	}

	ERROR("btintel: version response: %" B_PRIuSIZE " bytes\n", ver_actual);

	// Detect legacy vs TLV format:
	// Legacy: exactly sizeof(intel_version) (10 bytes) with
	//         hw_platform (byte[1]) == 0x37
	// TLV: anything else (typically much longer) — parse as TLV entries
	if (ver_actual == sizeof(intel_version) && raw_ver[1] == 0x37) {
		// Legacy format — copy into intel_version struct
		intel_version ver;
		memcpy(&ver, raw_ver, sizeof(ver));

		ERROR("btintel: legacy version: status=0x%02x "
			"hw_platform=0x%02x hw_variant=0x%02x "
			"fw_variant=0x%02x fw_revision=%d fw_build=%d.%d\n",
			ver.status, ver.hw_platform, ver.hw_variant,
			ver.fw_variant, ver.fw_revision,
			ver.fw_build_ww, ver.fw_build_yy);

		if (ver.status != 0x00) {
			ERROR("btintel: version command returned error "
				"status 0x%02x\n", ver.status);
			return B_ERROR;
		}

		// Check if firmware already loaded
		if (ver.fw_variant == 0x23) {
			ERROR("btintel: firmware already loaded, "
				"device operational\n");
			return B_OK;
		}

		if (ver.fw_variant != 0x06) {
			ERROR("btintel: unexpected fw_variant 0x%02x "
				"(expected 0x06 bootloader)\n", ver.fw_variant);
			return B_NOT_SUPPORTED;
		}

		// Legacy bootloader path — read boot params, load .sfi
		intel_boot_params bp;
		memset(&bp, 0, sizeof(bp));
		err = btintel_read_boot_params(bdev, &bp);
		if (err != B_OK) {
			ERROR("btintel: failed to read boot params: %s\n",
				strerror(err));
			return err;
		}

		ERROR("btintel: secure_boot=%d otp_lock=%d api_lock=%d "
			"dev_revid=0x%04x hw_revision=%d fw_revision=%d\n",
			bp.secure_boot, bp.otp_lock, bp.api_lock, bp.dev_revid,
			ver.hw_revision, ver.fw_revision);

		// Firmware filename depends on hw_variant:
		//   0x0b, 0x0c (SfP/WsP): ibt-<hw_variant>-<dev_revid>.sfi
		//   0x11+ (JfP/ThP/HrP/CcP): ibt-<hw_variant>-<hw_revision>-<fw_revision>.sfi
		char fw_name[64];
		uint8* fw_data = NULL;
		size_t fw_size = 0;

		if (ver.hw_variant >= 0x11) {
			// JfP/ThP/HrP/CcP: 3-component naming
			snprintf(fw_name, sizeof(fw_name), "ibt-%d-%d-%d.sfi",
				ver.hw_variant, ver.hw_revision, ver.fw_revision);
			ERROR("btintel: looking for firmware: %s\n", fw_name);
			err = btintel_load_firmware_file(fw_name, &fw_data,
				&fw_size);
		} else {
			// SfP/WsP: 2-component naming
			snprintf(fw_name, sizeof(fw_name), "ibt-%d-%d.sfi",
				ver.hw_variant, bp.dev_revid);
			ERROR("btintel: looking for firmware: %s\n", fw_name);
			err = btintel_load_firmware_file(fw_name, &fw_data,
				&fw_size);
		}

		// Fallback: try 2-component name with dev_revid
		if (err != B_OK) {
			snprintf(fw_name, sizeof(fw_name), "ibt-%d-%d.sfi",
				ver.hw_variant, bp.dev_revid);
			ERROR("btintel: trying fallback: %s\n", fw_name);
			err = btintel_load_firmware_file(fw_name, &fw_data,
				&fw_size);
		}

		// Fallback: try 2-component name with hw_revision
		if (err != B_OK) {
			snprintf(fw_name, sizeof(fw_name), "ibt-%d-%d.sfi",
				ver.hw_variant, ver.hw_revision);
			ERROR("btintel: trying fallback: %s\n", fw_name);
			err = btintel_load_firmware_file(fw_name, &fw_data,
				&fw_size);
		}

		if (err != B_OK) {
			ERROR("btintel: no firmware file found — "
				"device stays in bootloader mode\n");
			return err;
		}

		err = btintel_download_firmware(bdev, fw_data, fw_size);
		free(fw_data);

		if (err != B_OK) {
			ERROR("btintel: firmware download failed: %s\n",
				strerror(err));
			return err;
		}

		// Intel Reset — device will reboot its USB interface.
		// Do NOT read version afterwards; the USB handle is dead.
		uint8 reset_params[] = {
			0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00
		};
		btintel_send_cmd_nosync(bdev, INTEL_HCI_RESET,
			reset_params, sizeof(reset_params));

		ERROR("btintel: firmware sent, device rebooting into "
			"operational mode\n");
		return B_SHUTTING_DOWN;
	}

	// TLV format — parse TLV entries
	ERROR("btintel: detected TLV version format "
		"(%" B_PRIuSIZE " bytes)\n", ver_actual);

	intel_version_tlv ver_tlv;
	memset(&ver_tlv, 0, sizeof(ver_tlv));
	err = btintel_parse_version_tlv(raw_ver, ver_actual, &ver_tlv);
	if (err != B_OK) {
		ERROR("btintel: failed to parse TLV version: %s\n",
			strerror(err));
		return err;
	}

	// Validate hw_platform is 0x37 (Intel Bluetooth)
	if (BTINTEL_HW_PLATFORM(ver_tlv.cnvi_bt) != 0x37) {
		ERROR("btintel: TLV unexpected hw_platform 0x%02x "
			"(expected 0x37)\n",
			BTINTEL_HW_PLATFORM(ver_tlv.cnvi_bt));
		return B_BAD_DATA;
	}

	return btintel_setup_tlv(bdev, &ver_tlv);
}
