# DenteBlu — Prompt per collaboratore

Sei uno sviluppatore che lavora sul progetto DenteBlu: uno stack Bluetooth
completo per Haiku OS. Questo documento ti da' tutto il contesto necessario
per essere produttivo immediatamente.

---

## Cos'e' DenteBlu

DenteBlu aggiunge supporto Bluetooth Classic e BLE a Haiku OS. Il progetto
implementa da zero (o estende lo scheletro originale di Haiku) tutti i layer
dello stack: HCI, L2CAP, SDP, RFCOMM, ATT/GATT, SMP, e i profili applicativi
(SPP, PBAP, OPP, A2DP, HFP, AVRCP). Il target originale era comunicare con
un Heltec V3 (MeshCore) via Nordic UART Service, ma il progetto e' cresciuto
fino a coprire quasi tutti i profili BT classici.

## Architettura dello stack

```
  ┌─────────────────────────────────────────────────────┐
  │                  APPLICAZIONI                        │
  │  Bluetooth Prefs │ BluetoothSendFile │ Terminal      │
  └────────┬─────────┴──────┬────────────┴──────┬───────┘
           │                │                   │
  ┌────────▼────────────────▼───────────────────▼───────┐
  │              libbluetooth.so  (Kit)                  │
  │  SppClient, PbapClient, OppClient, A2dpSource,      │
  │  A2dpSink, HfpClient, HfpAudioGateway, AvrcpTarget, │
  │  ScoSocket, BleDevice, NusClient, SbcEncoder/Dec,   │
  │  RfcommSession, ObexClient, AvdtpSession,           │
  │  CommandManager, LocalDevice, RemoteDevice           │
  └────────┬────────────────────────────────────────────┘
           │ BMessage IPC
  ┌────────▼────────────────────────────────────────────┐
  │              bluetooth_server                        │
  │  LocalDeviceImpl, SdpServer (10 record SDP),        │
  │  BluetoothKeyStore, DeskbarReplicant                │
  └────────┬────────────────────────────────────────────┘
           │ ioctl / module_info
  ┌────────▼────────────────────────────────────────────┐
  │              KERNEL                                  │
  │  hci module │ btCoreData │ att │ smp                 │
  │  l2cap (network protocol)                            │
  │  h2generic (USB driver + btintel firmware loader)    │
  └─────────────────────────────────────────────────────┘
```

### Comunicazione tra layer

- **Kit <-> Server**: BMessage via BMessenger (libbluetooth.so parla col bluetooth_server)
- **Server <-> Kernel**: ioctl() sul device /dev/bluetooth/h2/0
- **Kernel modules**: `module_info` interfaces (`bt_hci_module_info`, `bluetooth_core_data_module_info`)
- **CommandManager**: funzioni `buildXxx()` che generano comandi HCI (malloc, caller frees)
- **Event handling**: `HandleEvent()` nel server → `FindPetition()` → handler specifico

### Directory structure

```
haiku/headers/os/bluetooth/        # API pubblica (26 header)
haiku/headers/private/bluetooth/   # Header interni (21 header)
haiku/src/kits/bluetooth/          # libbluetooth.so — 26 .cpp, ~16K righe
haiku/src/servers/bluetooth/       # bluetooth_server — 9 .cpp, ~7.4K righe
haiku/src/add-ons/kernel/bluetooth/  # Moduli kernel (att, btCoreData, hci, smp)
haiku/src/add-ons/kernel/network/protocols/l2cap/  # L2CAP — ~4.5K righe
haiku/src/add-ons/kernel/drivers/bluetooth/h2/h2generic/  # Driver USB + btintel
haiku/src/tests/kits/bluetooth/    # 35+ test programs
haiku/src/preferences/bluetooth/   # GUI Preferenze — 15 .cpp, ~6.3K righe
tools/                             # download_intel_fw.sh
releases/                          # Pacchetti binari + install.sh
```

## Build system

Haiku usa **Jam** (non Make/CMake). I Jamfile sono nelle directory sorgenti.
Per compilare serve un checkout completo di Haiku con il nostro codice copiato
nelle posizioni corrispondenti.

```sh
# Compilare singoli target:
jam bluetooth_server     # → generated/.../servers/bluetooth/bluetooth_server
jam libbluetooth.so      # → generated/.../kits/bluetooth/libbluetooth.so
jam h2generic            # → generated/.../drivers/bluetooth/h2/h2generic/h2generic
jam l2cap                # → generated/.../network/protocols/l2cap/l2cap
jam btCoreData           # → generated/.../bluetooth/btCoreData/btCoreData
jam bt_spp_test          # qualsiasi test

# ATTENZIONE:
# -Werror e' attivo: variabili non usate sono errore fatale
# BString.h va incluso esplicitamente dove BString e' un campo
```

## Installazione dei binari

```sh
# Userspace (puo' essere sostituito a caldo, MA:
#   - NON sostituire libbluetooth.so mentre bluetooth_server gira → segfault
#   - NON sostituire add-on Deskbar mentre e' caricato → invalid opcode)
cp bluetooth_server  /boot/system/non-packaged/servers/
cp libbluetooth.so   /boot/system/non-packaged/lib/

# Kernel (richiede REBOOT):
cp h2generic   /boot/system/non-packaged/add-ons/kernel/drivers/bin/
ln -sf ../../../bin/h2generic /boot/system/non-packaged/add-ons/kernel/drivers/dev/bluetooth/h2/h2generic
cp btCoreData  /boot/system/non-packaged/add-ons/kernel/bluetooth/
cp hci         /boot/system/non-packaged/add-ons/kernel/bluetooth/
cp l2cap       /boot/system/non-packaged/add-ons/kernel/network/protocols/

# CRITICO: h2generic DEVE stare in drivers/bin/ con symlink in drivers/dev/bluetooth/h2/!
# Haiku devfs carica SOLO i driver da drivers/bin/. Se messo in drivers/bluetooth/
# viene ignorato e caricato il driver stock del sistema (senza btintel firmware loader).

# IMPORTANTE: MAI usare .bak per backup dei driver!
# Haiku devfs carica QUALSIASI file in drivers/bin/ come driver.
# h2generic.bak diventa una seconda istanza del driver.
```

## Stato implementazione — cosa funziona

| Componente | Stato | Note |
|------------|-------|------|
| HCI (comandi, eventi, ACL) | Completo | Classic + LE |
| L2CAP Basic Mode | Completo | PSM 1 (SDP), PSM 3 (RFCOMM) |
| L2CAP ERTM | Completo | Per GOEP 2.0 PBAP |
| L2CAP LE (CID fissi) | Completo | ATT, SMP |
| SDP Server | Completo | 10 record (SPP, PBAP, OPP, A2DP Sink/Source, HFP HF/AG, AVRCP) |
| RFCOMM | Completo | Userspace, over L2CAP PSM 3 |
| ATT/GATT | Completo | Kernel module, ioctl-based |
| SMP (pairing) | Completo | P-192 (Legacy), P-256 (SC) |
| SSP (Secure Simple Pairing) | Completo | Numeric comparison, passkey |
| SPP (Serial Port Profile) | Completo | SppClient + GUI terminal |
| PBAP (PhoneBook Access) | Completo | ObexClient + PbapClient + GUI browser |
| OPP (Object Push) | Completo | OppClient + BluetoothSendFile app |
| A2DP Sink | Completo | AvdtpSession + SbcDecoder |
| A2DP Source | Completo | A2dpSource + SbcEncoder + SDP record |
| AVRCP Target | Completo | Play/Pause/Stop/Volume, SDP record |
| HFP Client (Hands-Free) | Completo | AtParser + HfpClient |
| HFP Audio Gateway | Completo | HfpAudioGateway + SDP record |
| SCO Audio | Completo | ScoSocket + HCI sync commands |
| bluetooth_audio media add-on | Completo | BBufferConsumer per Media Kit |
| Intel firmware loader (btintel) | Completo | TLV + Legacy + IML + Secure Send |
| Auto-reconnect | Completo | |
| Deskbar replicant | Completo | Add-on separato (no dipendenza libbluetooth) |
| BLE/NUS | Codice presente | Serve adattatore BT 4.0+ per test |

## Problemi noti aperti

### 1. XHCI ConfigureEndpoint su Tiger Lake / Alder Lake
Il driver XHCI di Haiku non gestisce correttamente `max_burst_payload` per
certi Intel USB controller. Errore: `ConfigureEndpoint() failed invalid
max_burst_payload`. Impedisce il funzionamento dei bulk endpoint.
**Impatto**: il firmware Intel si scarica (usa control endpoint), ma il
dispositivo potrebbe non funzionare dopo il boot del firmware.
**Serve**: fix nel driver XHCI di Haiku (non nel nostro codice).

### 2. Android non apre RFCOMM verso Haiku (incoming)
Il telefono fa SDP query (funziona), ma non tenta mai authentication o
L2CAP Connection Request. Ipotesi: Android richiede Secure Connections
(P-256/SC) ma il BCM2070 supporta solo P-192.
**Status**: deprioritizzato, le connessioni in uscita da Haiku funzionano.

### 3. PSM leak su process kill
Se un processo viene killato con SIGKILL, Shutdown() non viene chiamato
e il PSM L2CAP resta bound. Workaround: reboot.

### 4. Profili audio non testati su hardware reale
A2DP Source, AVRCP Target, HFP AG, SCO: codice completo con unit test,
ma mai testato con cuffie BT reali. Serve un adattatore BT e cuffie.

## Hardware di sviluppo

- **Dongle USB**: Broadcom BCM2070 (Foxconn T77H114), BT 2.1+EDR
  - bdaddr C4:46:19:CB:3F:72, MaxKeys=1, no P-256/SC
  - Funziona bene per Classic BT, no BLE
- **Telefono test**: moto g15 (0C:7D:B0:B2:81:6A), Android 14+
  - App "Serial Bluetooth Terminal" per SPP
- **Cuffie test**: Pixel Buds 1 (CC:A7:C1:F2:52:65) — A2DP/HFP

## Pattern di codice importanti

### Aggiungere un nuovo profilo
1. Header pubblico in `headers/os/bluetooth/NuovoProfilo.h`
2. Implementazione in `src/kits/bluetooth/NuovoProfilo.cpp`
3. Aggiungere al Jamfile di libbluetooth.so
4. Se serve SDP record: aggiungere in `SdpServer.cpp`
5. Se serve GUI: aggiungere finestra in `src/preferences/bluetooth/`
6. Test in `src/tests/kits/bluetooth/bt_nuovoprofilo_test.cpp`

### Comandi HCI
```cpp
// In CommandManager.cpp:
void* buildNuovoComando(/* params */) {
    // alloca struct, riempie, ritorna puntatore
    // Il caller fa free()
}
```

### Logging
- Server: `TRACE_BT("msg\n")` → `/tmp/bt_server.log`
- Kernel: `ERROR("module: msg\n")` / `TRACE("msg\n")` → syslog (`/var/log/syslog`)
- Grep utili: `grep "bt:" /var/log/syslog` per L2CAP kernel, `cat /tmp/bt_server.log`

## Intel firmware loader (btintel)

Il modulo btintel in h2generic gestisce il firmware loading per chip Intel:

- **Legacy** (hw_variant 0x0b-0x14): firmware `.sfi`, download via Secure Send
  - Naming: `ibt-<hw_variant>-<hw_revision>-<fw_revision>.sfi` (3 componenti per >= 0x11)
  - Naming: `ibt-<hw_variant>-<dev_revid>.sfi` (2 componenti per 0x0b/0x0c)
- **TLV** (risposta version != 10 bytes o byte[1] != 0x37): firmware `.sfi`
  - Naming: `ibt-<cnvi>-<cnvr>.sfi` (hex, SWAB-packed)
- **Blazar** (hw_variant >= 0x1e): two-stage boot (bootloader→IML→operational)

Regole critiche:
- MAI mandare HCI Reset prima di Read Version su Intel
- Read Version (0xFC05) con param 0xFF
- Dopo firmware download: Intel Reset (0xFC01), il chip reboota USB
- Il driver vede `device_removed` + `device_added` → controlla se operational

## Cosa manca per compatibilita' 100% BT (rispetto a Linux/FreeBSD)

Analisi completata marzo 2026. Lista dettagliata con stime e priorita' in `TODO.md`.

### Priorita' alta (bloccano use-case reali)

1. **SCO data path nel driver USB** (~500 righe) — il codice ScoSocket manda comandi HCI
   per setup SCO, e gli endpoint isochronous sono enumerati, ma il trasferimento dati
   effettivo non e' implementato. Blocca audio voce HFP.
   - File: `h2generic.cpp`, `h2transactions.cpp`
   - Dipendenza: fix XHCI isoc (`tools/haiku_ticket_xhci_isoc.md`)

2. **Test hardware profili audio** — A2DP Source, AVRCP Target, HFP AG, SCO sono
   completi con unit test ma mai verificati con cuffie BT reali.

3. **Fix kernel smp.cpp** — bug Haiku (non DenteBlu) che causa KDL su Tiger Lake.
   Fix pronta in `smp-fix-ticket.md`, da proporre upstream.

### Priorita' media (miglioramenti significativi)

4. **GATT server** (~800 righe) — solo client implementato; serve per fare da
   peripheral BLE (esporre servizi, beacon)
5. **BLE advertising** (~300 righe) — possiamo fare scan ma non advertising
6. **L2CAP LE CoC** (~600 righe) — per BLE data transfer ad alta velocita'
7. **LE Privacy / RPA** (~400 righe) — indirizzi randomizzati BLE per pairing persistente
8. **Bluetooth HID** (~1000 righe) — mouse e tastiere BT, input_server add-on.
   Alta richiesta dalla community Haiku.

### Priorita' bassa (spec BT 5.0+)

9. **Extended Advertising / Coded PHY** — BT 5.0, richiede controller 5.0+
10. **LE Audio / LC3** (~3000+ righe) — BT 5.2, nuovo subsystem completo
11. **Bluetooth Mesh** (~5000+ righe) — networking IoT

## Come contribuire

1. Clona un albero Haiku completo (branch `master`)
2. Copia il contenuto di `haiku/` di questo pacchetto nelle posizioni
   corrispondenti nell'albero Haiku
3. `jam <target>` per compilare
4. Copia i binari nelle posizioni di installazione (vedi sopra)
5. Per i moduli kernel: reboot dopo l'installazione

Per test rapidi senza reboot: modifica solo libbluetooth.so + bluetooth_server,
reinstalla e rilancia il server.

---

*DenteBlu — ~60,000 righe, 252 file sorgenti, release r007a (2026-03-15)*
