#!/bin/bash
# Coverage test runner for DenteBlu Bluetooth unit tests
# Compiles unit tests with --coverage, runs them, collects gcov output.
#
# Usage: ./run_coverage.sh [--html]   (--html requires lcov)

set -e

SRC="/Magazzino/DenteBlu/haiku"
TESTS="$SRC/src/tests/kits/bluetooth"
SMP="$TESTS/smp_crypto"
KIT="$SRC/src/kits/bluetooth"
SERVER="$SRC/src/servers/bluetooth"
COVDIR="$TESTS/coverage_build"

# Include paths (our source tree overrides system headers)
INCS=(
    -I"$SRC/headers/os"
    -I"$SRC/headers/os/bluetooth"
    -I"$SRC/headers/os/bluetooth/HCI"
    -I"$SRC/headers/os/bluetooth/L2CAP"
    -I"$SRC/headers/private"
    -I"$SRC/headers/private/bluetooth"
    -I"$SRC/headers/private/shared"
    -I"$SRC/headers/private/net"
    -I"$SRC/headers/private/kernel"
    -I"$SERVER"
    -I"$SMP"
)

CXX="g++"
CXXFLAGS="-g -O0 --coverage -std=c++17 -D_BSD_SOURCE"
LDFLAGS="--coverage -lbe"

PASS=0
FAIL=0
SKIP=0
TOTAL=0

rm -rf "$COVDIR"
mkdir -p "$COVDIR"

build_and_run() {
    local name="$1"
    shift
    local sources=("$@")

    TOTAL=$((TOTAL + 1))
    printf "  %-28s " "$name"

    # Compile
    if ! $CXX $CXXFLAGS "${INCS[@]}" -o "$COVDIR/$name" "${sources[@]}" $LDFLAGS 2>"$COVDIR/${name}_build.log"; then
        echo "BUILD FAIL"
        cat "$COVDIR/${name}_build.log" | head -5
        SKIP=$((SKIP + 1))
        return
    fi

    # Run (pass any extra args after --)
    local run_args="${RUN_ARGS:-}"
    if "$COVDIR/$name" $run_args >"$COVDIR/${name}_output.log" 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        local rc=$?
        echo "FAIL (exit=$rc)"
        FAIL=$((FAIL + 1))
    fi
}

echo "============================================"
echo "  DenteBlu Unit Test Coverage Runner"
echo "============================================"
echo ""
echo "Building and running unit tests with --coverage..."
echo ""

# --- Pure unit tests (no BT hardware, no server) ---

# 1. RFCOMM FCS
build_and_run "bt_rfcomm_fcs_test" \
    "$TESTS/bt_rfcomm_fcs_test.cpp"

# 2. HCI LE command structs
build_and_run "bt_hci_le_cmd_test" \
    "$TESTS/bt_hci_le_cmd_test.cpp"

# 3. KeyStore
build_and_run "bt_keystore_test" \
    "$TESTS/bt_keystore_test.cpp" \
    "$SERVER/BluetoothKeyStore.cpp"

# 4. LTK persistence
build_and_run "bt_ltk_persist_test" \
    "$TESTS/bt_ltk_persist_test.cpp" \
    "$SERVER/BluetoothKeyStore.cpp"

# 5. Key E2E (needs --add-test argument)
RUN_ARGS="--add-test" build_and_run "bt_key_e2e_test" \
    "$TESTS/bt_key_e2e_test.cpp" \
    "$SERVER/BluetoothKeyStore.cpp"

# 6. SMP crypto (AES, CMAC, f4/f5/f6/g2)
build_and_run "test_smp_crypto" \
    "$SMP/test_smp_crypto.cpp" \
    "$SMP/smp_crypto.cpp" \
    "$SMP/p256.cpp"

# 7. SMP pairing simulation
build_and_run "test_smp_pairing" \
    "$SMP/test_smp_pairing.cpp" \
    "$SMP/smp_crypto.cpp" \
    "$SMP/p256.cpp"

# 8. P-256 ECDH
build_and_run "test_p256" \
    "$SMP/test_p256.cpp" \
    "$SMP/p256.cpp"

echo ""
echo "============================================"
echo "  Results: $PASS pass, $FAIL fail, $SKIP skip (of $TOTAL)"
echo "============================================"
echo ""

if [ $PASS -eq 0 ]; then
    echo "No tests passed, skipping coverage collection."
    exit 1
fi

# --- Collect gcov ---
echo "Collecting gcov coverage data..."
echo ""

cd "$COVDIR"

# Run gcov on all .gcno files (suppress per-line output)
gcov -b *.gcno 2>/dev/null | grep -E "^(File|Lines|Branches)" | head -80

echo ""

# Summary: list covered source files with line %
echo "--- Per-file coverage summary ---"
echo ""
printf "%-50s %8s %8s %6s\n" "Source File" "Lines" "Exec" "Cov%"
printf "%-50s %8s %8s %6s\n" "----------" "-----" "----" "----"

for gcov_file in *.gcov; do
    [ -f "$gcov_file" ] || continue
    src=$(head -1 "$gcov_file" | sed 's/.*Source://' | sed 's/^ *//')
    # Skip system headers
    case "$src" in
        /boot/system/*) continue ;;
        /Magazzino/*)   ;;
        *)              continue ;;
    esac
    exec_lines=$(grep -c "^[[:space:]]*[0-9]" "$gcov_file" 2>/dev/null || true)
    not_exec=$(grep -c "^[[:space:]]*#####" "$gcov_file" 2>/dev/null || true)
    exec_lines=${exec_lines:-0}
    not_exec=${not_exec:-0}
    coverable=$((exec_lines + not_exec))
    if [ "$coverable" -gt 0 ]; then
        pct=$((exec_lines * 100 / coverable))
        # Shorten path
        short=$(echo "$src" | sed "s|$SRC/||")
        printf "%-50s %8d %8d %5d%%\n" "$short" "$coverable" "$exec_lines" "$pct"
    fi
done | sort -t% -k4 -rn

echo ""

# HTML report if lcov available and --html requested
if [ "$1" = "--html" ]; then
    if command -v lcov >/dev/null 2>&1; then
        echo "Generating HTML coverage report..."
        lcov --capture --directory . --output-file coverage.info --quiet
        lcov --remove coverage.info '/boot/system/*' --output-file coverage.info --quiet
        genhtml coverage.info --output-directory html --quiet
        echo "HTML report: $COVDIR/html/index.html"
    else
        echo "lcov not installed. Install with: pkgman install lcov"
    fi
fi

echo "Coverage data in: $COVDIR/"
echo "Raw test output:  $COVDIR/*_output.log"
