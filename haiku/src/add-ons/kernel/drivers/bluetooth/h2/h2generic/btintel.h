/*
 * Copyright 2025 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Intel Bluetooth firmware loader for h2generic driver.
 */
#ifndef _BTINTEL_H_
#define _BTINTEL_H_


#include "h2generic.h"


// Intel HCI vendor command opcodes
#define INTEL_HCI_RESET            0xfc01
#define INTEL_HCI_READ_VERSION     0xfc05
#define INTEL_HCI_SECURE_SEND      0xfc09
#define INTEL_HCI_READ_BOOT_PARAMS 0xfc0d
#define INTEL_HCI_SET_EVENT_MASK   0xfc52
#define INTEL_HCI_WRITE_DDC        0xfc8b

// Intel vendor USB IDs
#define USB_VENDOR_INTEL           0x8087

// CSS header layout
#define INTEL_CSS_HEADER_OFFSET     8
#define INTEL_CSS_RSA_HEADER_SIZE   644
#define INTEL_CSS_ECDSA_HEADER_SIZE 320

// Maximum firmware ID string length
#define BTINTEL_FWID_MAXLEN        64

// Image type constants (TLV img_type field)
#define BTINTEL_IMG_BOOTLOADER     0x01
#define BTINTEL_IMG_IML            0x02
#define BTINTEL_IMG_OP             0x03

// Macros for extracting CNVx_TOP fields and building firmware filenames
#define INTEL_CNVX_TOP_TYPE(cnvx_top)    ((cnvx_top) & 0x00000fff)
#define INTEL_CNVX_TOP_STEP(cnvx_top)    (((cnvx_top) & 0x0f000000) >> 24)

// Pack type (12-bit) and step (4-bit) into a 16-bit value, byte-swapped.
// Result is used as hex component of firmware filename.
#define INTEL_CNVX_TOP_PACK_SWAB(t, s) \
	((uint16)(((((uint16)(((t) << 4) | (s))) & 0xFF) << 8) | \
	          ((((uint16)(((t) << 4) | (s))) >> 8) & 0xFF)))

// Macros for extracting hw_platform/hw_variant from cnvi_bt
#define BTINTEL_HW_PLATFORM(cnvx_bt)  ((uint8)(((cnvx_bt) & 0x0000ff00) >> 8))
#define BTINTEL_HW_VARIANT(cnvx_bt)   ((uint8)(((cnvx_bt) & 0x003f0000) >> 16))

// TLV type constants for Intel Read Version TLV response
enum {
	INTEL_TLV_CNVI_TOP = 0x10,
	INTEL_TLV_CNVR_TOP,          // 0x11
	INTEL_TLV_CNVI_BT,           // 0x12
	INTEL_TLV_CNVR_BT,           // 0x13
	INTEL_TLV_CNVI_OTP,          // 0x14
	INTEL_TLV_CNVR_OTP,          // 0x15
	INTEL_TLV_DEV_REV_ID,        // 0x16
	INTEL_TLV_USB_VENDOR_ID,     // 0x17
	INTEL_TLV_USB_PRODUCT_ID,    // 0x18
	INTEL_TLV_PCIE_VENDOR_ID,    // 0x19
	INTEL_TLV_PCIE_DEVICE_ID,    // 0x1a
	INTEL_TLV_PCIE_SUBSYSTEM_ID, // 0x1b
	INTEL_TLV_IMAGE_TYPE,        // 0x1c
	INTEL_TLV_TIME_STAMP,        // 0x1d
	INTEL_TLV_BUILD_TYPE,        // 0x1e
	INTEL_TLV_BUILD_NUM,         // 0x1f
	INTEL_TLV_FW_BUILD_PRODUCT,  // 0x20
	INTEL_TLV_FW_BUILD_HW,       // 0x21
	INTEL_TLV_FW_STEP,           // 0x22
	INTEL_TLV_BT_SPEC,           // 0x23
	INTEL_TLV_MFG_NAME,          // 0x24
	INTEL_TLV_HCI_REV,           // 0x25
	INTEL_TLV_LMP_SUBVER,        // 0x26
	INTEL_TLV_OTP_PATCH_VER,     // 0x27
	INTEL_TLV_SECURE_BOOT,       // 0x28
	INTEL_TLV_KEY_FROM_HDR,      // 0x29
	INTEL_TLV_OTP_LOCK,          // 0x2a
	INTEL_TLV_API_LOCK,          // 0x2b
	INTEL_TLV_DEBUG_LOCK,        // 0x2c
	INTEL_TLV_MIN_FW,            // 0x2d
	INTEL_TLV_LIMITED_CCE,       // 0x2e
	INTEL_TLV_SBE_TYPE,          // 0x2f
	INTEL_TLV_OTP_BDADDR,        // 0x30
	INTEL_TLV_UNLOCKED_STATE,    // 0x31
	INTEL_TLV_GIT_SHA1,          // 0x32
	INTEL_TLV_FW_ID = 0x50
};

// TLV wire format element
struct intel_tlv {
	uint8 type;
	uint8 len;
	uint8 val[0];
} __attribute__((packed));

// Intel version response (legacy format, 10 bytes)
struct intel_version {
	uint8  status;
	uint8  hw_platform;
	uint8  hw_variant;
	uint8  hw_revision;
	uint8  fw_variant;
	uint8  fw_revision;
	uint8  fw_build_num;
	uint8  fw_build_ww;
	uint8  fw_build_yy;
	uint8  fw_patch_num;
} __attribute__((packed));

// Intel version response (TLV format, parsed from TLV entries)
struct intel_version_tlv {
	uint32 cnvi_top;
	uint32 cnvr_top;
	uint32 cnvi_bt;
	uint32 cnvr_bt;
	uint16 dev_rev_id;
	uint8  img_type;
	uint16 timestamp;
	uint8  build_type;
	uint32 build_num;
	uint8  secure_boot;
	uint8  otp_lock;
	uint8  api_lock;
	uint8  debug_lock;
	uint8  min_fw_build_nn;
	uint8  min_fw_build_cw;
	uint8  min_fw_build_yy;
	uint8  limited_cce;
	uint8  sbe_type;
	uint32 git_sha1;
	char   fw_id[BTINTEL_FWID_MAXLEN];
	uint8  otp_bd_addr[6];
};

// Intel boot parameters response
struct intel_boot_params {
	uint8  status;
	uint8  otp_format;
	uint8  otp_content;
	uint8  otp_patch;
	uint16 dev_revid;
	uint8  secure_boot;
	uint8  key_from_hdr;
	uint8  key_type;
	uint8  otp_lock;
	uint8  api_lock;
	uint8  debug_lock;
	uint8  otp_bdaddr[6];
	uint8  min_fw_build_nn;
	uint8  min_fw_build_cw;
	uint8  min_fw_build_yy;
	uint8  limited_cce;
	uint8  unlocked_state;
} __attribute__((packed));

// Synchronous USB transfer context
struct intel_sync_ctx {
	sem_id      done;
	status_t    status;
	size_t      actual_len;
};


// Main entry point — called from device_added() for Intel devices
status_t btintel_setup(bt_usb_dev* bdev);


#endif // _BTINTEL_H_
