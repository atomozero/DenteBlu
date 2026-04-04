#!/bin/sh
# bt_audio_switch.sh — Switch system audio to Bluetooth
# Usage: bt_audio_switch.sh [on|off]

DENTEBLU=/boot/home/Desktop/denteblu

case "${1:-on}" in
    on)
        echo "Switching audio to Bluetooth..."
        LD_LIBRARY_PATH=/boot/system/non-packaged/lib \
            $DENTEBLU/build/set_bt_output 2>&1
        echo "Done. System audio → Bluetooth."
        ;;
    off)
        echo "Switching audio back to HD Audio..."
        # Restart media to reset to default (HD Audio)
        kill $(ps | grep media_addon_server | grep -v grep | awk '{print $2}') 2>/dev/null
        kill $(ps | grep media_server | grep -v grep | awk '{print $2}') 2>/dev/null
        echo "Done. System audio → HD Audio (internal speakers)."
        ;;
    *)
        echo "Usage: $0 [on|off]"
        ;;
esac
