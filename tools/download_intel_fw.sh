#!/bin/sh
#
# DenteBlu — Intel Bluetooth firmware downloader
#
# Downloads the correct Intel BT firmware (.sfi + .ddc) files from
# the linux-firmware repository.
#
# Usage:
#   sh download_intel_fw.sh              # auto-detect from syslog
#   sh download_intel_fw.sh ibt-1040-0041  # download specific firmware
#   sh download_intel_fw.sh --list       # show common firmware files
#   sh download_intel_fw.sh --auto       # non-interactive (for install.sh)
#

FW_DIR="/boot/system/non-packaged/data/firmware/intel"
BASE_URL="https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/intel"
MIN_FW_SIZE=10240   # 10KB minimum — smaller files are likely 404 HTML pages
AUTO_MODE=false

# ── helpers ──────────────────────────────────────────────────

info() { echo "  [ii]  $1"; }
ok()   { echo "  [OK]  $1"; }
fail() { echo "  [!!]  $1"; }

# USB product ID → firmware name fallback map
# Used when syslog auto-detect fails (e.g. first boot without firmware)
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
    # validate_download <filepath> <name>
    # Returns 0 if file exists and is > MIN_FW_SIZE, 1 otherwise
    if [ ! -f "$1" ]; then
        return 1
    fi
    size=$(ls -l "$1" | awk '{print $5}')
    if [ "$size" -lt "$MIN_FW_SIZE" ] 2>/dev/null; then
        fail "$2: downloaded file too small ($size bytes) — likely a 404 page"
        rm -f "$1"
        return 1
    fi
    return 0
}

download_fw() {
    name="$1"
    sfi="${name}.sfi"
    ddc="${name}.ddc"

    mkdir -p "$FW_DIR"

    info "Downloading $sfi..."
    wget -q -O "$FW_DIR/$sfi" "$BASE_URL/$sfi" 2>/dev/null
    if [ $? -eq 0 ] && validate_download "$FW_DIR/$sfi" "$sfi"; then
        size=$(ls -l "$FW_DIR/$sfi" | awk '{print $5}')
        ok "$sfi downloaded ($size bytes)"
    else
        rm -f "$FW_DIR/$sfi"
        fail "$sfi not found in linux-firmware"
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

# ── parse arguments ──────────────────────────────────────────

for arg in "$@"; do
    case "$arg" in
        --auto) AUTO_MODE=true ;;
    esac
done

# Filter out --auto for positional args
POSITIONAL=""
for arg in "$@"; do
    case "$arg" in
        --auto) ;;
        *) POSITIONAL="$arg" ;;
    esac
done

# ── list mode ────────────────────────────────────────────────

if [ "$POSITIONAL" = "--list" ]; then
    echo ""
    echo "Common Intel Bluetooth firmware files:"
    echo ""
    echo "  ibt-1040-0041    AX201 (Alder Lake i5-12xxx/i7-12xxx)"
    echo "  ibt-1040-4150    AX201 (Tiger Lake)"
    echo "  ibt-0040-0041    AX201 (variant)"
    echo "  ibt-0040-4150    AX201 (variant)"
    echo "  ibt-0190-0291    AX210/211 (Blazar)"
    echo "  ibt-0190-0291-iml  AX210/211 IML stage"
    echo ""
    echo "To find YOUR firmware name, check syslog after boot:"
    echo "  grep 'loading firmware' /var/log/syslog"
    echo ""
    echo "Or run:  sh download_intel_fw.sh     (auto-detect)"
    echo ""
    exit 0
fi

# ── manual mode ──────────────────────────────────────────────

if [ -n "$POSITIONAL" ] && [ "$POSITIONAL" != "--list" ]; then
    echo ""
    echo "======================================"
    echo "  Intel BT Firmware Downloader"
    echo "======================================"
    echo ""
    download_fw "$POSITIONAL"
    echo ""
    echo "Done. Reboot to load the firmware."
    exit 0
fi

# ── auto-detect mode ─────────────────────────────────────────

if [ "$AUTO_MODE" = false ]; then
    echo ""
    echo "======================================"
    echo "  Intel BT Firmware Downloader"
    echo "======================================"
    echo ""
fi

# Auto-detect from syslog — matches both TLV and legacy formats:
#   "btintel: TLV bootloader, loading firmware: ibt-1040-0041.sfi"
#   "btintel: looking for firmware: ibt-19-0-4.sfi"
FW_NAME=""
DETECT_SOURCE=""

for logfile in /var/log/syslog /tmp/bt_server.log /var/log/syslog.old; do
    if [ -z "$FW_NAME" ] && [ -f "$logfile" ]; then
        FW_NAME=$(grep "btintel:.*firmware:" "$logfile" 2>/dev/null \
            | grep "ibt-" | tail -1 \
            | sed 's/.*firmware: //' | sed 's/\.sfi$//' | tr -d ' ')
        if [ -n "$FW_NAME" ]; then
            DETECT_SOURCE="$(basename "$logfile")"
        fi
    fi
done

# Fallback: USB product ID map
if [ -z "$FW_NAME" ] && command -v listusb >/dev/null 2>&1; then
    PRODUCT_ID=$(listusb 2>/dev/null | grep "8087:" | head -1 \
        | sed 's/.*8087://' | awk '{print $1}' | tr -d ' ')
    if [ -n "$PRODUCT_ID" ]; then
        FW_NAME=$(fw_name_from_usb_product "$PRODUCT_ID")
        if [ -n "$FW_NAME" ]; then
            DETECT_SOURCE="USB product ID 8087:$PRODUCT_ID"
        fi
    fi
fi

if [ -z "$FW_NAME" ]; then
    fail "Could not auto-detect firmware name."
    echo ""
    echo "  No Intel BT device found, or firmware name not recognized."
    echo ""
    echo "  To manually specify the firmware:"
    echo "    sh download_intel_fw.sh ibt-1040-0041"
    echo ""
    echo "  To see common firmware names:"
    echo "    sh download_intel_fw.sh --list"
    echo ""
    exit 1
fi

info "Detected firmware: $FW_NAME (from $DETECT_SOURCE)"
echo ""

# Check if already installed
if [ -f "$FW_DIR/${FW_NAME}.sfi" ]; then
    size=$(ls -l "$FW_DIR/${FW_NAME}.sfi" | awk '{print $5}')
    info "Firmware already installed: ${FW_NAME}.sfi ($size bytes)"
    if [ "$AUTO_MODE" = true ]; then
        info "Keeping existing firmware (--auto mode)"
        exit 0
    fi
    echo ""
    printf "  Re-download? [y/N] "
    read answer
    if [ "$answer" != "y" ] && [ "$answer" != "Y" ]; then
        echo "  Keeping existing firmware."
        exit 0
    fi
fi

download_fw "$FW_NAME"

echo ""
echo "Done. Reboot to load the firmware."
echo ""
