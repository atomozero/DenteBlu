/*
 * Copyright 2025 Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 * Standalone unit tests for Intel Bluetooth firmware loader logic.
 * Tests pure functions (TLV parsing, macro evaluation, filename generation)
 * without any USB/hardware dependency.
 *
 * Build:  jam bt_intel_test
 * Run:    generated/objects/haiku/x86_64/release/tests/kits/bluetooth/bt_intel_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>


// --- Stub ERROR() macro (replaces kernel dprintf / h2debug) ---
#define ERROR(fmt, ...) printf("  [btintel] " fmt, ##__VA_ARGS__)


// =====================================================================
// Copied from btintel.h — cannot include directly (depends on h2generic.h)
// =====================================================================

#define BTINTEL_FWID_MAXLEN 64

#define BTINTEL_IMG_BOOTLOADER 0x01
#define BTINTEL_IMG_IML        0x02
#define BTINTEL_IMG_OP         0x03

#define INTEL_CNVX_TOP_TYPE(cnvx_top)  ((cnvx_top) & 0x00000fff)
#define INTEL_CNVX_TOP_STEP(cnvx_top)  (((cnvx_top) & 0x0f000000) >> 24)

#define INTEL_CNVX_TOP_PACK_SWAB(t, s) \
	((uint16_t)(((((uint16_t)(((t) << 4) | (s))) & 0xFF) << 8) | \
	             ((((uint16_t)(((t) << 4) | (s))) >> 8) & 0xFF)))

#define BTINTEL_HW_PLATFORM(cnvx_bt) ((uint8_t)(((cnvx_bt) & 0x0000ff00) >> 8))
#define BTINTEL_HW_VARIANT(cnvx_bt)  ((uint8_t)(((cnvx_bt) & 0x003f0000) >> 16))

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

struct intel_tlv {
	uint8_t type;
	uint8_t len;
	uint8_t val[0];
} __attribute__((packed));

struct intel_version {
	uint8_t status;
	uint8_t hw_platform;
	uint8_t hw_variant;
	uint8_t hw_revision;
	uint8_t fw_variant;
	uint8_t fw_revision;
	uint8_t fw_build_num;
	uint8_t fw_build_ww;
	uint8_t fw_build_yy;
	uint8_t fw_patch_num;
} __attribute__((packed));

struct intel_version_tlv {
	uint32_t cnvi_top;
	uint32_t cnvr_top;
	uint32_t cnvi_bt;
	uint32_t cnvr_bt;
	uint16_t dev_rev_id;
	uint8_t  img_type;
	uint16_t timestamp;
	uint8_t  build_type;
	uint32_t build_num;
	uint8_t  secure_boot;
	uint8_t  otp_lock;
	uint8_t  api_lock;
	uint8_t  debug_lock;
	uint8_t  min_fw_build_nn;
	uint8_t  min_fw_build_cw;
	uint8_t  min_fw_build_yy;
	uint8_t  limited_cce;
	uint8_t  sbe_type;
	uint32_t git_sha1;
	char     fw_id[BTINTEL_FWID_MAXLEN];
	uint8_t  otp_bd_addr[6];
};


// =====================================================================
// Copied from btintel.cpp — pure functions only (no USB/hardware)
// =====================================================================

#ifndef min_c
#define min_c(a, b) ((a) < (b) ? (a) : (b))
#endif

// Haiku status_t aliases for userspace test
#ifndef B_OK
#define B_OK       0
#define B_ERROR    (-1)
#define B_BAD_DATA (-2)
#endif

// Replace Haiku format macros with standard ones
#ifndef B_PRIuSIZE
#define B_PRIuSIZE "zu"
#endif

typedef int status_t;


static status_t
btintel_parse_version_tlv(const uint8_t* data, size_t len,
	intel_version_tlv* ver)
{
	if (len < 1) {
		ERROR("TLV response empty\n");
		return B_BAD_DATA;
	}

	if (data[0] != 0x00) {
		ERROR("TLV version command returned status 0x%02x\n", data[0]);
		return B_ERROR;
	}

	data++;
	len--;

	while (len > 0) {
		if (len < 2) {
			ERROR("TLV truncated (%" B_PRIuSIZE " bytes remaining)\n", len);
			return B_BAD_DATA;
		}

		uint8_t type = data[0];
		uint8_t vlen = data[1];

		if (len < (size_t)(2 + vlen)) {
			ERROR("TLV entry type=0x%02x len=%d exceeds remaining "
				"data (%" B_PRIuSIZE ")\n", type, vlen, len);
			return B_BAD_DATA;
		}

		const uint8_t* val = data + 2;

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
				break;
		}

		data += 2 + vlen;
		len -= 2 + vlen;
	}

	return B_OK;
}


static void
btintel_get_fw_name_tlv(const intel_version_tlv* ver, char* buf,
	size_t buf_size, const char* suffix)
{
	uint16_t cnvi = INTEL_CNVX_TOP_PACK_SWAB(
		INTEL_CNVX_TOP_TYPE(ver->cnvi_top),
		INTEL_CNVX_TOP_STEP(ver->cnvi_top));
	uint16_t cnvr = INTEL_CNVX_TOP_PACK_SWAB(
		INTEL_CNVX_TOP_TYPE(ver->cnvr_top),
		INTEL_CNVX_TOP_STEP(ver->cnvr_top));

	if (BTINTEL_HW_VARIANT(ver->cnvi_bt) >= 0x1e) {
		if (ver->img_type == BTINTEL_IMG_BOOTLOADER) {
			snprintf(buf, buf_size, "ibt-%04x-%04x-iml.%s",
				cnvi, cnvr, suffix);
			return;
		}
		if (ver->fw_id[0] != '\0') {
			snprintf(buf, buf_size, "ibt-%04x-%04x-%s.%s",
				cnvi, cnvr, ver->fw_id, suffix);
			return;
		}
	}

	snprintf(buf, buf_size, "ibt-%04x-%04x.%s", cnvi, cnvr, suffix);
}


// =====================================================================
// Test infrastructure (same pattern as bt_hci_le_cmd_test.cpp)
// =====================================================================

static int sTestCount = 0;
static int sPassCount = 0;


static void
Check(bool condition, const char* description)
{
	sTestCount++;
	if (condition) {
		sPassCount++;
		printf("  PASS: %s\n", description);
	} else {
		printf("  FAIL: %s\n", description);
	}
}


// =====================================================================
// Helper: build a TLV buffer with status byte + entries
// =====================================================================

static size_t
AppendTlv(uint8_t* buf, size_t offset, uint8_t type, const void* val,
	uint8_t len)
{
	buf[offset] = type;
	buf[offset + 1] = len;
	memcpy(buf + offset + 2, val, len);
	return offset + 2 + len;
}


static size_t
AppendTlvU8(uint8_t* buf, size_t offset, uint8_t type, uint8_t val)
{
	return AppendTlv(buf, offset, type, &val, 1);
}


static size_t
AppendTlvU16(uint8_t* buf, size_t offset, uint8_t type, uint16_t val)
{
	return AppendTlv(buf, offset, type, &val, 2);
}


static size_t
AppendTlvU32(uint8_t* buf, size_t offset, uint8_t type, uint32_t val)
{
	return AppendTlv(buf, offset, type, &val, 4);
}


// =====================================================================
// Test groups
// =====================================================================

static void
TestStructSizes()
{
	printf("Test 1: Struct sizes\n");

	Check(sizeof(intel_version) == 10,
		"sizeof(intel_version) == 10");
	Check(sizeof(intel_tlv) == 2,
		"sizeof(intel_tlv) == 2 (type + len, flexible array)");
}


static void
TestCnvxTopMacros()
{
	printf("Test 2: CNVX_TOP_TYPE / STEP macros\n");

	// AX201: cnvi_top has type=0x900, step=0
	uint32_t ax201_cnvi = 0x00000900;
	Check(INTEL_CNVX_TOP_TYPE(ax201_cnvi) == 0x900,
		"TYPE(0x00000900) == 0x900");
	Check(INTEL_CNVX_TOP_STEP(ax201_cnvi) == 0,
		"STEP(0x00000900) == 0");

	// AX211: cnvi_top has type=0x900, step=1
	uint32_t ax211_cnvi = 0x01000900;
	Check(INTEL_CNVX_TOP_TYPE(ax211_cnvi) == 0x900,
		"TYPE(0x01000900) == 0x900");
	Check(INTEL_CNVX_TOP_STEP(ax211_cnvi) == 1,
		"STEP(0x01000900) == 1");

	// step=3
	uint32_t step3 = 0x03000900;
	Check(INTEL_CNVX_TOP_STEP(step3) == 3,
		"STEP(0x03000900) == 3");

	// type=0x910
	uint32_t type910 = 0x00000910;
	Check(INTEL_CNVX_TOP_TYPE(type910) == 0x910,
		"TYPE(0x00000910) == 0x910");
}


static void
TestPackSwab()
{
	printf("Test 3: PACK_SWAB macro\n");

	// AX201: type=0x900, step=0 → (0x900<<4)|0 = 0x9000 → swab → 0x0090
	Check(INTEL_CNVX_TOP_PACK_SWAB(0x900, 0) == 0x0090,
		"PACK_SWAB(0x900, 0) == 0x0090");

	// AX211: type=0x900, step=1 → (0x900<<4)|1 = 0x9001 → swab → 0x0190
	Check(INTEL_CNVX_TOP_PACK_SWAB(0x900, 1) == 0x0190,
		"PACK_SWAB(0x900, 1) == 0x0190");

	// type=0x910, step=0 → (0x910<<4)|0 = 0x9100 → swab → 0x0091
	Check(INTEL_CNVX_TOP_PACK_SWAB(0x910, 0) == 0x0091,
		"PACK_SWAB(0x910, 0) == 0x0091");

	// type=0xA00, step=2 → (0xA00<<4)|2 = 0xA002 → swab → 0x02A0
	Check(INTEL_CNVX_TOP_PACK_SWAB(0xA00, 2) == 0x02A0,
		"PACK_SWAB(0xA00, 2) == 0x02A0");
}


static void
TestHwPlatformVariant()
{
	printf("Test 4: HW_PLATFORM / HW_VARIANT macros\n");

	// cnvi_bt=0x00173700 → platform byte[1]=0x37, variant byte[2]=0x17
	uint32_t cnvi_bt1 = 0x00173700;
	Check(BTINTEL_HW_PLATFORM(cnvi_bt1) == 0x37,
		"HW_PLATFORM(0x00173700) == 0x37");
	Check(BTINTEL_HW_VARIANT(cnvi_bt1) == 0x17,
		"HW_VARIANT(0x00173700) == 0x17");

	// Blazar: cnvi_bt=0x001E3700 → variant=0x1E
	uint32_t cnvi_bt2 = 0x001E3700;
	Check(BTINTEL_HW_PLATFORM(cnvi_bt2) == 0x37,
		"HW_PLATFORM(0x001E3700) == 0x37");
	Check(BTINTEL_HW_VARIANT(cnvi_bt2) == 0x1E,
		"HW_VARIANT(0x001E3700) == 0x1E (Blazar)");

	// Edge: variant=0x3F (max 6-bit value)
	uint32_t cnvi_bt3 = 0x003F0000;
	Check(BTINTEL_HW_VARIANT(cnvi_bt3) == 0x3F,
		"HW_VARIANT(0x003F0000) == 0x3F (max)");
}


static void
TestTlvParserComplete()
{
	printf("Test 5: TLV parser — complete AX201 synthetic response\n");

	// Build a synthetic TLV response buffer:
	// [status=0x00] [TLV entries...]
	uint8_t buf[512];
	size_t off = 0;

	buf[off++] = 0x00; // status OK

	uint32_t cnvi_top = 0x00000900; // type=0x900, step=0
	uint32_t cnvr_top = 0x01000140; // type=0x140, step=1
	uint32_t cnvi_bt  = 0x00173700; // platform=0x37, variant=0x17
	uint32_t cnvr_bt  = 0x00001234;
	uint16_t dev_rev  = 0x0042;
	uint32_t build_num = 0xDEADBEEF;
	uint32_t git_sha  = 0xCAFEBABE;

	off = AppendTlvU32(buf, off, INTEL_TLV_CNVI_TOP, cnvi_top);
	off = AppendTlvU32(buf, off, INTEL_TLV_CNVR_TOP, cnvr_top);
	off = AppendTlvU32(buf, off, INTEL_TLV_CNVI_BT, cnvi_bt);
	off = AppendTlvU32(buf, off, INTEL_TLV_CNVR_BT, cnvr_bt);
	off = AppendTlvU16(buf, off, INTEL_TLV_DEV_REV_ID, dev_rev);
	off = AppendTlvU8(buf, off, INTEL_TLV_IMAGE_TYPE, BTINTEL_IMG_BOOTLOADER);
	off = AppendTlvU8(buf, off, INTEL_TLV_BUILD_TYPE, 0x01);
	off = AppendTlvU32(buf, off, INTEL_TLV_BUILD_NUM, build_num);
	off = AppendTlvU8(buf, off, INTEL_TLV_SECURE_BOOT, 0x01);
	off = AppendTlvU8(buf, off, INTEL_TLV_OTP_LOCK, 0x01);
	off = AppendTlvU8(buf, off, INTEL_TLV_API_LOCK, 0x00);
	off = AppendTlvU8(buf, off, INTEL_TLV_DEBUG_LOCK, 0x01);
	off = AppendTlvU8(buf, off, INTEL_TLV_LIMITED_CCE, 0x00);
	off = AppendTlvU8(buf, off, INTEL_TLV_SBE_TYPE, 0x02);
	off = AppendTlvU32(buf, off, INTEL_TLV_GIT_SHA1, git_sha);

	uint8_t bdaddr[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
	off = AppendTlv(buf, off, INTEL_TLV_OTP_BDADDR, bdaddr, 6);

	intel_version_tlv ver;
	memset(&ver, 0, sizeof(ver));
	status_t err = btintel_parse_version_tlv(buf, off, &ver);

	Check(err == B_OK, "parse returns B_OK");
	Check(ver.cnvi_top == cnvi_top, "cnvi_top parsed correctly");
	Check(ver.cnvr_top == cnvr_top, "cnvr_top parsed correctly");
	Check(ver.cnvi_bt == cnvi_bt, "cnvi_bt parsed correctly");
	Check(ver.cnvr_bt == cnvr_bt, "cnvr_bt parsed correctly");
	Check(ver.dev_rev_id == dev_rev, "dev_rev_id parsed correctly");
	Check(ver.img_type == BTINTEL_IMG_BOOTLOADER, "img_type == BOOTLOADER");
	Check(ver.build_type == 0x01, "build_type == 0x01");
	Check(ver.build_num == build_num, "build_num parsed correctly");
	Check(ver.secure_boot == 0x01, "secure_boot == 1");
	Check(ver.otp_lock == 0x01, "otp_lock == 1");
	Check(ver.api_lock == 0x00, "api_lock == 0");
	Check(ver.debug_lock == 0x01, "debug_lock == 1");
	Check(ver.limited_cce == 0x00, "limited_cce == 0");
	Check(ver.sbe_type == 0x02, "sbe_type == 0x02");
	Check(ver.git_sha1 == git_sha, "git_sha1 parsed correctly");
	Check(memcmp(ver.otp_bd_addr, bdaddr, 6) == 0,
		"otp_bd_addr parsed correctly");
}


static void
TestTlvParserErrors()
{
	printf("Test 6: TLV parser — error cases\n");

	intel_version_tlv ver;

	// Empty buffer
	memset(&ver, 0, sizeof(ver));
	Check(btintel_parse_version_tlv(NULL, 0, &ver) == B_BAD_DATA,
		"empty buffer returns B_BAD_DATA");

	// Non-zero status
	uint8_t bad_status[] = {0x01};
	memset(&ver, 0, sizeof(ver));
	Check(btintel_parse_version_tlv(bad_status, 1, &ver) == B_ERROR,
		"status=0x01 returns B_ERROR");

	// Truncated TLV header (status OK + only 1 byte)
	uint8_t trunc_hdr[] = {0x00, 0x10};
	memset(&ver, 0, sizeof(ver));
	Check(btintel_parse_version_tlv(trunc_hdr, 2, &ver) == B_BAD_DATA,
		"truncated TLV header returns B_BAD_DATA");

	// TLV len overflows remaining data
	uint8_t overflow[] = {0x00, 0x10, 0x04, 0x01, 0x02}; // type=0x10, len=4, but only 3 bytes
	memset(&ver, 0, sizeof(ver));
	Check(btintel_parse_version_tlv(overflow, 5, &ver) == B_BAD_DATA,
		"TLV len overflow returns B_BAD_DATA");

	// Unknown type — should be silently skipped, parse succeeds
	uint8_t unknown[] = {0x00, 0xFF, 0x01, 0xAA};
	memset(&ver, 0, sizeof(ver));
	Check(btintel_parse_version_tlv(unknown, 4, &ver) == B_OK,
		"unknown TLV type silently skipped, returns B_OK");
}


static void
TestTlvParserFwId()
{
	printf("Test 7: TLV parser — FW_ID string\n");

	// Short fw_id
	{
		uint8_t buf[128];
		size_t off = 0;
		buf[off++] = 0x00; // status

		const char* fw_id = "myfw";
		off = AppendTlv(buf, off, INTEL_TLV_FW_ID, fw_id, strlen(fw_id));

		intel_version_tlv ver;
		memset(&ver, 0, sizeof(ver));
		status_t err = btintel_parse_version_tlv(buf, off, &ver);
		Check(err == B_OK, "short fw_id parse OK");
		Check(strcmp(ver.fw_id, "myfw") == 0,
			"short fw_id == \"myfw\"");
	}

	// Long fw_id — should be truncated to BTINTEL_FWID_MAXLEN-1
	{
		uint8_t buf[256];
		size_t off = 0;
		buf[off++] = 0x00;

		// Create a 100-char string (longer than BTINTEL_FWID_MAXLEN=64)
		char long_id[100];
		memset(long_id, 'X', 99);
		long_id[99] = '\0';
		off = AppendTlv(buf, off, INTEL_TLV_FW_ID, long_id, 99);

		intel_version_tlv ver;
		memset(&ver, 0, sizeof(ver));
		status_t err = btintel_parse_version_tlv(buf, off, &ver);
		Check(err == B_OK, "long fw_id parse OK");
		Check(strlen(ver.fw_id) == BTINTEL_FWID_MAXLEN - 1,
			"long fw_id truncated to 63 chars");
		// Verify all copied chars are 'X'
		bool all_x = true;
		for (int i = 0; i < BTINTEL_FWID_MAXLEN - 1; i++) {
			if (ver.fw_id[i] != 'X') {
				all_x = false;
				break;
			}
		}
		Check(all_x, "truncated fw_id content correct");
	}
}


static void
TestFwNameStandard()
{
	printf("Test 8: Firmware filename — standard (non-Blazar)\n");

	intel_version_tlv ver;
	memset(&ver, 0, sizeof(ver));

	// AX201-like: cnvi_top type=0x900 step=0, cnvr_top type=0x140 step=0
	// hw_variant=0x17 (< 0x1E, non-Blazar)
	// PACK_SWAB(0x900, 0): (0x900<<4)|0=0x9000 → swab → 0x0090
	// PACK_SWAB(0x140, 0): (0x140<<4)|0=0x1400 → swab → 0x0014
	ver.cnvi_top = 0x00000900; // type=0x900, step=0
	ver.cnvr_top = 0x00000140; // type=0x140, step=0
	ver.cnvi_bt  = 0x00173700; // variant=0x17
	ver.img_type = BTINTEL_IMG_BOOTLOADER;

	char name[128];

	// SFI filename
	btintel_get_fw_name_tlv(&ver, name, sizeof(name), "sfi");
	Check(strcmp(name, "ibt-0090-0014.sfi") == 0,
		"standard SFI: ibt-0090-0014.sfi");

	// DDC filename
	btintel_get_fw_name_tlv(&ver, name, sizeof(name), "ddc");
	Check(strcmp(name, "ibt-0090-0014.ddc") == 0,
		"standard DDC: ibt-0090-0014.ddc");
}


static void
TestFwNameBlazar()
{
	printf("Test 9: Firmware filename — Blazar (hw_variant >= 0x1E)\n");

	intel_version_tlv ver;
	memset(&ver, 0, sizeof(ver));

	// Blazar chip: cnvi_top type=0x900, step=1; cnvr_top type=0x140, step=0
	// hw_variant=0x1E (Blazar)
	// PACK_SWAB(0x900, 1): (0x900<<4)|1=0x9001 → swab → 0x0190
	// PACK_SWAB(0x140, 0): (0x140<<4)|0=0x1400 → swab → 0x0014
	ver.cnvi_top = 0x01000900; // type=0x900, step=1 → 0x0190
	ver.cnvr_top = 0x00000140; // type=0x140, step=0 → 0x0014
	ver.cnvi_bt  = 0x001E3700; // variant=0x1E

	char name[128];

	// Bootloader → IML filename
	ver.img_type = BTINTEL_IMG_BOOTLOADER;
	ver.fw_id[0] = '\0';
	btintel_get_fw_name_tlv(&ver, name, sizeof(name), "sfi");
	Check(strcmp(name, "ibt-0190-0014-iml.sfi") == 0,
		"Blazar bootloader: ibt-0190-0014-iml.sfi");

	// IML with fw_id → operational firmware filename
	ver.img_type = BTINTEL_IMG_IML;
	strncpy(ver.fw_id, "myfw", sizeof(ver.fw_id));
	btintel_get_fw_name_tlv(&ver, name, sizeof(name), "sfi");
	Check(strcmp(name, "ibt-0190-0014-myfw.sfi") == 0,
		"Blazar IML+fw_id: ibt-0190-0014-myfw.sfi");

	// IML without fw_id → fallback (no suffix)
	ver.img_type = BTINTEL_IMG_IML;
	ver.fw_id[0] = '\0';
	btintel_get_fw_name_tlv(&ver, name, sizeof(name), "sfi");
	Check(strcmp(name, "ibt-0190-0014.sfi") == 0,
		"Blazar IML no fw_id: ibt-0190-0014.sfi (fallback)");

	// Blazar DDC with fw_id
	ver.img_type = BTINTEL_IMG_OP;
	strncpy(ver.fw_id, "opfw", sizeof(ver.fw_id));
	btintel_get_fw_name_tlv(&ver, name, sizeof(name), "ddc");
	Check(strcmp(name, "ibt-0190-0014-opfw.ddc") == 0,
		"Blazar DDC+fw_id: ibt-0190-0014-opfw.ddc");
}


static void
TestLegacyVsTlvDetection()
{
	printf("Test 10: Legacy vs TLV detection logic\n");

	// Detection logic from btintel_setup():
	//   Legacy: exactly sizeof(intel_version)==10 && byte[1]==0x37
	//   TLV:    anything else

	// Case 1: 10 bytes, byte[1]==0x37 → legacy
	{
		uint8_t data[10] = {0x00, 0x37, 0x07, 0x00, 0x06, 0x00,
			0x00, 0x00, 0x00, 0x00};
		bool is_legacy = (sizeof(data) == sizeof(intel_version)
			&& data[1] == 0x37);
		Check(is_legacy, "10 bytes + byte[1]==0x37 → legacy");
	}

	// Case 2: 10 bytes, byte[1]!=0x37 → TLV
	{
		uint8_t data[10] = {0x00, 0x50, 0x07, 0x00, 0x06, 0x00,
			0x00, 0x00, 0x00, 0x00};
		bool is_legacy = (sizeof(data) == sizeof(intel_version)
			&& data[1] == 0x37);
		Check(!is_legacy, "10 bytes + byte[1]==0x50 → TLV");
	}

	// Case 3: 50 bytes, byte[1]==0x37 → TLV (length != 10)
	{
		uint8_t data[50];
		memset(data, 0, sizeof(data));
		data[1] = 0x37;
		size_t len = sizeof(data);
		bool is_legacy = (len == sizeof(intel_version) && data[1] == 0x37);
		Check(!is_legacy, "50 bytes + byte[1]==0x37 → TLV (length != 10)");
	}
}


// =====================================================================
// Main
// =====================================================================

int
main()
{
	printf("=== Intel BT Firmware Loader Tests ===\n\n");

	TestStructSizes();
	TestCnvxTopMacros();
	TestPackSwab();
	TestHwPlatformVariant();
	TestTlvParserComplete();
	TestTlvParserErrors();
	TestTlvParserFwId();
	TestFwNameStandard();
	TestFwNameBlazar();
	TestLegacyVsTlvDetection();

	printf("\n=== Results: %d/%d passed ===\n", sPassCount, sTestCount);
	return (sPassCount == sTestCount) ? 0 : 1;
}
