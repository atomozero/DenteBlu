#!/bin/bash
#
# Copyright 2025 Haiku, Inc.
# All rights reserved. Distributed under the terms of the MIT License.
#
# BLE end-to-end test script.
#
# Usage:
#   ble_test.sh              # smoke test only (no hardware needed)
#   ble_test.sh --device     # smoke test + interactive device test
#

set -u

# ---------- Paths ----------

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# Walk up to haiku/ root from src/tests/kits/bluetooth/
HAIKU_TOP="$(cd "$SCRIPT_DIR/../../../.." && pwd)"
BUILD_DIR="$HAIKU_TOP/generated/objects/haiku/x86_64/release"

BLE_SCAN="$BUILD_DIR/tests/kits/bluetooth/ble_scan"
BLE_CONNECT="$BUILD_DIR/tests/kits/bluetooth/ble_connect"
BLE_PAIR="$BUILD_DIR/tests/kits/bluetooth/ble_pair"
BT_SERVER="$BUILD_DIR/servers/bluetooth/bluetooth_server"
LIBBT="$BUILD_DIR/kits/bluetooth/libbluetooth.so"

# ---------- Counters ----------

PASS=0
FAIL=0
SKIP=0

# ---------- Helpers ----------

pass() {
	printf "  \033[32mPASS\033[0m  %s\n" "$1"
	PASS=$((PASS + 1))
}

fail() {
	printf "  \033[31mFAIL\033[0m  %s\n" "$1"
	FAIL=$((FAIL + 1))
}

skip() {
	printf "  \033[33mSKIP\033[0m  %s\n" "$1"
	SKIP=$((SKIP + 1))
}

separator() {
	echo "-------------------------------------------"
}

# ====================================================
# Part 1 — Smoke test (automated, no hardware needed)
# ====================================================

smoke_test() {
	echo ""
	echo "==========================================="
	echo "  BLE Smoke Test"
	echo "==========================================="
	echo ""

	# 1. Check binaries exist
	echo "[1] Checking binaries exist..."
	for bin in "$BLE_SCAN" "$BLE_CONNECT" "$BLE_PAIR"; do
		name="$(basename "$bin")"
		if [ -x "$bin" ]; then
			pass "$name exists and is executable"
		elif [ -f "$bin" ]; then
			fail "$name exists but is not executable"
		else
			fail "$name not found at $bin"
		fi
	done
	separator

	# 2. Check --help exits cleanly
	echo "[2] Checking --help output..."
	for bin in "$BLE_SCAN" "$BLE_CONNECT" "$BLE_PAIR"; do
		name="$(basename "$bin")"
		if [ ! -x "$bin" ]; then
			skip "$name --help (binary missing)"
			continue
		fi
		output=$("$bin" --help 2>&1)
		rc=$?
		if echo "$output" | grep -qi "usage"; then
			pass "$name --help shows usage (exit $rc)"
		else
			fail "$name --help (exit $rc, no usage text)"
			echo "       output: $output"
		fi
	done
	separator

	# 3. Check libbluetooth.so has BLE symbols
	echo "[3] Checking libbluetooth.so BLE symbols..."
	if [ ! -f "$LIBBT" ]; then
		fail "libbluetooth.so not found"
	else
		missing=""
		for sym in BleDevice NusClient StartScan; do
			if nm -DC "$LIBBT" 2>/dev/null | grep -q "$sym"; then
				pass "symbol $sym found"
			else
				fail "symbol $sym NOT found"
				missing="$missing $sym"
			fi
		done
	fi
	separator

	# 4. Check bluetooth_server binary
	echo "[4] Checking bluetooth_server..."
	if [ -x "$BT_SERVER" ]; then
		pass "bluetooth_server binary exists"
	elif [ -f "$BT_SERVER" ]; then
		fail "bluetooth_server exists but is not executable"
	else
		fail "bluetooth_server not found"
	fi

	# Check if server is running (use roster or ps)
	server_running=false
	if command -v roster >/dev/null 2>&1; then
		if roster 2>/dev/null | grep -q "bluetooth_server"; then
			server_running=true
			pass "bluetooth_server is running (roster)"
		else
			skip "bluetooth_server not running (needed for live tests)"
		fi
	else
		if ps 2>/dev/null | grep -q "[b]luetooth_server"; then
			server_running=true
			pass "bluetooth_server is running (ps)"
		else
			skip "bluetooth_server not running (needed for live tests)"
		fi
	fi
	separator

	# 5. Quick scan test (only if server is running)
	echo "[5] Quick scan test (3 seconds)..."
	if [ "$server_running" = false ]; then
		skip "scan test (bluetooth_server not running)"
	elif [ ! -x "$BLE_SCAN" ]; then
		skip "scan test (ble_scan binary missing)"
	else
		output=$("$BLE_SCAN" --duration 3 2>&1)
		rc=$?
		if [ $rc -eq 0 ]; then
			pass "ble_scan --duration 3 completed (exit 0)"
			count=$(echo "$output" | grep -o '[0-9]* devices' | head -1)
			if [ -n "$count" ]; then
				echo "       Found: $count"
			fi
		else
			fail "ble_scan --duration 3 failed (exit $rc)"
			echo "       output: $(echo "$output" | head -5)"
		fi
	fi
	separator

	# 6. Report
	echo ""
	echo "==========================================="
	total=$((PASS + FAIL + SKIP))
	printf "  Results: %d passed, %d failed, %d skipped (total %d)\n" \
		"$PASS" "$FAIL" "$SKIP" "$total"
	echo "==========================================="
	echo ""

	return $FAIL
}

# ====================================================
# Part 2 — Interactive device test (--device flag)
# ====================================================

device_test() {
	echo ""
	echo "==========================================="
	echo "  BLE Interactive Device Test"
	echo "==========================================="
	echo ""
	echo "This test guides you through scan -> connect -> pair"
	echo "with a real BLE device (e.g. Heltec V3 / MeshCore)."
	echo ""

	# Verify prerequisites
	for bin in "$BLE_SCAN" "$BLE_CONNECT" "$BLE_PAIR"; do
		if [ ! -x "$bin" ]; then
			echo "ERROR: $(basename "$bin") not found. Run smoke test first."
			return 1
		fi
	done

	# Step 1: Scan
	echo "--- Step 1: Scanning for BLE devices (15 seconds) ---"
	echo ""
	"$BLE_SCAN" --duration 15
	scan_rc=$?
	echo ""

	if [ $scan_rc -ne 0 ]; then
		echo "Scan failed (exit $scan_rc). Is bluetooth_server running?"
		return 1
	fi

	# Step 2: Get address from user
	echo "--- Step 2: Enter device address ---"
	echo ""
	printf "BD_ADDR (e.g. AA:BB:CC:DD:EE:FF): "
	read -r addr
	if [ -z "$addr" ]; then
		echo "No address entered, aborting."
		return 1
	fi

	printf "Address type (0=public, 1=random) [1]: "
	read -r addr_type
	addr_type="${addr_type:-1}"

	# Step 3: Connect
	echo ""
	echo "--- Step 3: Connecting to $addr (type $addr_type) ---"
	echo ""
	echo "NOTE: ble_connect will wait for Enter to disconnect."
	echo "      Press Enter when you are ready to proceed."
	echo ""
	"$BLE_CONNECT" "$addr" "$addr_type"
	connect_rc=$?

	if [ $connect_rc -ne 0 ]; then
		echo "Connect failed (exit $connect_rc)."
		echo ""
		echo "Continue with pairing test anyway? (y/N): "
		read -r cont
		if [ "$cont" != "y" ] && [ "$cont" != "Y" ]; then
			return 1
		fi
	fi

	# Step 4: Get passkey
	echo ""
	echo "--- Step 4: Enter passkey for pairing ---"
	echo ""
	printf "Passkey (6-digit number): "
	read -r passkey
	if [ -z "$passkey" ]; then
		echo "No passkey entered, aborting."
		return 1
	fi

	# Step 5: Pair + NUS
	echo ""
	echo "--- Step 5: Pairing + NUS test ---"
	echo ""
	"$BLE_PAIR" "$addr" "$addr_type" "$passkey"
	pair_rc=$?

	echo ""
	if [ $pair_rc -eq 0 ]; then
		echo "Pairing + NUS test PASSED."
	else
		echo "Pairing + NUS test FAILED (exit $pair_rc)."
	fi

	return $pair_rc
}

# ====================================================
# Main
# ====================================================

mode="smoke"
for arg in "$@"; do
	case "$arg" in
		--device)
			mode="device"
			;;
		--help|-h)
			echo "Usage: ble_test.sh [--device]"
			echo ""
			echo "  (no flags)   Run automated smoke test (no hardware needed)"
			echo "  --device     Run smoke test + interactive device test"
			echo ""
			exit 0
			;;
		*)
			echo "Unknown option: $arg"
			echo "Run with --help for usage."
			exit 1
			;;
	esac
done

smoke_test
smoke_rc=$?

if [ "$mode" = "device" ]; then
	device_test
	device_rc=$?
	exit $((smoke_rc + device_rc))
fi

exit $smoke_rc
