#!/bin/bash
# Build script per DenteBlu — compila lo stack BT userspace senza Jam
# Uso: ./build.sh [all|lib|server|prefs|media|tests|clean]

set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/haiku/src"
HDR="$ROOT/haiku/headers"
BUILD="$ROOT/build"

# Include paths
CFLAGS="-std=c++17 -Wall -Werror -fPIC -O2 \
    -Wno-address-of-packed-member -Wno-class-memaccess"
INCLUDES=(
    -I"$HDR/os"
    -I"$HDR/os/bluetooth"
    -I"$HDR/os/bluetooth/HCI"
    -I"$HDR/os/bluetooth/L2CAP"
    -I"$HDR/private"
    -I"$HDR/private/bluetooth"
    -I/boot/system/develop/headers
    -I/boot/system/develop/headers/os
    -I/boot/system/develop/headers/os/support
    -I/boot/system/develop/headers/os/app
    -I/boot/system/develop/headers/os/interface
    -I/boot/system/develop/headers/os/storage
    -I/boot/system/develop/headers/os/media
    -I/boot/system/develop/headers/private
    -I/boot/system/develop/headers/private/shared
    -I/boot/system/develop/headers/private/bluetooth
    -I/boot/system/develop/headers/private/net
)

OK=0
FAIL=0
FAILURES=""

pass() { OK=$((OK + 1)); echo "  ✓ $1"; }
fail() { FAIL=$((FAIL + 1)); FAILURES="$FAILURES\n  ✗ $1"; echo "  ✗ $1"; }

mkdir -p "$BUILD/obj/lib" "$BUILD/obj/server" "$BUILD/obj/prefs" \
         "$BUILD/obj/media" "$BUILD/obj/tests"

# ═══════════════════════════════════════════════════════════════
# libbluetooth.so
# ═══════════════════════════════════════════════════════════════
build_lib() {
    echo "══ libbluetooth.so ══"
    local LIB_SRC="$SRC/kits/bluetooth"
    local LIB_SRCS=(
        LocalDevice.cpp DiscoveryListener.cpp DiscoveryAgent.cpp
        RemoteDevice.cpp CommandManager.cpp KitSupport.cpp
        DeviceClass.cpp BleDevice.cpp NusClient.cpp
        RfcommSession.cpp SppClient.cpp ObexClient.cpp
        OppClient.cpp PbapClient.cpp VCardParser.cpp
        PeopleWriter.cpp HfpClient.cpp HfpAudioGateway.cpp
        AtParser.cpp A2dpSink.cpp A2dpSource.cpp
        AvrcpTarget.cpp AvdtpSession.cpp SbcDecoder.cpp
        ScoSocket.cpp SbcEncoder.cpp
    )
    local UI_SRCS=(
        PincodeWindow.cpp NumericComparisonWindow.cpp
        ConnectionIncoming.cpp ConnectionView.cpp
        BluetoothIconView.cpp
    )

    local OBJS=()
    for f in "${LIB_SRCS[@]}"; do
        local obj="$BUILD/obj/lib/${f%.cpp}.o"
        if g++ $CFLAGS "${INCLUDES[@]}" -I"$LIB_SRC/UI" \
           -c "$LIB_SRC/$f" -o "$obj" 2>&1; then
            pass "$f"
        else
            fail "$f"
        fi
        OBJS+=("$obj")
    done
    for f in "${UI_SRCS[@]}"; do
        local obj="$BUILD/obj/lib/UI_${f%.cpp}.o"
        if g++ $CFLAGS "${INCLUDES[@]}" -I"$LIB_SRC/UI" \
           -c "$LIB_SRC/UI/$f" -o "$obj" 2>&1; then
            pass "UI/$f"
        else
            fail "UI/$f"
        fi
        OBJS+=("$obj")
    done

    echo "  → linking libbluetooth.so"
    if g++ -shared -o "$BUILD/libbluetooth.so" "${OBJS[@]}" \
       -lbe -lnetwork -llocalestub 2>&1; then
        pass "libbluetooth.so linked"
    else
        fail "libbluetooth.so link"
    fi
}

# ═══════════════════════════════════════════════════════════════
# bluetooth_server
# ═══════════════════════════════════════════════════════════════
build_server() {
    echo "══ bluetooth_server ══"
    local SRV_SRC="$SRC/servers/bluetooth"
    local SRV_SRCS=(
        BluetoothKeyStore.cpp BluetoothServer.cpp
        DeskbarReplicant.cpp DeviceManager.cpp
        HCIControllerAccessor.cpp HCITransportAccessor.cpp
        LocalDeviceHandler.cpp LocalDeviceImpl.cpp
        SdpServer.cpp
    )

    local OBJS=()
    for f in "${SRV_SRCS[@]}"; do
        local obj="$BUILD/obj/server/${f%.cpp}.o"
        if g++ $CFLAGS "${INCLUDES[@]}" \
           -c "$SRV_SRC/$f" -o "$obj" 2>&1; then
            pass "$f"
        else
            fail "$f"
        fi
        OBJS+=("$obj")
    done

    echo "  → linking bluetooth_server"
    if g++ -o "$BUILD/bluetooth_server" "${OBJS[@]}" \
       -L"$BUILD" -lbe -lnetwork -lbluetooth -llocalestub 2>&1; then
        pass "bluetooth_server linked"
    else
        fail "bluetooth_server link"
    fi
}

# ═══════════════════════════════════════════════════════════════
# Bluetooth preferences
# ═══════════════════════════════════════════════════════════════
build_prefs() {
    echo "══ Bluetooth (preferences) ══"
    local PREF_SRC="$SRC/preferences/bluetooth"
    local PREF_SRCS=(
        BluetoothDeviceView.cpp BluetoothMain.cpp
        BluetoothSettings.cpp BluetoothSettingsView.cpp
        BluetoothWindow.cpp DeviceListItem.cpp
        ExtendedLocalDeviceView.cpp InquiryPanel.cpp
        RemoteDevicesView.cpp SdpServicesWindow.cpp
        SppTerminalWindow.cpp PbapBrowserWindow.cpp
        OppTransferWindow.cpp HfpCallWindow.cpp
        A2dpPlayerWindow.cpp
    )

    local OBJS=()
    for f in "${PREF_SRCS[@]}"; do
        local obj="$BUILD/obj/prefs/${f%.cpp}.o"
        if g++ $CFLAGS "${INCLUDES[@]}" \
           -c "$PREF_SRC/$f" -o "$obj" 2>&1; then
            pass "$f"
        else
            fail "$f"
        fi
        OBJS+=("$obj")
    done

    echo "  → linking Bluetooth"
    if g++ -o "$BUILD/Bluetooth" "${OBJS[@]}" \
       -L"$BUILD" -lbe -lshared -lnetwork -lmedia -ltracker -lbluetooth -llocalestub 2>&1; then
        pass "Bluetooth linked"
    else
        fail "Bluetooth link"
    fi
}

# ═══════════════════════════════════════════════════════════════
# bluetooth_audio (media add-on)
# ═══════════════════════════════════════════════════════════════
build_media() {
    echo "══ bluetooth_audio ══"
    local MEDIA_SRC="$SRC/add-ons/media/bluetooth_audio"
    local MEDIA_SRCS=(
        BluetoothAudioAddOn.cpp
        BluetoothAudioNode.cpp
    )

    local OBJS=()
    for f in "${MEDIA_SRCS[@]}"; do
        local obj="$BUILD/obj/media/${f%.cpp}.o"
        if g++ $CFLAGS "${INCLUDES[@]}" -DBUILDING_MEDIA_ADDON \
           -c "$MEDIA_SRC/$f" -o "$obj" 2>&1; then
            pass "$f"
        else
            fail "$f"
        fi
        OBJS+=("$obj")
    done

    echo "  → linking bluetooth_audio"
    if g++ -shared -o "$BUILD/bluetooth_audio" "${OBJS[@]}" \
       -L"$BUILD" -lbe -lmedia -lbluetooth 2>&1; then
        pass "bluetooth_audio linked"
    else
        fail "bluetooth_audio link"
    fi
}

# ═══════════════════════════════════════════════════════════════
# Kernel modules (btCoreData + l2cap)
# ═══════════════════════════════════════════════════════════════
build_kernel() {
    echo "══ kernel modules ══"

    local KFLAGS="-std=c++17 -Wall -Werror -fPIC -O2 \
        -Wno-address-of-packed-member -Wno-class-memaccess \
        -fno-exceptions -fno-rtti -D_KERNEL_MODE=1"
    local KINC=(
        -include "$BUILD/kernel_fwd.h"
        -I"$BUILD"
        -I"$HDR/os"
        -I"$HDR/os/bluetooth"
        -I"$HDR/os/bluetooth/HCI"
        -I"$HDR/os/bluetooth/L2CAP"
        -I"$HDR/private"
        -I"$HDR/private/bluetooth"
        -I/boot/system/develop/headers
        -I/boot/system/develop/headers/os
        -I/boot/system/develop/headers/private
        -I/boot/system/develop/headers/private/shared
        -I/boot/system/develop/headers/private/net
        -I/boot/system/develop/headers/private/kernel
        -I/boot/system/develop/headers/private/kernel/util
        -I/boot/system/develop/headers/private/kernel/arch/x86
    )

    # Generate support files if needed
    if [ ! -f "$BUILD/kernel_debug_config.h" ]; then
        cat > "$BUILD/kernel_debug_config.h" << 'KEOF'
#ifndef KERNEL_DEBUG_CONFIG_H
#define KERNEL_DEBUG_CONFIG_H
#define KDEBUG_LEVEL_2  0
#define KDEBUG_LEVEL_1  0
#define KDEBUG_LEVEL_0  1
#define B_DEBUG_SPINLOCK_CONTENTION 0
#endif
KEOF
    fi
    if [ ! -f "$BUILD/kernel_fwd.h" ]; then
        cat > "$BUILD/kernel_fwd.h" << 'KEOF'
#ifndef KERNEL_FWD_H
#define KERNEL_FWD_H
struct Thread;
#endif
KEOF
    fi
    if [ ! -f "$BUILD/obj/dso_handle.o" ]; then
        echo 'void *__dso_handle __attribute__((visibility("hidden"))) = &__dso_handle;' \
            > "$BUILD/dso_handle.c"
        g++ -fPIC -c "$BUILD/dso_handle.c" -o "$BUILD/obj/dso_handle.o"
    fi

    # btCoreData
    local BTC_SRC="$SRC/add-ons/kernel/bluetooth/btCoreData"
    local BTC_OBJS=()
    for f in ConnectionInterface.cpp BTCoreData.cpp; do
        local obj="$BUILD/obj/kern_${f%.cpp}.o"
        if g++ $KFLAGS "${KINC[@]}" \
           -c "$BTC_SRC/$f" -o "$obj" 2>&1; then
            pass "btCoreData/$f"
        else
            fail "btCoreData/$f"
        fi
        BTC_OBJS+=("$obj")
    done

    echo "  → linking btCoreData"
    if g++ -shared -nostdlib -o "$BUILD/btCoreData" \
       "${BTC_OBJS[@]}" "$BUILD/obj/dso_handle.o" -lgcc 2>&1; then
        pass "btCoreData linked"
    else
        fail "btCoreData link"
    fi

    # l2cap
    local L2_SRC="$SRC/add-ons/kernel/network/protocols/l2cap"
    local L2_OBJS=()
    local L2_FILES=(l2cap.cpp l2cap_address.cpp l2cap_command.cpp l2cap_le.cpp l2cap_signal.cpp L2capEndpoint.cpp L2capEndpointManager.cpp)
    for f in "${L2_FILES[@]}"; do
        local obj="$BUILD/obj/kern_${f%.cpp}.o"
        if g++ $KFLAGS "${KINC[@]}" \
           -c "$L2_SRC/$f" -o "$obj" 2>&1; then
            pass "l2cap/$f"
        else
            fail "l2cap/$f"
        fi
        L2_OBJS+=("$obj")
    done

    echo "  → linking l2cap"
    if g++ -shared -nostdlib -o "$BUILD/l2cap" \
       "${L2_OBJS[@]}" "$BUILD/obj/dso_handle.o" -lgcc 2>&1; then
        pass "l2cap linked"
    else
        fail "l2cap link"
    fi
}

# ═══════════════════════════════════════════════════════════════
# Tests
# ═══════════════════════════════════════════════════════════════
build_tests() {
    echo "══ tests ══"
    local TEST_SRC="$SRC/tests/kits/bluetooth"
    local KIT_SRC="$SRC/kits/bluetooth"
    local SRV_SRC="$SRC/servers/bluetooth"

    local TEST_INCLUDES=(
        "${INCLUDES[@]}"
        -I"$KIT_SRC"
        -I"$SRV_SRC"
        -I/boot/system/develop/headers/private/kernel
    )

    # Simple tests that link libbluetooth.so
    local SIMPLE_BT_TESTS=(
        bt_spp_test bt_pbap_test bt_opp_test bt_hfp_test
        bt_a2dp_test bt_a2dp_source_test bt_hfp_ag_test
        bt_avrcp_test bt_sco_test bt_ertm_test bt_pair_test
        bt_psm_leak_test bt_reconnect_test bt_query
        bt_sdp_query bt_contact_sync_daemon bt_sbc_test
        bt_sbc_enc_test bt_at_parser_test ble_scan ble_connect
        ble_pair BluetoothTerminal
    )

    for t in "${SIMPLE_BT_TESTS[@]}"; do
        local src="$TEST_SRC/${t}.cpp"
        if [ ! -f "$src" ]; then
            fail "$t (source not found)"
            continue
        fi
        # bt_a2dp_test uses BSoundPlayer → needs -lmedia
        local extra_libs=""
        if [ "$t" = "bt_a2dp_test" ]; then
            extra_libs="-lmedia"
        fi
        if g++ $CFLAGS "${TEST_INCLUDES[@]}" \
           "$src" -o "$BUILD/$t" \
           -L"$BUILD" -lbe -lbluetooth -lnetwork $extra_libs 2>&1; then
            pass "$t"
        else
            fail "$t"
        fi
    done

    # Tests that compile server sources directly
    echo "  -- tests con sorgenti server --"
    for t in bt_keystore_test bt_ltk_persist_test bt_key_e2e_test bt_keystore_clean; do
        local src="$TEST_SRC/${t}.cpp"
        if [ ! -f "$src" ]; then
            fail "$t (source not found)"
            continue
        fi
        if g++ $CFLAGS "${TEST_INCLUDES[@]}" \
           "$src" "$SRV_SRC/BluetoothKeyStore.cpp" \
           -o "$BUILD/$t" -L"$BUILD" -lbe 2>&1; then
            pass "$t"
        else
            fail "$t"
        fi
    done

    # bt_server_key_test needs more server sources
    if g++ $CFLAGS "${TEST_INCLUDES[@]}" \
       "$TEST_SRC/bt_server_key_test.cpp" \
       "$SRV_SRC/BluetoothKeyStore.cpp" \
       "$SRV_SRC/LocalDeviceImpl.cpp" \
       "$SRV_SRC/LocalDeviceHandler.cpp" \
       -o "$BUILD/bt_server_key_test" \
       -L"$BUILD" -lbe -lbluetooth 2>&1; then
        pass "bt_server_key_test"
    else
        fail "bt_server_key_test"
    fi

    # Standalone tests (no libbluetooth needed)
    # bt_hci_le_cmd_test skipped: references undefined structs
    # (hci_rp_le_ltk_request_reply) not yet in our headers
    for t in bt_rfcomm_fcs_test bt_intel_test; do
        local src="$TEST_SRC/${t}.cpp"
        if [ ! -f "$src" ]; then
            fail "$t (source not found)"
            continue
        fi
        if g++ $CFLAGS "${TEST_INCLUDES[@]}" \
           "$src" -o "$BUILD/$t" -lbe 2>&1; then
            pass "$t"
        else
            fail "$t"
        fi
    done

    # bt_media_addon_test
    if g++ $CFLAGS "${TEST_INCLUDES[@]}" \
       "$TEST_SRC/bt_media_addon_test.cpp" \
       -o "$BUILD/bt_media_addon_test" -lbe -lmedia 2>&1; then
        pass "bt_media_addon_test"
    else
        fail "bt_media_addon_test"
    fi

    # bt_contact_sync needs tracker
    if g++ $CFLAGS "${TEST_INCLUDES[@]}" \
       "$TEST_SRC/bt_contact_sync.cpp" \
       -o "$BUILD/bt_contact_sync" \
       -L"$BUILD" -lbe -lbluetooth -lnetwork -ltracker 2>&1; then
        pass "bt_contact_sync"
    else
        fail "bt_contact_sync"
    fi

    # BluetoothSendFile needs tracker
    if g++ $CFLAGS "${TEST_INCLUDES[@]}" \
       "$TEST_SRC/BluetoothSendFile.cpp" \
       -o "$BUILD/BluetoothSendFile" \
       -L"$BUILD" -lbe -lbluetooth -lnetwork -ltracker 2>&1; then
        pass "BluetoothSendFile"
    else
        fail "BluetoothSendFile"
    fi

    # vcf2people
    if g++ $CFLAGS "${TEST_INCLUDES[@]}" \
       "$TEST_SRC/vcf2people.cpp" \
       -o "$BUILD/vcf2people" \
       -L"$BUILD" -lbe -lbluetooth 2>&1; then
        pass "vcf2people"
    else
        fail "vcf2people"
    fi
}

# ═══════════════════════════════════════════════════════════════
# Clean
# ═══════════════════════════════════════════════════════════════
do_clean() {
    echo "Cleaning build/"
    rm -rf "$BUILD"
    echo "Done."
    exit 0
}

# ═══════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════
TARGET="${1:-all}"

case "$TARGET" in
    clean) do_clean ;;
    lib)    build_lib ;;
    server) build_lib; build_server ;;
    prefs)  build_lib; build_prefs ;;
    media)  build_lib; build_media ;;
    kernel) build_kernel ;;
    tests)  build_lib; build_tests ;;
    all)
        build_lib
        build_server
        build_prefs
        build_media
        build_kernel
        build_tests
        ;;
    *) echo "Usage: $0 [all|lib|server|prefs|media|kernel|tests|clean]"; exit 1 ;;
esac

echo ""
echo "════════════════════════════════════"
echo "  Risultato: $OK ok, $FAIL errori"
if [ $FAIL -gt 0 ]; then
    echo -e "  Falliti:$FAILURES"
fi
echo "════════════════════════════════════"
echo "  Output: $BUILD/"

exit $FAIL
