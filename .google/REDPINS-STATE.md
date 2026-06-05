[UPDATE-REDPINS-STATE]
## 1. ARCHITEKTÚRA SYSTÉMU (SSoT) - Revízia 2026-05-29
- **Primary Orchestrator:** Android Aplikácia (`redpins-core-android`).
- **Sense Node (Upgrade):** ESP32-C6 so 1.47" LCD (172x320, radič ST7789, Touch verzia). Úloha: Lokálna telemetria, zber dát z OpenWeather, vykresľovanie stavu/grafov a komunikácia s Androidom cez RCP v2.1.
- **Komunikačná kompatibilita:** Nový firmvér striktne emuluje správanie starého Sense. Navonok vystupuje s identickou sadou BLE charakteristík (A101 - A104)[cite: 3]. Android nesmie zistiť, že komunikuje s novým hardvérom[cite: 3, 5].

## 2. REPO SETUP - REDPINS SENSE UPGRADE
- **Git Repo Name:** `redpins-sense-esp32`
- **Stack:** ESP-IDF / C++ (Procedurálny štýl, STOP OOP overhead, zákaz dynamickej alokácie v hlavnom cykle).
- **Hardware Abstraction Layer (HAL):**
  - **Displej:** 1.47" LCD (172x320 ST7789 IPS) riadený cez SPI s DMA prenosom.
  - **RGB LED:** Riadenie integrovaného podsvietenia pod akrylom cez natívny ESP-IDF RMT (Remote Control) ovládač[cite: 1].
  - **TF Karta:** SPI režim pre ukladanie histórie senzorov a počasia (Delta Sync)[cite: 1, 3].

## 3. STRATEGICKÉ ROZHODNUTIA
- **Asynchrónny rendering:** Keďže ESP32-C6 je single-core RISC-V, GUI engine (LVGL alebo lightweight TFT driver) sa konfiguruje s minimálnym parciálnym bufferom v HP SRAM[cite: 1]. Vykresľovanie nesmie blokovať rádio stack (Wi-Fi/BLE), ktorý beží na pozadí s najvyššou prioritou[cite: 1].
- **Zero-fragmentation:** Dáta pre telemetriu (A101) a JSON metadáta (A102) sa spracovávajú na stacku alebo v statických bufferoch[cite: 1, 3, 5].

[UPDATE-REDPINS-STATE]
## 8. AKTUÁLNY STAV (VÝVOJ) - Aktuálna revízia (Konfigurácia & BLE Autonómia)
- **Centrálna konfigurácia (`app_config`):** Systém teraz využíva `config.json` na LittleFS ako SSoT. Všetky nastavenia (alias, Wi-Fi, GPS, kalibrácia, UX preferencie) sú perzistentné a automaticky migrované/vytvárané na mieru.
- **RCP v2.1 Kompatibilita (A103):** Úspešne implementované a otestované BLE príkazy `0x31` (Lokalita), `0x26` (Kalibrácia), `0x32` (Zmena Aliasu).
- **HMI & UX Vylepšenia:** Systémová obrazovka presunutá na koniec rotácie (index 3). Vykresľuje detailné dáta menším fontom (IP, GPS, Kalibrácia, Úložisko). Počasie sťahuje a zobrazuje názov mesta z OWM (vrátane bez-alokačného `in-place` filtra diakritiky).

## AKTUALIZÁCIA STAVU (Handoff)
**Dátum:** Koniec session (Pokročilé HMI a Fail-Safe Storage)
**Fáza:** Finalizácia C++ kódu, Príprava na Android Core
**Status:** 🟢 STABILIZOVANÉ (Bez-blikania, Atomic úložisko)

**Čo sa implementovalo v poslednej session:**
- **Fail-Safe Úložisko:** 4-kroková atomic rotácia logov v `storage.cpp` chráni pred korupciou CSV pri výpadku prúdu.
- **State Caching (HMI):** Odstránené preblikávanie GUI pri aktualizácii hodnôt. Obrazovka reaguje stabilne a šetrí SPI zbernicu.
- **Prémiové UX funkcie:** Zavedený 3-stĺpcový dizajn pre "H2O Asistenta vetrania" a "Kvalitu ovzdušia (AQI/PM2.5)". Všetko beží s `O(1)` RAM komplexitou.
- **Oprava ovládania:** Odstránená pasca "mŕtveho" kontextového menu na obrazovkách bez nastavení.

**Kroky pre ďalšiu session (Otvorené úlohy):**
1. **ESP32 HMI - Barometrický trend:** Doplniť na obrazovku počasia predikciu (šípky) podľa offline histórie tlaku z LittleFS.
2. **Android Core (Bluetooth):** Implementácia prúdového parsera `DataStreamHandler` v Androide pre bezpečné spájanie `0xFD` (JSON) a `0xFE` (CSV) chunkov z charakteristiky A104 a ich import do Room databázy.