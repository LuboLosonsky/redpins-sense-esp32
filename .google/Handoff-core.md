# 🚩 REDPINS CORE - HANDOFF REPORT (V2.3)

## 📌 Identita projektu
- **Názov:** Redpins Core (Android Orchestrator)
- **Hardvér:** Samsung Galaxy S25 Ultra (Android 15, API 35)
- **Stav:** **RCP V2.3 Stabilizácia.** Plná podpora pre ESP32-C6 a nepriestrelná dátová integrita.

## 🚀 Aktuálny stav implementácie

### 1. Protokol RCP V2.3 (Sense C6 Readiness)
- **Binary Framing:** Implementovaná podpora pre 5-bajtové obálky `[Mode][Length LE]`. Streamovanie je teraz "Raw" bez zbytočnej réžie na každý paket.
- **Explicit Tagging:** Striktné rozlíšenie módov `0xA3` (SENS) a `0xA4` (WXT). Odstránená ambiguita pri 5-stĺpcových formátoch.
- **NTP Guard Awareness:** Orchestrátor korektne spracováva nulovú dĺžku streamu, ak ESP32 nemá synchronizovaný čas.
- **Naked JSON Support:** Pridaná reasemblácia JSON metadát priamo z Data Stream charakteristiky (A104) bez nutnosti hlavičiek.

### 2. Dátová Integrita & SSoT
- **Context-Aware Routing:** Android si pamätá typ požiadavky a smeruje prichádzajúce CSV dáta do správnej tabuľky aj pri "Legacy" hlavičkách.
- **Hardware Identification:** Pridané sledovanie **Hardware Revision** (napr. `C6-V1`). Informácia je perzistovaná v DB a zobrazená v zozname zariadení.
- **Robust Parsing:** Implementovaný `safeParseFloat` so sanitáciou znakov; ochrana proti kolíziám dát počas heartbeatu.

### 3. Profesionálne Logovanie (Silent Operator)
- **Cleanup:** Odstránený technický šum o jednotlivých paketoch.
- **Reporting:** Pridaná sumarizácia po každom streame (Total chars, Record count) a výpis posledných 3 vzoriek dát pre rýchlu kontrolu validity.
- **Standardization:** Všetky systémové logy orchestrátora prepnuté do angličtiny.

## 📂 Technické detaily
- **Database:** Room V9 (pridané stĺpce `hardwareRevision` a `firmwareVersion`).
- **Batching:** Implementovaný dávkový zápis (Batching) aj pre Weather API dáta (zvýšená reaktivita grafov).
- **Architecture:** Zjednotený `BaseDeviceViewModel` s predvolenými implementáciami callbackov.

## ⏭️ Najbližšie kroky
1. **Focus Dashboard:** Implementácia vizualizácie Pomodoro cyklov pre RPF moduly.
2. **Vision Stream:** Príprava na binárny prenos obrazu z ESP32-S3.
3. **Orbit Sync:** Prvé testy synchronizácie lokálnej DB s cloudom.

---
*Posledná aktualizácia: 2026-06-08 (Lubo & Mentorka Gemini)*
