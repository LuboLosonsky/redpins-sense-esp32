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
## 8. AKTUÁLNY STAV (VÝVOJ) - Revízia 2026-05-29 (Senzory & Autonómia Uzavreté)
- **LittleFS & Delta Sync (0x23):** Úplne funkčný O(1) priamy prístup k CSV záznamom. Zamedzené fragmentácii RAM, úspešne streamované cez 19B chunky s `0xFE` hlavičkou.
- **Autonómny Sensor Task:** DHT11 prepojený na bezpečný GPIO 18. Čítanie prebieha nezávisle vo FreeRTOS tasku každých 5s. Zápis do LittleFS prebieha každých 10 minút (pripravené na Delta Sync).
- **RCP v2.1 Kompatibilita (A101/A103):** Telemetria prepísaná zo stringov na prísny 10-bajtový binárny payload (Little Endian: `4B Temp`, `4B Hum`, `2B Pres`), čím sme odblokovali Android Core parser.
- **Aktuálny cieľ pre ďalšiu session:** 
  1. Integrácia SNTP (Reálny čas z Wi-Fi pre presné UNIX timestampy v logoch).
  2. Implementácia `gui_task` pre vykresľovanie nameraných a stiahnutých dát priamo na lokálny 1.47" ST7789 displej.

## AKTUALIZÁCIA STAVU (Handoff)
**Dátum:** 2026-05-31 (Vizuálna Architektúra & Boot Sequence)
**Fáza:** Integrácia GUI / ST7789 displeja (Originál Waveshare na ESP32-C6)
**Status:** 🟢 STABILIZOVANÉ (Plnohodnotný HMI Terminál)

**Čo sa implementovalo:**
- Do systému integrovaný na mieru napísaný `Lightweight Text Renderer` v rámci princípu *Wu Wei*. Súbor `font8x8.h` zaberá v pamäti mikroskopických 768 bajtov, bez nutnosti použitia RAM (načíta sa z Flash).
- DMA buffer bol zväčšený na 40 riadkov, čo umožňuje vykresľovať znaky so Scale faktorom 4 (32px výška) na jediný plynulý zápis cez `esp_lcd_panel_draw_bitmap`.
- Vizuálny layout bol prepracovaný do režimu "Landscape" (na šírku 320x172) výmenou osí (MADCTL Swap_XY).
- Hardvérová korekcia MADCTL zrkadlenia (Mirror_Y = true) zabezpečila správnu orientáciu a čitateľnosť textu (odstránenie zrkadlových blokov).

**Kroky pre ďalšiu session:**
1. Odstrániť (fyzicky vymazať) zložku `.google/`, aby sme predišli ďalším halucináciám kompilátora.
2. Pripraviť a otestovať SNTP integráciu (získanie reálneho UNIX času cez Wi-Fi) na zabezpečenie platných hlavičiek pre Delta Sync záznamy v LittleFS.
