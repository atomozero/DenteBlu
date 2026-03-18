# DenteBlu

Complete Bluetooth stack (Classic BR/EDR + BLE) for Haiku OS, from USB driver to application profiles.

## Status

The stack works with USB Bluetooth 2.0 dongles (BCM2045, CSR chipsets, etc.) on Haiku R1 beta5. Pairing, SDP discovery, and SPP/RFCOMM profiles are operational.

For **Intel AX201** chips (USB 8087:0026), the firmware loader is implemented and completes the full download (801KB, 3233 fragments via Secure Send). The chip reboots with operational firmware. Integration with bluetooth_server is in progress — see [Intel firmware loader](#intel-firmware-loader).

## Architecture

```
Applications / Preferences GUI
        │ BMessage IPC
   libbluetooth.so  (profiles, RFCOMM, OBEX, AVDTP, SBC codec)
        │ ioctl on /dev/bluetooth/h2/0
   bluetooth_server  (LocalDeviceImpl, SDP server, KeyStore)
        │ ioctl
   Kernel: hci + btCoreData + l2cap + h2generic (USB driver)
```

Four layers, ~60,000 lines of C++ across 252 source files.

## Building

Requires a Haiku source checkout. Copy DenteBlu files to the matching paths in the Haiku source tree. Build system is Jam.

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

# Kernel (requires reboot)
cp h2generic   /boot/system/non-packaged/add-ons/kernel/drivers/bin/
ln -sf ../../../bin/h2generic \
  /boot/system/non-packaged/add-ons/kernel/drivers/dev/bluetooth/h2/h2generic
cp btCoreData  /boot/system/non-packaged/add-ons/kernel/bluetooth/
cp hci         /boot/system/non-packaged/add-ons/kernel/bluetooth/
cp l2cap       /boot/system/non-packaged/add-ons/kernel/network/protocols/
```

h2generic **must** go in `drivers/bin/` with a symlink in `drivers/dev/bluetooth/h2/`. Haiku devfs only loads drivers from `drivers/bin/`.

## Intel firmware loader

The `btintel.cpp` module handles Intel chips that require firmware download over USB:

1. Read Intel Version (0xFC05) — detects bootloader mode (`fw_variant=0x06`)
2. Read Boot Params (0xFC0D), load `.sfi` from `/boot/system/non-packaged/data/firmware/intel/`
3. Download via Secure Send (0xFC09) on bulk endpoint
4. Intel Reset (0xFC01) — chip reboots USB
5. Drain boot events, Read Version → `fw_variant=0x23` (operational)
6. DDC loading (0xFC8B), Intel Event Mask (0xFC52)

Firmware files are available from the [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/intel) repository (e.g. `ibt-19-0-4.sfi`, `ibt-19-0-4.ddc`).

**Note:** Tiger Lake/Alder Lake machines hit a kernel bug in `smp.cpp` that causes KDL during heavy XHCI activity. See `smp-fix-ticket.md` for the analysis and patch.

## Implemented profiles

| Profile | Status |
|---------|--------|
| SPP (Serial Port) | Working |
| PBAP (Phone Book) | Implemented |
| OPP (Object Push) | Implemented |
| A2DP (Audio) | Implemented, untested on HW |
| HFP (Hands-Free) | Implemented, untested on HW |
| AVRCP (Remote Control) | Implemented, untested on HW |
| SCO/eSCO (Sync audio) | USB endpoints ready, streaming TBD |
| ATT/GATT (BLE) | Implemented |

## Comparison with upstream Haiku (March 2026)

Haiku's built-in Bluetooth stack only covers a partially-working L2CAP (recently rewritten by waddlesplash) and an incomplete SDP userland. There are no profiles, no BLE, and no working SSP pairing on modern devices. Upstream work is currently focused on basic HCI event handling patches.

DenteBlu provides all of the above plus: full HCI with Intel firmware loading, 10 SDP service records, RFCOMM, ATT/GATT, SMP (P-192 + P-256), and seven application profiles (SPP, PBAP, OPP, A2DP Source/Sink, AVRCP, HFP Client/AG), a Media Kit audio add-on, SCO audio support, BLE/NUS client, and a preferences GUI with Deskbar replicant.

See `TODO.md` for features still missing toward full BT spec coverage.

## Known issues

- Kernel bug in `smp.cpp` on Tiger Lake causes KDL during USB reconnection after Intel firmware load — see `smp-fix-ticket.md`
- Android won't open incoming RFCOMM to Haiku (likely needs Secure Connections P-256; BCM2070 only has P-192)
- L2CAP PSM leak if a process is SIGKILLed (workaround: reboot)

## Fixed issues

- **Use-after-free in L2capEndpoint::Free()** — race between socket close and HCI disconnect caused spinlock panic (KDL) when `free_command_idents_by_pointer()` accessed a destroyed `HciConnection::fLock`. Fixed by validating connection liveness in btCoreData and reordering `Free()` to read `fConnection` before unbinding from channel maps.

## License

MIT
