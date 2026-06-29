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

[UPDATE-REDPINS-STATE]
Dátum: 2026-06-28 (Claude Code session)
Fáza: ESP32 Firmware (Sense) - HMI bugfix

1. GUI Bugfix (gui.cpp, obrazovka SENSORS):
- Opravené prekrytie textu v spodnom (treťom) bloku obrazovky "SENSORS". Riadok stavu senzora počasia (`"WEATHER SENZOR: OK/FAIL"`, zelená/červená) sa kresbil na takmer rovnakej y-pozícii ako popisky `"H2O VETRANIE:"`, `"VONKU"`, `"VNÚTRI"` (rozdiel len 2px), čo spôsobovalo viditeľné prekreslenie/prekrytie textov.
- Príčina: stav senzora bol pridaný neskôr (commit `f996a6c`) bez úpravy y-súradníc existujúcich popiskov ventilácie.
- Fix: zúžená výška clear-rect pre riadok stavu senzora (12px → 9px) a popisky ventilácie posunuté z `box_y2+5` na `box_y2+13`, čím sa riadky oddelili bez prekrytia.
- Poznámka: farba "WEATHER SENZOR" textu je v kóde korektne zelená/červená (`SYS_WIFI_OK_FG`/`SYS_WIFI_ERR_FG`), nie modrá - užívateľ zariadenie reálne nahlásil ako modrastý odtieň, čo je pravdepodobne vizuálny dojem na malom IPS displeji, nie chyba v `rgb_endian` konfigurácii (overené `LCD_RGB_ENDIAN_RGB` v `display_hal.cpp`).

2. Konvencia pre ďalšie session:
- Lubo teraz používa Claude Code (VS Code extension) namiesto/popri Gemini a Copilot CLI. Claude udržiava vlastnú perzistentnú pamäť MIMO repozitára (automatická, cross-session) a navyše pokračuje v aktualizácii tohto súboru rovnakým `[UPDATE-REDPINS-STATE]` formátom pre transparentnosť v repozitári.
[/UPDATE-REDPINS-STATE]

[UPDATE-REDPINS-STATE]
Dátum: 2026-06-28 (Claude Code session, pokračovanie)
Fáza: ESP32 Firmware (Sense) - HMI redesign + BME280 noise fix

1. Redesign obrazovky ATMOSPHERE (gui.cpp):
- Layout zjednotený so vzorom obrazovky SENSORS: horný riadok = dva boxy vedľa seba (VONKAJŠÍ TLAK z OpenWeatherMap, VNÚTORNÝ TLAK z lokálneho BME280 + trendová šípka), spodný riadok = jeden box cez celú šírku (KVALITA OVZDUŠIA, AQI+PM2.5).
- Oprava: barometrický trend (`storage_get_pressure_trend()`) sa počíta z histórie lokálneho senzora (`sensor.csv`), pôvodne sa ale zobrazoval pri vonkajšom (OWM) tlaku - teraz je správne pri vnútornom.

2. Vypnuté debug logy tlačidiel (gui.cpp):
- `GUI_BUTTON_DEBUG` flag prepnutý na 0 (bolo 1). Tlačidlá sú overené ako funkčné, periodický "BTN RAW: ..." log každé 2s aj change-log boli len diagnostika počas vývoja.

3. Fix: BME280 šum spôsoboval zbytočný redraw displeja (sensor_core.cpp, gui.cpp):
- Príčina: BME280 má jemnejšie rozlíšenie ako starý DHT11, raw čítanie kolíše o ±0.2-0.3°C/%RH medzi vzorkami (1s interval). GUI porovnávalo cache na plnú float presnosť, displej ale zobrazuje len 1 desatinné miesto - takže aj neviditeľná zmena vyvolala redraw každých 2s.
- Fix časť 1 (zdroj dát): EMA filter v `sensor_task` (`SENSOR_EMA_ALPHA = 0.1f`) vyhladzuje `s_last_t`/`s_last_h` priamo pri zápise - prospieva displeju, CSV logu aj BLE telemetrii naraz.
- Fix časť 2 (GUI cache): nový `round1()` helper v gui.cpp zaokrúhľuje hodnotu na 1 desatinné miesto PRED porovnaním s cache (`cache_t`, `cache_h`, `cache_v_t`, `cache_v_h`, `cache_wt`) - redraw sa teda spustí len keď sa reálne zmení zobrazený text, nie pri šume v ďalších desatinách.
- Overené užívateľom ako vyriešené ("uz je to kludnejsie").
[/UPDATE-REDPINS-STATE]

[UPDATE-REDPINS-STATE]
Dátum: 2026-06-29 (Claude Code session)
Fáza: ESP32 Firmware (Sense) - Pridaná obrazovka COMPARE + Refaktoring gui.cpp na moduly

1. Nová obrazovka COMPARE (gui.cpp, screen index 0, hned po boote):
- 2x2 grid: horný riadok teplota senzor (BME280) vs API (OpenWeatherMap), spodný riadok vlhkosť senzor vs API. Posunulo všetky ostatné obrazovky o +1 index (SENSORS=1, WEATHER=2, ATMOSPHERE=3, TEMPERATURE=4, SYSTEM=5), rotácia zmenená z %5 na %6.
- Presun ULOZISKO sekcie na SYSTEM obrazovke z ľavého do pravého stĺpca (ľavý stĺpec pri plnom obsadení presahoval LCD_V_RES=172px, ULOZISKO bolo mimo viditeľnej oblasti displeja).

2. Identifikovaný (nie opravený) bug v auto-jase displeja:
- `map_lux_to_backlight_percent()` v gui.cpp má skutočný strop 88%, nikdy nedosiahne 100%.
- Trojvrstvové vyhladzovanie (EMA filtered_lux 80/20 + hystéréza dead-zone + slew-rate limit ±3%/s) sa kumuluje, takže pri náhlej zmene osvetlenia trvá ~20-30s, kým sa to prejaví na displeji - pôsobí to ako "nereaguje". Užívateľ rozhodol opraviť to v samostatnej session, nie počas refaktoringu.

3. VEĽKÝ REFAKTORING: gui.cpp rozdelený z 1843 na 613 riadkov (orchestrácia) + 13 nových modulov:
- `gui_state.h` - `struct GuiState` (zdieľaný stav) + button pin defines.
- `gui_primitives.h/.cpp` - kresliace primitívy (gui_draw_string/rect/...), DMA buffery, blend_color.
- `gui_helpers.h/.cpp` - round1, format_sensor_val, get_absolute_humidity, get_weather_desc, map_lux_to_backlight_percent, ikony i_thermometer/i_drop.
- `gui_screen_compare/sensors/weather/atmosphere/system/graph.h/.cpp` - jeden modul na obrazovku, signatúra `void gui_draw_screen_X(GuiState& s, uint32_t now_ms)`.
- Architektúra: plain struct + free funkcie (žiadne virtuály, žiadna nová heap alokácia) - zachováva projektový štýl "C s structami". 8 pôvodných lambda closures v gui_task prevedené na static free funkcie.
- DÔLEŽITÉ pre budúce zmeny: `src/CMakeLists.txt` má EXPLICITNÝ zoznam SRCS, nie glob - nový .cpp súbor sa musí pridať manuálne, inak sa ticho nepreloží.
- Refaktoring robený postupne po krokoch (extrakcia primitives → helpers → GuiState in-place → SYSTEM → WEATHER → ATMOSPHERE → COMPARE → SENSORS → GRAPH → cleanup), s build+flash+smoke-test po každom kroku. Žiadna funkčná zmena, čisto štrukturálne. Užívateľ overil každý krok na fyzickom zariadení.
[/UPDATE-REDPINS-STATE]
