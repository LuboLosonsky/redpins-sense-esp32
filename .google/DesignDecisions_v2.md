# 🏛️ ARCHITEKTONICKÉ ROZHODNUTIA (RCP v2.0)

## 6. Autonómia a Lokálna Identita (Multi-Node Strategy)
**Dátum:** 08. 05. 2026
**Priorita:** Škálovateľnosť a komerčný potenciál

* **Rozhodnutie 6.1: Plná autonómia uzlov (Edge Computing):**
    - Každé zariadenie *Redpins Sense* musí fungovať nezávisle od prítomnosti mobilnej aplikácie.
    - Úlohy (napr. sťahovanie OpenWeatherAPI dát) sa vykonávajú periodicky na základe interných časovačov (Ticker) v C++.
    - Konfigurácia (WiFi, GPS, Alias) je perzistentne uložená v `config.json` v LittleFS.

* **Rozhodnutie 6.2: Location Handshake (GPS & Geocoding):**
    - S25 Ultra (Core) slúži ako „poskytovateľ polohy“. Pri prvom nastavení alebo zmene lokality Core odošle súradnice do Sense.
    - Príkaz `0x31` (SET_COORDS): `[0x31][4B float LAT][4B float LON]` (Little Endian).
    - Príkaz `0x32` (SET_ALIAS): `[0x32][1B len][String Name]` (napr. "Chata", "Zilina").

* **Rozhodnutie 6.3: Transparentnosť konfigurácie (Obojsmerný Sync):**
    - Sense musí reportovať svoje aktuálne nastavenia v metadátach.
    - Rozšírenie JSON formátu v charakteristike `A102` (Device Info) o polia: `alias`, `lat`, `lon`.
    - Android Core pri každom pripojení overí súlad (SSoT je zariadenie) a aktualizuje lokálnu Room DB podľa dát zo Sense.

* **Rozhodnutie 6.4: Kalibrácia senzorov (Precision Engineering):**
    - Príkaz `0x26` (SET_CALIBRATION): `[0x26][4B float DHT_offset][4B float BMP_offset]`.
    - Ofsety sa ukladajú do LittleFS a aplikujú sa priamo pri čítaní hardvéru pred zápisom do CSV/Telemetrie.

---
*Schválené architektkou a Lubom pre implementáciu (v2.0).*
