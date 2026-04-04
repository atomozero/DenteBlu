# DenteBlu — Roadmap

## Stato attuale (4 aprile 2026)

### Funzionante
- Compilazione completa stack BT (106 target, build.sh senza Jam)
- Kernel modules btCoreData e l2cap con simboli @KERNEL_BASE corretti
- bluetooth_server con controller HCI (Broadcom BCM2070)
- Inquiry classica, discovery dispositivi
- **A2DP Source completo**: connessione → AVDTP → SBC (libsbc) → RTP → streaming
- **bt_a2dp_play**: riproduzione MP3/WAV su Google Home — 3+ min verificati
- Auto-gain, resampling 11025→44100Hz, pacing uniforme (~11ms/pkt)
- App Preferenze BT con scansione automatica, lista 2 righe (nome + tipo)
- Bounds check InquiryResult (previene crash da dispositivi anomali)
- Filtro connessioni in ingresso (solo dispositivi accoppiati)
- Gestione robusta disconnessione remota (Suspend/Close/Disconnect)
- SSP Authentication Requested su ACL esistente
- Log conteggio link key al boot

### Risolto in questa sessione (14 commit)
- ✅ 1.1 Media add-on: rimosso B_PHYSICAL_OUTPUT, B_FLAVOR_IS_GLOBAL, _InitParameterWeb
- ✅ 1.2 Crash StopStream/Suspend: check socket in _RecvSignal, Close robusto
- ✅ 1.3 Bounds check InquiryResult: clamp count a max 20
- ✅ 2.3 Filtro connessioni in ingresso: whitelist da keystore
- ✅ 2.2 (parziale) SSP su ACL esistente: Auth Requested con handle
- ✅ Crash _RecvSignal su socket morto: check fSignalingSocket < 0
- ✅ Media add-on _InitParameterWeb crash: rimosso (vtable thunking bug)

---

## Priorità 1: Media add-on (audio di sistema → BT)

### 1.1 Stabilizzare BluetoothAudioNode nel media_addon_server
Il nodo si registra come dormant ma InstantiateDormantNode fallisce
con "Name not found" — il media_addon_server crasha durante il
caricamento. Problemi vtable thunking con ereditarietà multipla.

**Approccio:** investigare se il crash è nella libbluetooth.so di
sistema (vecchia, senza A2dpSource) vs la nostra. Il media_addon_server
potrebbe caricare la lib di sistema invece della nostra.
Possibile fix: linkare l'add-on con RPATH o path assoluto a
/boot/system/non-packaged/lib/libbluetooth.so.
**File:** BluetoothAudioNode.cpp, BluetoothAudioAddOn.cpp, build.sh

### 1.2 BufferReceived → SendAudio con pacing disabilitato
Il codice è pronto (SetPacingEnabled(false) nel B_START handler).
Serve verificare che i buffer dal mixer arrivino nel formato
corretto e che SendAudio funzioni senza pacing.
**File:** BluetoothAudioNode.cpp

### 1.3 Selezione dispositivo BT dal media add-on
Attualmente legge da ~/config/settings/bluetooth_audio_device.
Aggiungere fallback al primo dispositivo A2DP nel keystore.
**File:** BluetoothAudioNode.cpp

## Priorità 2: Pairing permanente

### 2.1 Verifica end-to-end persistenza link key
La catena LinkKeyNotify → AddLinkKey → Save è implementata.
Verificare con test su dispositivo reale:
1. Connettere Google Home, verificare che link key è salvata
2. Riavviare server, verificare che la key viene caricata
3. Riconnettere senza pairing mode

**Problema noto:** durante le connessioni A2DP riuscite il controller
spesso non invia LINK_KEY_NOTIFY (Just Works senza scambio chiave).
Il pairing SSP vero richiede Authentication_Requested esplicito.
**File:** BluetoothKeyStore.cpp, LocalDeviceImpl.cpp

### 2.2 ConnectionIncoming dialog per pairing
La finestra è stata rimossa (era uno stub vuoto). Serve implementare:
- Mostrare nome dispositivo, tipo, e codice numerico SSP
- Pulsanti Accept/Decline
- Inviare User_Confirmation_Reply al server
**File:** ConnectionIncoming.cpp, ConnectionView.cpp

### 2.3 Pairing dal pulsante Preferenze — flusso completo
Il pulsante Pair ora chiama Authenticate che gestisce ACL 0x0B.
Serve verificare che il flusso SSP completi (IO_CAP → Confirmation
→ Link Key) e che la chiave venga salvata.
**File:** RemoteDevice.cpp, BluetoothWindow.cpp

## Priorità 3: UX e stabilità

### 3.1 Auto-reconnect al boot
Infrastruttura creata (timer 5s + scan keystore per HasAutoReconnect).
Serve implementare la connessione A2DP effettiva per i dispositivi
marcati, e aggiungere "Connect on startup" nelle Preferenze.
**File:** BluetoothServer.cpp, BluetoothWindow.cpp

### 3.2 Stato connessione nelle Preferenze
Mostrare icona/testo connesso/disconnesso accanto ai dispositivi.
Polling periodico o notifica dal server su DisconnectionComplete.
**File:** BluetoothWindow.cpp

### 3.3 Preferenze: mostrare dispositivo BT collegato nel media add-on
Nel pannello Preferenze Media, indicare a quale speaker/cuffia BT
è collegato il "Bluetooth Audio Output" e lo stato della connessione.
**File:** BluetoothAudioNode.cpp (BControllable parameters)

### 3.4 bt_a2dp_play: gestione file troncati
Il decoder MP3 si ferma al 25% per file VBR con metadata errati.
Gestire "Last buffer" come fine normale senza errore.
**File:** bt_a2dp_play.cpp

## Priorità 4: Funzionalità future

### 4.1 SCO/eSCO per audio voce HFP
Endpoint isocroni USB enumerati, stub submit_rx/tx_sco pronti.
Richiede supporto XHCI isochronous nel kernel Haiku.
**File:** h2transactions.cpp, h2generic.cpp

### 4.2 AVRCP ↔ media transport
Forwarding play/pause/skip dal controller AVRCP al media kit.
**File:** AvrcpTarget.cpp, BluetoothAudioNode.cpp

### 4.3 Bluetooth HID (tastiere, mouse)
Input server add-on (~1000 righe). PSM 0x0011 + 0x0013.

### 4.4 A2DP Sink (ricevere audio BT)
Ricevere streaming audio da telefono → Haiku speaker.
Infrastruttura A2dpSink.cpp esiste ma non testata.

### 4.5 Fix kernel smp.cpp per Tiger Lake/Alder Lake
Bug nel kernel Haiku, non DenteBlu.

---

## Ordine consigliato

1. **1.1** Media add-on stabile (sblocca audio sistema → BT)
2. **2.1** Verifica persistenza link key (pairing permanente)
3. **2.2** Dialog ConnectionIncoming (UI pairing)
4. **3.1** Auto-reconnect (infrastruttura pronta)
5. **2.3** Pairing completo da Preferenze
6. **3.2** Stato connessione in UI
7. **1.2–1.3** Media add-on: BufferReceived + selezione device
8. **3.4** Gestione file troncati
9. **4.x** Funzionalità future
