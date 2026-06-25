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
**Dátum:** Koniec session (RCP V2.3 a Finálne HMI ESP32)
**Fáza:** Prechod na Android Core
**Status:** 🟢 STABILIZOVANÉ A PRIPRAVENÉ NA ORCHESTRÁCIU

**Čo sa implementovalo v poslednej session:**
- **Zjednotené 5-Screen HMI:** Rozšírenie na 5 plynule rotujúcich obrazoviek. Pridaná nová obrazovka "ATMOSPHERE" pre veľké zobrazenie AQI/PM2.5 a tlaku. Dizajn dlaždíc unifikovaný na pixel-perfect okraje naprieč celým systémom.
- **O(1) Barometrický Trend:** Kruhový buffer v SRAM (72 bajtov) drží 3-hodinovú históriu tlaku. Automatický výpočet trendu z LittleFS a elegantné zobrazenie formou textových znakov (`^`, `v`, `-`) pre šetrenie Heapu.
- **Striktná Integrita Dát (RCP V2.3):** Úplný prepis sťahovania histórie (Delta Sync) cez charakteristiku A104. Implementovaný Binary Framing (5-bajtová obálka pre dĺžku payloadu), unikátne tagovanie streamov (0xA3/0xA4), NTP Guard a Dynamic Headers pre Android parser.
- **Auto-Migrácia a Abort Guard:** Zariadenie pri štarte samo deteguje staré hlavičky CSV súborov a bezpečne ich migruje. Pridaná ochrana proti kolíziám – nový zápis na A103 alebo odpojenie Androidu okamžite, čisto a bezpečne zruší bežiaci dump na A104.

**Kroky pre ďalšiu session (Otvorené úlohy):**
1. **Android Core - RCP V2.3 Parser (Java/Kotlin):** Pripraviť projekt na strane smartfónu. Vybudovať nový `DataStreamHandler`, ktorý zachytáva 5-bajtové obálky, spája 20-bajtové chunky, automaticky extrahuje mapovanie z Dynamic Header (0. riadok) a okamžite ukladá prúdové dáta do Room DB bez preťažovania Heapu (bez zbytočného `StringBuilder`).

[UPDATE-REDPINS-STATE]
Dátum: 2026-06-08 (Aktuálna fáza)
Fáza: ESP32 Firmware Uzavretý (RCP v2.3) -> Prechod na Android Core

1. HMI a Displej:
- Rozšírené na 5 rotujúcich obrazoviek (Sensors, Weather, Atmosphere, Temperature, System).
- Nová obrazovka "Atmosphere" obsahuje čistý a veľký layout pre kvalitu ovzdušia a barometrický tlak.
- Opravené indexovanie menu a rotácie po partial-merge konflikte. Displej je plne responzívny (bez alokácie na Heape).
- Pridaná možnosť HW rotácie displeja o 180° s perzistenciou v `config.json`, ktorá sa aplikuje okamžite pri boote.

2. Úložisko a Barometria (storage.cpp):
- Implementovaný O(1) kruhový buffer (18 vzoriek / 72 bajtov v SRAM) pre 3-hodinový trend tlaku.
- Buffer sa automaticky predvyplní pri štarte (rýchlym seekom v sensor.csv).
- Trend sa zobrazuje v GUI ako textové znaky `^`, `v`, `-` bez bitmapovej fragmentácie pamäte.

3. Architektúra a API (BLE):
- Prechod na striktnú integritu RCP v2.3 (5-bajtová obálka, tagovanie streamov `0xA3`/`0xA4`).
- Pripravené pre `DataStreamHandler` na strane Androidu (zamedzenie OOM pádov pri veľkých CSV prenosoch).

4. Aktuálny cieľ (Android Core):
- Implementácia `DataStreamHandler` v Kotline pre spracovanie 20-bajtových chunkov z `A104` do Room DB.
[/UPDATE-REDPINS-STATE]
