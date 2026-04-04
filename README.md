# DenteBlu

Complete Bluetooth stack (Classic BR/EDR + BLE) for Haiku OS, from USB driver to application profiles.

## Status

**A2DP audio streaming is working.** MP3/WAV files play on Bluetooth speakers (tested with Google Home) via the `bt_a2dp_play` command. The SBC encoder uses the reference libsbc implementation with proper RTP pacing.

The stack works with USB Bluetooth 2.0 dongles (BCM2045, BCM2070, CSR chipsets) on Haiku R1 beta5 (hrev59506+). Classic inquiry, device discovery, A2DP Source, and SSP pairing are operational.

For **Intel AX201** chips (USB 8087:0026), the firmware loader completes the full two-phase download. Integration with bluetooth_server is in progress.

## A2DP Audio — Quick Start

```sh
# 1. Start the Bluetooth server
/boot/system/non-packaged/servers/bluetooth_server &

# 2. Find your speaker's BD address (put it in pairing mode first)
LD_LIBRARY_PATH=/boot/system/non-packaged/lib bt_scan
# Check /tmp/bt_server.log for: InquiryResult: device XX:XX:XX:XX:XX:XX

# 3. Play audio to the Bluetooth speaker
LD_LIBRARY_PATH=/boot/system/non-packaged/lib \
  bt_a2dp_play XX:XX:XX:XX:XX:XX /path/to/music.mp3

# 4. Or send a test tone (440Hz, 10 seconds)
LD_LIBRARY_PATH=/boot/system/non-packaged/lib \
  bt_a2dp_source_test XX:XX:XX:XX:XX:XX --tone
```

Replace `XX:XX:XX:XX:XX:XX` with the BD address found in step 2.

Features:
- SBC encoding via libsbc (BlueZ reference implementation)
- Automatic sample rate negotiation (44100/48000 Hz)
- Resampling for files at non-standard rates (e.g. 11025 → 44100 Hz)
- Auto-gain normalization for quiet/8-bit audio files
- Uniform RTP pacing (~11ms per packet, 4 SBC frames)

## Architecture

```
Applications / Preferences GUI / bt_a2dp_play
        │ BMessage IPC
   libbluetooth.so  (A2DP, RFCOMM, OBEX, AVDTP, SBC via libsbc)
        │ ioctl on /dev/bluetooth/h2/0
   bluetooth_server  (LocalDeviceImpl, SDP server, KeyStore)
        │ ioctl
   Kernel: hci + btCoreData + l2cap + h2generic (USB driver)
```

Four layers, ~60,000 lines of C++ across 252 source files.

## Building

### With build.sh (recommended, no Jam required)

```sh
./build.sh all      # Compiles everything: lib, server, prefs, media, kernel, 35 tests
./build.sh lib      # libbluetooth.so only
./build.sh server   # bluetooth_server
./build.sh prefs    # Bluetooth preferences app
./build.sh media    # bluetooth_audio media add-on
./build.sh kernel   # btCoreData + l2cap kernel modules
./build.sh tests    # 35 test binaries
./build.sh clean    # Remove build/
```

Requires: `g++`, `libsbc` (`pkgman install sbc sbc_devel`).

### With Jam (Haiku source tree)

Requires a full Haiku source checkout with DenteBlu files copied to matching paths.

```sh
jam h2generic        # USB driver + Intel firmware loader
jam bluetooth_server # server
jam libbluetooth.so  # kit library
jam l2cap            # L2CAP kernel protocol
jam btCoreData       # core data kernel module
```

## Installation

```sh
# Userspace (replaceable at runtime)
cp bluetooth_server  /boot/system/non-packaged/servers/
cp libbluetooth.so   /boot/system/non-packaged/lib/
cp Bluetooth         /boot/system/non-packaged/preferences/

# Media add-on (restart media_server to detect)
cp bluetooth_audio.media_addon /boot/system/non-packaged/add-ons/media/

# Kernel (requires reboot)
cp btCoreData  /boot/system/non-packaged/add-ons/kernel/bluetooth/
cp l2cap       /boot/system/non-packaged/add-ons/kernel/network/protocols/
cp h2generic   /boot/system/non-packaged/add-ons/kernel/drivers/bin/
ln -sf ../../../bin/h2generic \
  /boot/system/non-packaged/add-ons/kernel/drivers/dev/bluetooth/h2/h2generic
```

h2generic **must** go in `drivers/bin/` with a symlink in `drivers/dev/bluetooth/h2/`. Haiku devfs only loads drivers from `drivers/bin/`.

## Implemented profiles

| Profile | Status |
|---------|--------|
| **A2DP Source** | **Working** — audio streams to BT speakers/headphones |
| SPP (Serial Port) | Working |
| PBAP (Phone Book) | Implemented |
| OPP (Object Push) | Implemented |
| A2DP Sink | Implemented, untested |
| HFP (Hands-Free) | Implemented, untested |
| AVRCP (Remote Control) | Implemented, not wired to media |
| SCO/eSCO (Sync audio) | USB endpoints ready, streaming TBD |
| ATT/GATT (BLE) | Implemented |

## Intel firmware loader

The `btintel.cpp` module handles Intel chips requiring firmware download:

1. Read Intel Version (0xFC05) — detects bootloader mode (`fw_variant=0x06`)
2. Read Boot Params (0xFC0D), load `.sfi` from `/boot/system/non-packaged/data/firmware/intel/`
3. Download via Secure Send (0xFC09) on bulk endpoint
4. Intel Reset (0xFC01) — chip reboots USB, retry Read Version (3 attempts)
5. Drain boot events, Read Version → `fw_variant=0x23` (operational)
6. DDC loading (0xFC8B), Intel Event Mask (0xFC52)

Firmware from [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/intel) (e.g. `ibt-19-0-4.sfi`, `.ddc`).

## Media add-on (bluetooth_audio)

The `bluetooth_audio.media_addon` integrates with Haiku's media kit as a `BBufferConsumer`. It appears as "Bluetooth Audio Output" in Media Preferences.

**Current status:** The node instantiates and registers correctly in the media_addon_server. A2DP connection and StartStream succeed. Audio routing from the mixer to the node is under development — see `ROADMAP.md` for details.

## Preferences app

The Bluetooth preferences app provides:
- Automatic device scanning (classic inquiry)
- Device list with name and type (2-line compact view)
- Pair button with SSP support
- Service query, terminal, file send

## Known issues

- Media add-on: mixer does not forward buffers to BT node (routing issue under investigation)
- Tiger Lake/Alder Lake kernel bug in `smp.cpp` causes KDL during XHCI activity
- Android won't open incoming RFCOMM (needs SC P-256; BCM2070 only has P-192)
- L2CAP PSM leak if process is SIGKILLed (workaround: reboot)

## Fixed issues (April 2026)

- **A2DP audio mute** — SBC encoder replaced with libsbc reference; RTP media header frame count fixed (bits 3-0, not 7-4); pacing fixed to uniform ~11ms per packet
- **Use-after-free in btCoreData** — RemoveConnection/AddConnection race; ConnectionByHandle freed memory access; delete under lock deadlock resolved
- **L2capEndpoint::Free() race** — reordered to read fConnection before unbinding from channel maps
- **Media add-on crash** — InitParameterWeb called before Run(); removed redundant HandleMessage dispatch; fixed GetNextInput stale source
- **StopStream crash** — robust socket checks in AvdtpSession Suspend/Close/_RecvSignal
- **Server buffer overflow** — bounds check in InquiryResult handlers
- **Incoming connection filter** — reject connections from unknown devices

## Other documentation

- `ROADMAP.md` — prioritized plan of remaining work
- `CLAUDE.md` — developer guide for the codebase
- `DEVELOPER_PROMPT.md` — comprehensive developer handbook (Italian)
- `STATO-PROGETTO.md` — project status (Italian)

## License

MIT
