#!/bin/sh
#
# DenteBlu r007 — Bluetooth stack installer for Haiku
#
# Usage:  sh install.sh                (install everything)
#         sh install.sh --check        (diagnostics only, don't install)
#         sh install.sh --firmware-only (download/update Intel firmware only)
#

RELEASE="r007"
NP="/boot/system/non-packaged"
FW_DIR="$NP/data/firmware/intel"
SCRIPT_DIR="$(dirname "$0")"
BASE_URL="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/intel"
MIN_FW_SIZE=10240
ERRORS=0
INSTALLED=0

# ── helpers ──────────────────────────────────────────────────

ok()   { echo "  [OK]  $1"; INSTALLED=$((INSTALLED + 1)); }
fail() { echo "  [!!]  $1 — $2"; ERRORS=$((ERRORS + 1)); }
skip() { echo "  [--]  $1 (skipped)"; }
info() { echo "  [ii]  $1"; }
warn() { echo "  [WW]  $1"; }
sep()  { echo ""; }

install_file() {
    # install_file <src> <dst_dir> <name>
    src="$1"; dst="$2"; name="$3"
    mkdir -p "$dst" || { fail "$name" "cannot create $dst"; return 1; }
    basename_src="$(basename "$src")"
    if [ -f "$dst/$basename_src" ]; then
        # IMPORTANT: do NOT use .bak extension for kernel drivers!
        # Haiku's devfs loads ANY file in drivers/bin/ as a driver.
        # A .bak copy would be loaded as a separate driver instance.
        cp "$dst/$basename_src" "$dst/${basename_src}.prev" 2>/dev/null
    fi
    # Remove any leftover .bak files from previous installs (they get
    # loaded as separate driver instances by devfs, causing double
    # firmware attempts and wasted device table slots)
    rm -f "$dst/${basename_src}.bak" 2>/dev/null
    cp "$src" "$dst/" || { fail "$name" "copy failed"; return 1; }
    ok "$name  ->  $dst/"
}

install_link() {
    # install_link <target> <link_path> <name>
    target="$1"; link="$2"; name="$3"
    mkdir -p "$(dirname "$link")" || { fail "$name" "cannot create dir"; return 1; }
    if [ ! -e "$link" ]; then
        ln -sf "$target" "$link" || { fail "$name" "symlink failed"; return 1; }
        ok "$name (symlink)"
    else
        skip "$name (symlink already exists)"
    fi
}

# USB product ID → firmware name map
fw_name_from_usb_product() {
    case "$1" in
        0x0032|0032) echo "ibt-1040-0041" ;;  # AX201 (Alder Lake)
        0x0033|0033) echo "ibt-1040-4150" ;;  # AX201 (Tiger Lake)
        0x0026|0026) echo "ibt-0040-0041" ;;  # AX201 variant
        0x0029|0029) echo "ibt-0040-4150" ;;  # AX201 variant
        0x0aa7|0aa7) echo "ibt-1040-0041" ;;  # AX201 variant
        0x0025|0025) echo "ibt-0040-0041" ;;  # AX201 variant
        0x0aaa|0aaa) echo "ibt-0040-0041" ;;  # AX201 variant
        *) echo "" ;;
    esac
}

validate_download() {
    # Returns 0 if file exists and > MIN_FW_SIZE
    if [ ! -f "$1" ]; then return 1; fi
    size=$(ls -l "$1" | awk '{print $5}')
    if [ "$size" -lt "$MIN_FW_SIZE" ] 2>/dev/null; then
        fail "$(basename "$1")" "too small ($size bytes) — likely a 404 page"
        rm -f "$1"
        return 1
    fi
    return 0
}

detect_intel_fw_name() {
    # Look for firmware name in syslog — matches both TLV and legacy formats:
    #   "btintel: TLV bootloader, loading firmware: ibt-1040-0041.sfi"
    #   "btintel: looking for firmware: ibt-19-0-4.sfi"
    for logfile in /var/log/syslog /tmp/bt_server.log /var/log/syslog.old; do
        if [ -f "$logfile" ]; then
            name=$(grep "btintel:.*firmware:" "$logfile" 2>/dev/null \
                | grep "ibt-" | tail -1 \
                | sed 's/.*firmware: //' | sed 's/\.sfi$//' | tr -d ' ')
            if [ -n "$name" ]; then echo "$name"; return 0; fi
        fi
    done
    # Fallback: USB product ID
    if command -v listusb >/dev/null 2>&1; then
        product_id=$(listusb 2>/dev/null | grep "8087:" | head -1 \
            | sed 's/.*8087://' | awk '{print $1}' | tr -d ' ')
        if [ -n "$product_id" ]; then
            name=$(fw_name_from_usb_product "$product_id")
            if [ -n "$name" ]; then echo "$name"; return 0; fi
        fi
    fi
    return 1
}

download_intel_fw() {
    fw_name="$1"
    sfi="${fw_name}.sfi"
    ddc="${fw_name}.ddc"

    mkdir -p "$FW_DIR"

    info "Downloading $sfi..."
    wget -q -O "$FW_DIR/$sfi" "$BASE_URL/$sfi" 2>/dev/null
    if [ $? -eq 0 ] && validate_download "$FW_DIR/$sfi" "$sfi"; then
        size=$(ls -l "$FW_DIR/$sfi" | awk '{print $5}')
        ok "$sfi downloaded ($size bytes)"
    else
        rm -f "$FW_DIR/$sfi"
        fail "$sfi" "not found in linux-firmware"
        return 1
    fi

    info "Downloading $ddc..."
    wget -q -O "$FW_DIR/$ddc" "$BASE_URL/$ddc" 2>/dev/null
    if [ $? -eq 0 ] && validate_download "$FW_DIR/$ddc" "$ddc"; then
        size=$(ls -l "$FW_DIR/$ddc" | awk '{print $5}')
        ok "$ddc downloaded ($size bytes)"
    else
        rm -f "$FW_DIR/$ddc"
        info "$ddc not available (optional, non-critical)"
    fi

    return 0
}

stop_bluetooth_server() {
    SERVER_PID=$(ps 2>/dev/null | grep bluetooth_server | grep -v grep | awk '{print $2}')
    if [ -n "$SERVER_PID" ]; then
        # Remove Deskbar replicant first (before killing server)
        if command -v DeskbarReplicantRemove >/dev/null 2>&1; then
            DeskbarReplicantRemove "BluetoothDeskbarReplicant" 2>/dev/null
        fi
        kill "$SERVER_PID" 2>/dev/null
        sleep 2
        if ps 2>/dev/null | grep bluetooth_server | grep -v grep > /dev/null; then
            fail "bluetooth_server" "could not stop (PID $SERVER_PID)"
            echo ""
            echo "Please close bluetooth_server manually and re-run this script."
            exit 1
        fi
        ok "bluetooth_server stopped (was PID $SERVER_PID)"
    else
        skip "bluetooth_server (not running)"
    fi
}

# ══════════════════════════════════════════════════════════════
# PRE-FLIGHT CHECKS (common to all modes)
# ══════════════════════════════════════════════════════════════

echo ""
echo "======================================"
echo "  DenteBlu $RELEASE — Bluetooth stack"
echo "======================================"
sep

# Must be Haiku
if [ ! -d /boot/system ]; then
    echo "ERROR: This script must be run on Haiku OS."
    exit 1
fi

# Architecture check
ARCH=$(getarch 2>/dev/null || uname -m)
case "$ARCH" in
    x86_64*) ;;
    *)  warn "This package is built for x86_64 (detected: $ARCH)"
        warn "Binaries may not work on this system."
        sep
        ;;
esac

# Detect Bluetooth hardware
HAS_BT_USB=false
IS_INTEL=false
INTEL_PRODUCT=""

if command -v listusb >/dev/null 2>&1; then
    if listusb 2>/dev/null | grep -qi "bluetooth\|8087"; then
        HAS_BT_USB=true
    fi
    if listusb 2>/dev/null | grep -q "8087:"; then
        IS_INTEL=true
        INTEL_PRODUCT=$(listusb 2>/dev/null | grep "8087:" | head -1 \
            | sed 's/.*8087://' | awk '{print $1}' | tr -d ' ')
    fi
fi

# ══════════════════════════════════════════════════════════════
# MODE: --check (diagnostics only)
# ══════════════════════════════════════════════════════════════

if [ "$1" = "--check" ]; then
    echo "System diagnostics:"
    sep

    # USB Bluetooth device
    if [ "$HAS_BT_USB" = true ]; then
        info "Bluetooth USB device detected:"
        listusb 2>/dev/null | grep -i "bluetooth\|8087" | sed 's/^/         /'
    else
        warn "No Bluetooth USB device detected."
    fi
    sep

    # Current installation status
    echo "Installed components:"
    for f in "$NP/servers/bluetooth_server" \
             "$NP/lib/libbluetooth.so" \
             "$NP/add-ons/kernel/bluetooth/btCoreData" \
             "$NP/add-ons/kernel/bluetooth/hci" \
             "$NP/add-ons/kernel/network/protocols/l2cap" \
             "$NP/add-ons/kernel/drivers/bin/h2generic" \
             "$NP/preferences/Bluetooth" \
             "$NP/add-ons/BluetoothDeskbarReplicant" \
             "$NP/bin/BluetoothSendFile"; do
        if [ -f "$f" ]; then
            fsize=$(ls -lh "$f" 2>/dev/null | awk '{print $5}')
            fdate=$(ls -l "$f" 2>/dev/null | awk '{print $6, $7, $8}')
            info "$(basename "$f")  installed  ($fsize, $fdate)"
        else
            warn "$(basename "$f")  NOT installed"
        fi
    done
    sep

    # Intel firmware status
    if [ "$IS_INTEL" = true ]; then
        info "Intel Bluetooth controller detected (product ID: 8087:$INTEL_PRODUCT)"
        if [ -d "$FW_DIR" ]; then
            sfi_count=$(ls "$FW_DIR"/*.sfi 2>/dev/null | wc -l)
            if [ "$sfi_count" -gt 0 ]; then
                info "Intel firmware: $sfi_count .sfi file(s) in $FW_DIR"
                ls -lh "$FW_DIR"/*.sfi 2>/dev/null | awk '{print "         " $NF " (" $5 ")"}'
            else
                warn "Intel firmware: MISSING — run: sh install.sh --firmware-only"
            fi
        else
            warn "Intel firmware directory missing: $FW_DIR"
            warn "Run: sh install.sh --firmware-only"
        fi

        # Check syslog for firmware issues
        if [ -f /var/log/syslog ]; then
            fw_errors=$(grep -c "Intel firmware setup failed" /var/log/syslog 2>/dev/null)
            if [ "$fw_errors" -gt 0 ]; then
                warn "syslog: $fw_errors firmware failure messages found"
                grep "Intel firmware" /var/log/syslog 2>/dev/null | tail -3 | sed 's/^/         /'
            fi
            xhci_errors=$(grep -c "ConfigureEndpoint.*failed" /var/log/syslog 2>/dev/null)
            if [ "$xhci_errors" -gt 0 ]; then
                warn "syslog: $xhci_errors XHCI ConfigureEndpoint errors (known Intel issue)"
            fi
        fi
    fi
    sep

    # Disk space
    avail=$(df /boot 2>/dev/null | tail -1 | awk '{print $4}')
    if [ -n "$avail" ]; then
        info "Disk space on /boot: $avail KB available"
    fi
    sep

    # bluetooth_server status
    if ps 2>/dev/null | grep bluetooth_server | grep -v grep > /dev/null; then
        info "bluetooth_server is running"
        bt_log_size=""
        if [ -f /tmp/bt_server.log ]; then
            bt_log_size=$(ls -lh /tmp/bt_server.log 2>/dev/null | awk '{print $5}')
            info "bt_server.log: $bt_log_size"
        fi
    else
        info "bluetooth_server is NOT running"
    fi
    sep

    exit 0
fi

# ══════════════════════════════════════════════════════════════
# MODE: --firmware-only
# ══════════════════════════════════════════════════════════════

if [ "$1" = "--firmware-only" ]; then
    echo "Intel Bluetooth firmware download"
    sep

    if [ "$IS_INTEL" = false ]; then
        warn "No Intel Bluetooth USB device detected."
        echo ""
        echo "  If you know the firmware name, run:"
        echo "    sh download_intel_fw.sh ibt-1040-0041"
        echo ""
        exit 1
    fi

    info "Intel BT controller: 8087:$INTEL_PRODUCT"

    FW_NAME=$(detect_intel_fw_name)
    if [ -z "$FW_NAME" ]; then
        fail "firmware" "could not auto-detect firmware name"
        echo ""
        echo "  Try specifying manually:"
        echo "    sh download_intel_fw.sh ibt-1040-0041"
        echo "    sh download_intel_fw.sh --list"
        echo ""
        exit 1
    fi

    info "Detected firmware: $FW_NAME"
    sep

    # Check existing
    if [ -f "$FW_DIR/${FW_NAME}.sfi" ]; then
        size=$(ls -l "$FW_DIR/${FW_NAME}.sfi" | awk '{print $5}')
        info "Firmware already installed: ${FW_NAME}.sfi ($size bytes)"
        echo ""
        printf "  Re-download? [y/N] "
        read answer
        if [ "$answer" != "y" ] && [ "$answer" != "Y" ]; then
            echo "  Keeping existing firmware."
            exit 0
        fi
    fi

    download_intel_fw "$FW_NAME"
    sep

    echo "======================================"
    if [ $ERRORS -eq 0 ]; then
        echo "  Firmware downloaded successfully"
    else
        echo "  Firmware download had errors"
    fi
    echo "======================================"
    sep
    echo "  ** REBOOT required to load the firmware **"
    sep
    exit $ERRORS
fi

# ══════════════════════════════════════════════════════════════
# MODE: full install (default)
# ══════════════════════════════════════════════════════════════

# ── Step 1/6: Pre-flight ────────────────────────────────────

echo "Step 1/6: Pre-flight checks..."
sep

if [ "$HAS_BT_USB" = true ]; then
    info "Bluetooth USB device found"
else
    warn "No Bluetooth USB device detected — installing anyway"
fi

# Check required files exist in package
MISSING_PKG=false
for f in servers/bluetooth_server lib/libbluetooth.so \
         kernel/bluetooth/btCoreData kernel/bluetooth/hci \
         kernel/network/protocols/l2cap \
         kernel/drivers/bluetooth/h2/h2generic; do
    if [ ! -f "$SCRIPT_DIR/$f" ]; then
        fail "$(basename "$f")" "not found in package"
        MISSING_PKG=true
    fi
done
if [ "$MISSING_PKG" = true ]; then
    echo ""
    echo "ERROR: Package incomplete. Re-download the release zip."
    exit 1
fi

# Disk space (need ~2MB)
avail=$(df /boot 2>/dev/null | tail -1 | awk '{print $4}')
if [ -n "$avail" ] && [ "$avail" -lt 2048 ] 2>/dev/null; then
    fail "disk" "less than 2MB free on /boot"
    exit 1
fi

ok "Pre-flight passed"
sep

# ── Step 2/6: Stop bluetooth_server ────────────────────────

echo "Step 2/6: Stopping bluetooth_server..."
sep

stop_bluetooth_server
sep

# ── Step 3/6: Install userspace components ──────────────────

echo "Step 3/6: Installing userspace components..."
sep

install_file "$SCRIPT_DIR/lib/libbluetooth.so"          "$NP/lib"          "libbluetooth.so"
install_file "$SCRIPT_DIR/servers/bluetooth_server"      "$NP/servers"      "bluetooth_server"
install_file "$SCRIPT_DIR/preferences/Bluetooth"         "$NP/preferences"  "Bluetooth prefs"

if [ -f "$SCRIPT_DIR/bin/BluetoothSendFile" ]; then
    install_file "$SCRIPT_DIR/bin/BluetoothSendFile"     "$NP/bin"          "BluetoothSendFile"
fi

if [ -f "$SCRIPT_DIR/add-ons/BluetoothDeskbarReplicant" ]; then
    install_file "$SCRIPT_DIR/add-ons/BluetoothDeskbarReplicant" "$NP/add-ons" "DeskbarReplicant"

    # Set MIME attributes
    REPLICANT="$NP/add-ons/BluetoothDeskbarReplicant"
    addattr -t "'MSIG'" BEOS:APP_SIG "application/x-vnd.Haiku-BluetoothDeskbar" "$REPLICANT" 2>/dev/null
    settype -t "application/x-vnd.Haiku-BluetoothDeskbar" "$REPLICANT" 2>/dev/null
    mimeset "$REPLICANT" 2>/dev/null
    info "MIME type registered for DeskbarReplicant"
fi
sep

# ── Step 4/6: Install kernel modules ───────────────────────

echo "Step 4/6: Installing kernel modules..."
sep

install_file "$SCRIPT_DIR/kernel/bluetooth/btCoreData"       "$NP/add-ons/kernel/bluetooth"         "btCoreData"
install_file "$SCRIPT_DIR/kernel/bluetooth/hci"              "$NP/add-ons/kernel/bluetooth"         "hci"
install_file "$SCRIPT_DIR/kernel/network/protocols/l2cap"    "$NP/add-ons/kernel/network/protocols" "l2cap"
install_file "$SCRIPT_DIR/kernel/drivers/bluetooth/h2/h2generic" "$NP/add-ons/kernel/drivers/bin"   "h2generic"

install_link "../../../bin/h2generic" "$NP/add-ons/kernel/drivers/dev/bluetooth/h2/h2generic" "h2generic devfs"

# Clean up .bak files from previous installs — critical!
# Haiku's devfs loads ANY file in drivers/bin/ as a driver, so
# h2generic.bak would be loaded as a second driver instance with
# its own firmware retry counter, causing double attempts.
for bak in "$NP/add-ons/kernel/drivers/bin/h2generic.bak" \
           "$NP/add-ons/kernel/bluetooth/btCoreData.bak" \
           "$NP/add-ons/kernel/bluetooth/hci.bak" \
           "$NP/add-ons/kernel/network/protocols/l2cap.bak"; do
    if [ -f "$bak" ]; then
        rm -f "$bak"
        info "Removed stale backup: $(basename "$bak")"
    fi
done

# Clean up h2generic from wrong install path (pre-r007a).
# The driver MUST be in drivers/bin/ with a symlink in drivers/dev/;
# drivers/bluetooth/h2/h2generic/ is NOT loaded by devfs.
STALE_H2="$NP/add-ons/kernel/drivers/bluetooth/h2/h2generic/h2generic"
if [ -f "$STALE_H2" ]; then
    rm -f "$STALE_H2"
    rmdir "$NP/add-ons/kernel/drivers/bluetooth/h2/h2generic" 2>/dev/null
    rmdir "$NP/add-ons/kernel/drivers/bluetooth/h2" 2>/dev/null
    rmdir "$NP/add-ons/kernel/drivers/bluetooth" 2>/dev/null
    info "Removed h2generic from wrong path (drivers/bluetooth/)"
fi
sep

# ── Step 5/6: Install test tools + Intel firmware ──────────

echo "Step 5/6: Installing test tools & firmware..."
sep

# Tests
mkdir -p "$NP/tests"
for test in bt_intel_test bt_sco_test bt_a2dp_source_test bt_avrcp_test bt_hfp_ag_test; do
    if [ -f "$SCRIPT_DIR/tests/$test" ]; then
        install_file "$SCRIPT_DIR/tests/$test" "$NP/tests" "$test"
    fi
done

# Intel firmware auto-download
if [ "$IS_INTEL" = true ]; then
    sep
    info "Intel BT controller detected (8087:$INTEL_PRODUCT)"

    FW_NAME=$(detect_intel_fw_name)
    if [ -n "$FW_NAME" ]; then
        if [ -f "$FW_DIR/${FW_NAME}.sfi" ]; then
            size=$(ls -l "$FW_DIR/${FW_NAME}.sfi" | awk '{print $5}')
            info "Firmware already installed: ${FW_NAME}.sfi ($size bytes)"
        else
            info "Auto-downloading firmware: $FW_NAME"
            download_intel_fw "$FW_NAME"
        fi
    else
        warn "Could not auto-detect Intel firmware name."
        warn "After reboot, run: sh download_intel_fw.sh"
        warn "Or manually: sh download_intel_fw.sh --list"
    fi
fi

# Copy download tool
if [ -f "$SCRIPT_DIR/download_intel_fw.sh" ]; then
    install_file "$SCRIPT_DIR/download_intel_fw.sh" "$NP/bin" "download_intel_fw.sh"
fi
sep

# ── Step 6/6: Post-install checks ─────────────────────────

echo "Step 6/6: Post-install verification..."
sep

ALL_OK=true
for f in "$NP/lib/libbluetooth.so" \
         "$NP/servers/bluetooth_server" \
         "$NP/add-ons/kernel/bluetooth/btCoreData" \
         "$NP/add-ons/kernel/bluetooth/hci" \
         "$NP/add-ons/kernel/network/protocols/l2cap" \
         "$NP/add-ons/kernel/drivers/bin/h2generic"; do
    if [ ! -f "$f" ]; then
        fail "$(basename "$f")" "not found after install"
        ALL_OK=false
    fi
done

if [ "$ALL_OK" = true ]; then
    ok "All critical files verified"
fi

# Intel-specific post-check
if [ "$IS_INTEL" = true ]; then
    sfi_count=$(ls "$FW_DIR"/*.sfi 2>/dev/null | wc -l)
    if [ "$sfi_count" -gt 0 ]; then
        info "Intel firmware: $sfi_count .sfi file(s) ready"
    else
        warn "Intel firmware not yet installed."
        warn "After first reboot, run: sh download_intel_fw.sh"
    fi
fi
sep

# ── Summary ─────────────────────────────────────────────────

echo "======================================"
if [ $ERRORS -eq 0 ]; then
    echo "  SUCCESS: $INSTALLED components installed"
else
    echo "  DONE: $INSTALLED installed, $ERRORS ERRORS"
fi
echo "======================================"
sep
echo "  ** REBOOT required to activate kernel modules **"
sep
echo "After reboot:"
echo "  - Bluetooth icon should appear in the Deskbar tray"
echo "  - Right-click it for: Send file, Settings, Quit"
if [ "$IS_INTEL" = true ]; then
    echo "  - Intel firmware should load automatically on boot"
    echo "  - Check: grep 'Intel firmware' /var/log/syslog"
fi
echo "  - Logs: /tmp/bt_server.log"
echo "  - Diagnostics: sh install.sh --check"
sep
echo "New in $RELEASE:"
echo "  - A2DP Source, AVRCP Target, HFP Audio Gateway, SCO audio"
echo "  - Intel driver: no longer registers failed bootloader devices"
echo "  - Auto Intel firmware download during install"
sep
