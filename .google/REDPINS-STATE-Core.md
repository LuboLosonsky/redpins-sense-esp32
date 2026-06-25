# REDPINS-STATE.md | Current State: 2026-05-23

## 1. ARCHITEKTÚRA SYSTÉMU (SSoT)
- **Primary Orchestrator:** Android Aplikácia (`redpins-core-android`).
- **Vision Node:** ESP32-S3 + OV2640 (V príprave).
- **Sense Node:** ESP32-C6 + Solar (Stabilizované).
- **Focus Node:** ESP32-2432S028R (Stabilizované).

## 2. REPO SETUP - ANDROID CORE
- **UI Standard:** Zjednotený Home Dashboard s "Rich Cards".
- **Visuals:** Custom Bezier Ribbon Glow (Fixed-height dynamic layers).
- **Architecture:** Modular Fragment Navigation (User-configurable).

## 3. STRATEGICKÉ ROZHODNUTIA
- **Personalizované UX:** Používateľ si konfiguruje viditeľnosť hardvérových modulov (Sense/Focus/Vision/Orbit).
- **Zanshin Connectivity:** Orchestrátor aktívne nadväzuje spojenia na pozadí hneď pri štarte pre minimalizáciu latencie v UI.

## 4. AKTUÁLNY SPRINT
- **Cieľ:** Stabilizácia unifikovaného rozhrania a oprava systémových anomálií (Témy/Prepínače).
- **Stav:** **DOKONČENÉ.** Dashboard je oživený, grafy sú prémiové, navigácia je inteligentná.

## 5. NASLEDUJÚCE CIELE
- **Focus Core:** Implementácia detailného Dashboardu pre Focus (Pomodoro stats).
- **Vision Prep:** Skladanie HW komponentov pre kamerové moduly.
