# DenteBlu — Stato del progetto (marzo 2026)

## Cos'è

Stack Bluetooth completo per Haiku OS: HCI, L2CAP, SDP, RFCOMM, ATT/GATT, SMP, profili SPP/PBAP/OPP/A2DP/HFP/AVRCP/SCO. Circa 60.000 righe C++ su 252 file sorgente.

## Hardware di test

Intel AX201 Bluetooth (USB 8087:0026) su Intel Core i7-1165G7 (Tiger Lake), Haiku R1 beta5 hrev59484.

## Cosa funziona

Il firmware loader Intel (`btintel.cpp`) completa l'intero flusso a due fasi:

**Prima fase (bootloader → firmware download → reset):**
Il chip viene rilevato in bootloader mode (`fw_variant=0x06`). Il driver legge la versione Intel (0xFC05), i boot params (0xFC0D), carica `ibt-19-0-4.sfi` da disco, lo scarica via Secure Send (0xFC09) su bulk endpoint in 3233 frammenti, estrae `boot_addr=0x00024800` dal firmware, e invia Intel Reset (0xFC01) fire-and-forget. Il chip reboota USB.

**Seconda fase (riconnessione → operativo):**
Il chip si riconnette con firmware operativo. Il driver drena i vendor events di boot (startup 0xFF/0x06, boot complete 0xFF/0x02) dall'interrupt endpoint prima di inviare Read Version. Quando legge `fw_variant=0x23` (operativo), carica la DDC (`ibt-19-0-4.ddc`) via 0xFC8B e imposta la Intel Event Mask (0xFC52). Restituisce `B_OK` al device_added.

**Supporto SCO/eSCO:**
Il driver enumera gli endpoint isochronous da USB interface 1 alt 1 e gestisce il lifecycle (init/cancel/purge) in tutto il ciclo device_open/close/removed. Le funzioni submit_rx_sco/submit_tx_sco sono scheletri pronti per l'integrazione audio.

## Cosa blocca

### Bug kernel: panic in smp.cpp durante operazioni XHCI (Tiger Lake)

`find_free_message()` in `src/system/kernel/smp.cpp:667` fa `ASSERT(are_interrupts_enabled())` quando il pool messaggi SMP è temporaneamente esaurito e il chiamante ha gli interrupt disabilitati. Questo è un percorso legittimo: `release_sem_etc` → `InterruptsLocker` (disabilita interrupt) → `scheduler_enqueue_in_run_queue` → `smp_send_ici` → `find_free_message`. Se il pool è vuoto, panic.

Succede sia dal thread eventi XHCI (`XHCI::ProcessEvents`) sia direttamente dall'interrupt handler (`XHCI::Interrupt`), quando il driver BT genera abbastanza completamenti USB da esaurire il pool.

La fix è in `smp-fix-ticket.md` nella root del progetto: sostituire l'assert con `process_all_pending_ici()` per liberare messaggi, stesso pattern già usato in `acquire_spinlock` nello stesso file.

Senza questa fix il sistema va in KDL durante il ciclo di riconnessione USB post-firmware o durante l'uso normale del controller XHCI sotto carico.

### Seconda fase non verificata end-to-end

Il flusso "drain boot events → Read Version → DDC → event mask → B_OK" è implementato ma il KDL impedisce di verificare che il bluetooth_server riesca a comunicare con il chip operativo. Una volta risolta la questione kernel, il test è: aprire le Preferenze Bluetooth e verificare che il dispositivo appaia.

## Struttura dei commit

```
889edb6 h2generic: supporto SCO/eSCO isochronous endpoints
0fb880d btintel: aggiorna documentazione con flusso completo a due fasi
4d42200 btintel: drain boot events, DDC loading, Intel event mask
72d6bf1 btintel: Intel Reset con boot_addr dal firmware e patch_enable=1
87d8be6 btintel: firmware download funzionante via bulk endpoint
c01a6f2 DenteBlu: stack Bluetooth completo per Haiku OS
```

## File chiave modificati

```
haiku/src/add-ons/kernel/drivers/bluetooth/h2/h2generic/btintel.cpp   — firmware loader Intel
haiku/src/add-ons/kernel/drivers/bluetooth/h2/h2generic/btintel.h     — definizioni comandi Intel
haiku/src/add-ons/kernel/drivers/bluetooth/h2/h2generic/h2generic.cpp — driver USB, endpoint discovery SCO
haiku/src/add-ons/kernel/drivers/bluetooth/h2/h2generic/h2generic.h   — struct bt_usb_dev con campi SCO
haiku/src/add-ons/kernel/drivers/bluetooth/h2/h2generic/h2cfg.h       — BLUETOOTH_SUPPORTS_SCO
haiku/src/add-ons/kernel/drivers/bluetooth/h2/h2generic/h2transactions.cpp — submit_rx/tx_sco
```
