# 📍 REDPINS ESP32 - Status Report (Koniec dňa)

### Čo sme úspešne dokončili (Milestones):
1. **Inteligentný asistent vetrania (Smart Ventilation):** Výpočet absolútnej vlhkosti (g/m³) cez Magnusovu rovnicu priamo na displeji pre porovnanie IN/OUT vzduchu.
2. **Kvalita vonkajšieho ovzdušia (AQI & Smog):** Zber PM2.5 a AQI dát z OWM zaintegrovaný do GUI bez preťaženia RAM.
3. **Lokálna predikcia (Barometrický trend):** (Pripravuje sa) Dynamické šípky trendu tlaku na základe porovnania s offline históriou z LittleFS.
4. **Perzistentná rotácia displeja:** Hardvérové zrkadlenie obrazovky o 180° s uložením do `config.json` a aplikáciou okamžite po štarte (pred boot screenom).

### Aktuálna misia:
Prechod na Android Core. Implementácia `DataStreamHandler`u pre asynchrónne parsovanie RCP V2.3 (zachytenie 5-bajtovej obálky, streamovanie CSV do Room DB).
