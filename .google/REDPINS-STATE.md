[UPDATE-REDPINS-STATE]
## 1. ARCHITEKTÚRA SYSTÉMU (SSoT) - Revízia 2026-05-29
- **Primary Orchestrator:** Android Aplikácia (`redpins-core-android`).
- **Sense Node (Upgrade):** ESP32-C6-LCD-1.47. Úloha: Lokálna telemetria, zber dát z OpenWeather, vykresľovanie stavu/grafov a komunikácia s Androidom cez RCP v2.1[cite: 2, 3].
- **Komunikačná kompatibilita:** Nový firmvér striktne emuluje správanie starého Sense. Navonok vystupuje s identickou sadou BLE charakteristík (A101 - A104)[cite: 3]. Android nesmie zistiť, že komunikuje s novým hardvérom[cite: 3, 5].

## 2. REPO SETUP - REDPINS SENSE UPGRADE
- **Git Repo Name:** `redpins-sense-esp32`
- **Stack:** ESP-IDF / C++ (Procedurálny štýl, STOP OOP overhead, zákaz dynamickej alokácie v hlavnom cykle).
- **Hardware Abstraction Layer (HAL):**
  - **Displej:** 1.47" LCD (172x320 ST7789) riadený cez SPI s DMA prenosom[cite: 1].
  - **RGB LED:** Riadenie integrovaného podsvietenia pod akrylom cez natívny ESP-IDF RMT (Remote Control) ovládač[cite: 1].
  - **TF Karta:** SPI režim pre ukladanie histórie senzorov a počasia (Delta Sync)[cite: 1, 3].

## 3. STRATEGICKÉ ROZHODNUTIA
- **Asynchrónny rendering:** Keďže ESP32-C6 je single-core RISC-V, GUI engine (LVGL alebo lightweight TFT driver) sa konfiguruje s minimálnym parciálnym bufferom v HP SRAM[cite: 1]. Vykresľovanie nesmie blokovať rádio stack (Wi-Fi/BLE), ktorý beží na pozadí s najvyššou prioritou[cite: 1].
- **Zero-fragmentation:** Dáta pre telemetriu (A101) a JSON metadáta (A102) sa spracovávajú na stacku alebo v statických bufferoch[cite: 1, 3, 5].