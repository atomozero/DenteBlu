# DenteBlu — TODO

Parti mancanti per raggiungere compatibilita' completa con le specifiche
Bluetooth (rispetto a stack maturi come BlueZ su Linux o FreeBSD).

Ultimo aggiornamento: marzo 2026.


## Priorita' alta (bloccano use-case reali)

### 1. SCO data path nel driver USB
Il codice ScoSocket manda comandi HCI per setup SCO, e gli endpoint
isochronous USB sono enumerati, ma il trasferimento dati effettivo non
e' implementato. Senza questo, HFP non puo' trasmettere audio voce.
- **File**: `h2generic.cpp`, `h2transactions.cpp` (submit_rx_sco / submit_tx_sco)
- **Stima**: ~500 righe
- **Dipendenza**: fix XHCI isoc (vedi `tools/haiku_ticket_xhci_isoc.md`)

### 2. Test hardware profili audio
A2DP Source, AVRCP Target, HFP AG e SCO sono completi a livello di codice
e unit test, ma mai testati con cuffie BT reali.
- **Serve**: adattatore BT 2.1+EDR + cuffie A2DP standard
- **Rischio**: possibili bug nel pacing RTP, nel codec negotiation, o nel
  media add-on lifecycle

### 3. Fix kernel smp.cpp (Tiger Lake / Alder Lake)
Il panic in `find_free_message()` impedisce l'uso di Intel AX201.
La fix e' pronta in `smp-fix-ticket.md`, va proposta upstream a Haiku.
- **Non e' codice DenteBlu**, e' un bug del kernel Haiku
- **Impatto**: blocca tutta la verifica su hardware Intel moderno


## Priorita' media (miglioramenti significativi)

### 4. GATT server (BLE peripheral)
Attualmente solo GATT client. Per poter fare da peripheral BLE (es.
esporre servizi custom, beacon) serve un GATT server nel modulo kernel ATT.
- **File**: `att.cpp`, nuovo `GattServer.cpp` nel kit
- **Stima**: ~800 righe

### 5. BLE advertising
Possiamo fare scan BLE ma non advertising. Serve per rendersi visibili
come dispositivo BLE.
- **File**: `CommandManager.cpp` (comandi LE Set Advertising Parameters/Data/Enable)
- **File**: `LocalDeviceImpl.cpp` (logica server)
- **Stima**: ~300 righe

### 6. L2CAP LE CoC (Connection-Oriented Channels)
Per trasferimento dati BLE ad alta velocita'. Richiede signaling
LE Credit Based Connection Request/Response nel modulo L2CAP kernel.
- **File**: `l2cap_signal.cpp`, `L2capEndpoint.cpp`
- **Stima**: ~600 righe

### 7. LE Privacy (RPA — Resolvable Private Address)
I dispositivi BLE moderni usano indirizzi randomizzati. Senza RPA,
il pairing con molti dispositivi BLE fallisce o non persiste.
- **File**: `smp.cpp` (kernel), `CommandManager.cpp` (LE Set Random Address)
- **Stima**: ~400 righe

### 8. Bluetooth HID (Human Interface Device)
Mouse e tastiere BT. HID usa L2CAP direttamente (PSM 0x0011 control,
PSM 0x0013 interrupt). Serve un input_server add-on per Haiku.
- **Nuovo componente**: `bluetooth_hid` input add-on
- **Stima**: ~1000 righe
- **Alta richiesta dalla community Haiku**


## Priorita' bassa (spec avanzate, BT 5.0+)

### 9. Extended Advertising / Coded PHY (BT 5.0)
Extended advertising con set multipli, PHY coded per long range.
- **Richiede**: controller BT 5.0+

### 10. LE Audio con codec LC3 (BT 5.2)
Nuovo protocollo audio BLE: Isochronous Channels + codec LC3.
Sostituto di A2DP per dispositivi moderni (earbuds, hearing aids).
- **Richiede**: controller BT 5.2+, libreria LC3 (liblc3)
- **Stima**: ~3000+ righe (nuovo subsystem)

### 11. Bluetooth Mesh
Networking mesh BLE per IoT. Specifica complessa e indipendente.
- **Stima**: ~5000+ righe
- **Casi d'uso**: domotica, sensori


## Bug aperti

### B1. Android incoming RFCOMM
Android non apre connessioni RFCOMM verso Haiku. Probabilmente richiede
Secure Connections (P-256) che il BCM2070 non supporta. Da riverificare
con un dongle BT 4.0+ che supporti SC.

### B2. PSM leak su SIGKILL
Se un processo viene killato, il PSM L2CAP non viene rilasciato.
Serve un cleanup hook nel kernel (es. team_death_entry).

### B3. XHCI ConfigureEndpoint rifiuta endpoint isoc con wMaxPacketSize=0
Bug nel driver XHCI di Haiku. Patch proposta in `tools/haiku_ticket_xhci_isoc.md`.
Non e' codice DenteBlu.
