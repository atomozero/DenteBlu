# DenteBlu — Roadmap

## Stato attuale (aprile 2026)

### Funzionante
- Compilazione completa stack BT (106 target, build.sh senza Jam)
- Kernel modules btCoreData e l2cap compilati e caricati correttamente
- bluetooth_server comunica con controller HCI (Broadcom BCM2070)
- Inquiry classica trova dispositivi nelle vicinanze
- A2DP Source: connessione, negoziazione SBC, streaming
- `bt_a2dp_source_test --tone`: tono 440Hz su Google Home ✓
- `bt_a2dp_play`: riproduzione MP3/WAV su Google Home via A2DP ✓
- Encoder SBC via libsbc (reference implementation)
- Auto-gain, resampling, pacing uniforme
- App Preferenze BT con scansione automatica e lista dispositivi

### Da risolvere

## Priorità 1: Bug critici (crash, KDL)

### 1.1 Media add-on crash in InstantiateDormantNode
Il media_addon_server crasha istanziando BluetoothAudioNode.
`BTimeSource::RealTimeFor: performance time too large` nel ControlLoop.

**Causa:** il TimeSource non è inizializzato quando il nodo parte.
**Fix:** verificare TimeSource() in NodeRegistered(), rimuovere
B_PHYSICAL_OUTPUT temporaneamente, override TimeSourceChanged().
**File:** BluetoothAudioNode.cpp, BluetoothAudioAddOn.cpp

### 1.2 Crash in StopStream/Suspend su disconnessione remota
`AvdtpSession::Suspend()` crasha se il socket è stato chiuso
dal dispositivo remoto durante la riproduzione.

**Fix:** verificare validità socket prima di send/recv in Suspend()
e Close(). Usare Abort() se il link è down.
**File:** AvdtpSession.cpp, A2dpSource.cpp

### 1.3 Buffer overflow server con alcuni dispositivi
InquiryResult handler non valida `count` contro la dimensione
dell'evento. Dispositivi con risposte anomale causano overflow.

**Fix:** `count <= (elen - 1) / 14` per InquiryResult standard.
Clamp a max 20.
**File:** LocalDeviceImpl.cpp (linee 1225-1380)

## Priorità 2: Funzionalità core

### 2.1 Persistenza link key (pairing permanente)
Le link key vengono salvate da LinkKeyNotify() ma serve verificare
che il caricamento all'avvio funzioni e che il tipo di chiave sia
compatibile con il dispositivo remoto.

**Approccio:** aggiungere log al boot, creare bt_keystore_dump,
verificare che i file in ~/config/settings/system/bluetooth/keys
siano validi.
**File:** BluetoothKeyStore.cpp, LocalDeviceImpl.cpp, BluetoothServer.cpp

### 2.2 Pairing SSP affidabile dal pulsante Preferenze
RemoteDevice::Authenticate() con ACL già esistente (0x0B) non
invia Authentication_Requested. Il flusso SSP non completa
perché NumericComparisonWindow invia la risposta su un path
diverso dalla petition originale.

**Fix:** inviare Auth_Requested per handle esistente, ristrutturare
il flusso SSP per non dipendere dal timeout della petition.
**File:** RemoteDevice.cpp, NumericComparisonWindow.cpp,
LocalDeviceImpl.cpp, BluetoothWindow.cpp

### 2.3 Filtrare connessioni in ingresso
ConnectionRequest() accetta TUTTE le connessioni. Serve whitelist
basata su key store (solo dispositivi accoppiati).

**File:** LocalDeviceImpl.cpp (linea 1418)

## Priorità 3: Miglioramenti UX

### 3.1 Auto-reconnect dispositivi accoppiati
BluetoothKeyStore ha già SetAutoReconnect()/HasAutoReconnect()
ma non sono usati. Al boot e dopo disconnessione, tentare
riconnessione automatica per dispositivi audio.

**File:** BluetoothServer.cpp, LocalDeviceImpl.cpp

### 3.2 Stato connessione nelle Preferenze
Mostrare icona connesso/disconnesso accanto ai dispositivi
accoppiati. Polling periodico handle ACL dal server.

**File:** BluetoothWindow.cpp

### 3.3 Media add-on: selezione dispositivo da key store
Se bluetooth_audio_device non esiste, usare il primo dispositivo
A2DP accoppiato dal key store. Dropdown BControllable per
selezionare tra dispositivi audio accoppiati.

**File:** BluetoothAudioNode.cpp

## Priorità 4: Funzionalità future

### 4.1 SCO/eSCO per audio voce HFP
Endpoint isocroni USB enumerati ma submit_rx/tx_sco sono stub.
Richiede supporto trasferimenti isocroni XHCI nel kernel Haiku.

### 4.2 Integrazione AVRCP con media transport
Forwarding comandi play/pause/skip AVRCP al sistema media.

### 4.3 Bluetooth HID (tastiere, mouse)
Input server add-on (~1000 righe). PSM 0x0011 + 0x0013.

### 4.4 Fix kernel smp.cpp per Tiger Lake/Alder Lake
Bug nel kernel Haiku, non DenteBlu. Documentato in smp-fix-ticket.md.

## Ordine consigliato di implementazione

1. **1.3** Bounds checking InquiryResult (veloce, previene crash)
2. **1.2** Gestione disconnessione in StopStream (veloce)
3. **1.1** Fix media add-on crash (medio, sblocca audio di sistema)
4. **2.3** Filtro connessioni in ingresso (veloce)
5. **2.1** Verifica persistenza link key (investigazione)
6. **2.2** Affidabilità pairing SSP (medio)
7. **3.1** Auto-reconnect (infrastruttura esiste)
8. **3.2** Stato connessione in Preferenze (UI)
9. **3.3** Selezione dispositivo media add-on (convenience)
