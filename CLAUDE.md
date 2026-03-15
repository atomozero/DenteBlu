# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

DenteBlu is a full Bluetooth stack (Classic + BLE) for Haiku OS. It implements HCI, L2CAP, SDP, RFCOMM, ATT/GATT, SMP, and application profiles (SPP, PBAP, OPP, A2DP, HFP, AVRCP, SCO). ~60,000 lines of C++ across 252 source files.

## Architecture

The stack has four layers communicating via different IPC mechanisms:

1. **Applications** (Preferences GUI, BluetoothSendFile, terminal) call into libbluetooth.so
2. **libbluetooth.so** (Kit) — profile clients, RFCOMM, OBEX, AVDTP, SBC codec, CommandManager. Talks to bluetooth_server via BMessage IPC
3. **bluetooth_server** — LocalDeviceImpl, SdpServer (10 SDP records), BluetoothKeyStore, Deskbar replicant. Talks to kernel via ioctl on `/dev/bluetooth/h2/0`
4. **Kernel modules** — hci, btCoreData, att, smp, l2cap (network protocol), h2generic USB driver with btintel firmware loader. Modules use `module_info` interfaces

Key patterns:
- `CommandManager::buildXxx()` functions allocate HCI command structs; caller frees
- Event flow: `HandleEvent()` in server → `FindPetition()` → specific handler
- `-Werror` is active: unused variables are fatal errors

## Build System

Haiku uses **Jam** (not Make/CMake). Requires a full Haiku source checkout with DenteBlu files copied to matching paths.

```sh
jam bluetooth_server     # server
jam libbluetooth.so      # kit library
jam h2generic            # USB driver + Intel firmware loader
jam l2cap                # L2CAP kernel protocol
jam btCoreData           # core data kernel module
jam bt_spp_test          # any test target by name
```

## Installation

Userspace binaries can be replaced at runtime (but NOT while bluetooth_server is using libbluetooth.so, and NOT the Deskbar add-on while loaded). Kernel modules require a reboot.

```sh
# Userspace
cp bluetooth_server  /boot/system/non-packaged/servers/
cp libbluetooth.so   /boot/system/non-packaged/lib/

# Kernel (reboot required)
cp h2generic   /boot/system/non-packaged/add-ons/kernel/drivers/bin/
ln -sf ../../../bin/h2generic /boot/system/non-packaged/add-ons/kernel/drivers/dev/bluetooth/h2/h2generic
cp btCoreData  /boot/system/non-packaged/add-ons/kernel/bluetooth/
cp hci         /boot/system/non-packaged/add-ons/kernel/bluetooth/
cp l2cap       /boot/system/non-packaged/add-ons/kernel/network/protocols/
```

**CRITICAL**: h2generic MUST go in `drivers/bin/` with a symlink in `drivers/dev/bluetooth/h2/`. Haiku's devfs only loads drivers from `drivers/bin/`. Placing h2generic in `drivers/bluetooth/` will NOT work — the stock driver from the system package will be loaded instead, missing the btintel firmware loader.

**NEVER** use `.bak` for driver backups — Haiku devfs loads ANY file in `drivers/bin/` as a driver.

## Quick Iteration

For testing without reboot: modify only libbluetooth.so + bluetooth_server, reinstall, and relaunch the server.

## Logging

- Server: `TRACE_BT("msg\n")` → `/tmp/bt_server.log`
- Kernel: `ERROR("module: msg\n")` / `TRACE("msg\n")` → `/var/log/syslog`
- Useful: `grep "bt:" /var/log/syslog` for L2CAP kernel messages

## Intel Firmware Loader (btintel)

Critical rules:
- NEVER send HCI Reset before Read Version on Intel chips
- Use Read Version (0xFC05) with param 0xFF
- After firmware download: Intel Reset (0xFC01), chip reboots USB → driver sees device_removed + device_added
- Firmware naming: Legacy (2 or 3 components depending on hw_variant), TLV (hex SWAB-packed cnvi-cnvr)

## Adding a New Profile

1. Public header in `headers/os/bluetooth/`
2. Implementation in `src/kits/bluetooth/`
3. Add to libbluetooth.so Jamfile
4. SDP record in `SdpServer.cpp` if needed
5. GUI window in `src/preferences/bluetooth/` if needed
6. Test in `src/tests/kits/bluetooth/`

## Known Issues

- XHCI ConfigureEndpoint fails on Tiger Lake/Alder Lake (Haiku driver bug, not ours)
- Android won't open incoming RFCOMM to Haiku (likely needs SC P-256; BCM2070 only has P-192)
- PSM leak if process is SIGKILLed (L2CAP PSM stays bound; workaround: reboot)
- Audio profiles (A2DP Source, AVRCP, HFP AG, SCO) not yet tested on real hardware

## Language

The codebase documentation and commit messages are in Italian. Code identifiers and API are in English.
