# DenteBlu

Stack Bluetooth completo per Haiku OS. Classic (BR/EDR) e BLE, dal driver USB fino ai profili applicativi.

## Stato

Lo stack funziona con dongle USB Bluetooth 2.0 (chipset BCM2045, CSR, ecc.) su Haiku R1 beta5. Pairing, SDP discovery, e profili SPP/RFCOMM sono operativi.

Per i chip **Intel AX201** (USB 8087:0026) il firmware loader è implementato e completa il download del firmware (801KB, 3233 frammenti via Secure Send). Il chip reboota con firmware operativo. Il lavoro di integrazione con il bluetooth_server è in corso — vedi la sezione [Intel firmware loader](#intel-firmware-loader).

## Architettura

```
Applicazioni / Preferences GUI
        │ BMessage IPC
   libbluetooth.so  (profili, RFCOMM, OBEX, AVDTP, SBC codec)
        │ ioctl su /dev/bluetooth/h2/0
   bluetooth_server  (LocalDeviceImpl, SDP server, KeyStore)
        │ ioctl
   Kernel: hci + btCoreData + l2cap + h2generic (driver USB)
```

Quattro livelli, ~60.000 righe C++ su 252 file sorgente.

## Compilazione

Serve un checkout sorgente Haiku. I file di DenteBlu vanno copiati nei path corrispondenti dell'albero Haiku. Il build system è Jam.

```sh
jam h2generic        # driver USB + Intel firmware loader
jam bluetooth_server # server
jam libbluetooth.so  # libreria kit
jam l2cap            # protocollo kernel L2CAP
jam btCoreData       # modulo kernel dati core
```

## Installazione

```sh
# Userspace (sostituibili a runtime)
cp bluetooth_server  /boot/system/non-packaged/servers/
cp libbluetooth.so   /boot/system/non-packaged/lib/

# Kernel (richiede reboot)
cp h2generic   /boot/system/non-packaged/add-ons/kernel/drivers/bin/
ln -sf ../../../bin/h2generic \
  /boot/system/non-packaged/add-ons/kernel/drivers/dev/bluetooth/h2/h2generic
cp btCoreData  /boot/system/non-packaged/add-ons/kernel/bluetooth/
cp hci         /boot/system/non-packaged/add-ons/kernel/bluetooth/
cp l2cap       /boot/system/non-packaged/add-ons/kernel/network/protocols/
```

h2generic **deve** stare in `drivers/bin/` con un symlink in `drivers/dev/bluetooth/h2/`. Haiku devfs carica i driver solo da `drivers/bin/`.

## Intel firmware loader

Il modulo `btintel.cpp` gestisce i chip Intel che richiedono firmware download via USB. Flusso:

1. Read Intel Version (0xFC05) — rileva bootloader (`fw_variant=0x06`)
2. Read Boot Params (0xFC0D), carica `.sfi` da `/boot/system/non-packaged/data/firmware/intel/`
3. Download via Secure Send (0xFC09) su bulk endpoint
4. Intel Reset (0xFC01) — il chip reboota USB
5. Drain boot events, Read Version → `fw_variant=0x23` (operativo)
6. DDC loading (0xFC8B), Intel Event Mask (0xFC52)

I file firmware si trovano nel repository [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/intel) (es. `ibt-19-0-4.sfi`, `ibt-19-0-4.ddc`).

**Nota:** su Tiger Lake/Alder Lake c'è un bug kernel in `smp.cpp` che causa KDL durante operazioni XHCI sotto carico. La fix è in `smp-fix-ticket.md`.

## Profili implementati

| Profilo | Stato |
|---------|-------|
| SPP (Serial Port) | Funzionante |
| PBAP (Phone Book) | Implementato |
| OPP (Object Push) | Implementato |
| A2DP (Audio) | Implementato, non testato su HW |
| HFP (Hands-Free) | Implementato, non testato su HW |
| AVRCP (Remote Control) | Implementato, non testato su HW |
| SCO/eSCO (Audio sync) | Endpoint USB pronti, streaming da completare |
| ATT/GATT (BLE) | Implementato |

## Problemi noti

- Il bug kernel `smp.cpp` su Tiger Lake causa KDL durante la riconnessione USB post-firmware Intel — vedi `smp-fix-ticket.md`
- Android non apre connessioni RFCOMM in ingresso verso Haiku (probabilmente serve Secure Connections P-256; BCM2070 ha solo P-192)
- Leak PSM in L2CAP se un processo viene killato con SIGKILL (workaround: reboot)

## Licenza

MIT
