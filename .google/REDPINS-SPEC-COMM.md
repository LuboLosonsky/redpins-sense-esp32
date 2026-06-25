# 🚩 REDPINS COMMUNICATION SPECIFICATION (RCP V2.1)

## 1. Identita projektu a Ekosystém
- **Názov:** Redpins Core (Android Orchestrator)
- **Cieľ:** Centrálne riadenie IoT ekosystému (Sense, Vision, Focus, Orbit)
- **Hardvér:** Optimalizované pre Samsung Galaxy S25 Ultra (Android 15, API 35)
- **Kódex:** „Voda“ (efektivita, poddajnosť) & „Zanshin“ (error handling, bdelosť)

### 1.1 Bluetooth Identity & UUIDs
Všetky zariadenia zdieľajú spoločný základ pre efektívnu filtráciu a bezpečnosť.

**Base UUID Prefix:** `FD65XXXX-B3D1-4E52-8B25-45C63B72XXXX`

| Komponent | Prefix | Service UUID | Účel |
| :--- | :--- | :--- | :--- |
| **Redpins Core** | `RP-C-` | N/A (Central) | Android Orchestrator |
| **Redpins Sense** | `RP-S-` | `fd651000-b3d1-4e52-8b25-45c63b72a100` | Environmentálne senzory |
| **Redpins Vision** | `RP-V-` | `fd652000-b3d1-4e52-8b25-45c63b72a200` | Kamera a AI analýza |
| **Redpins Focus** | `RP-F-` | `fd653000-b3d1-4e52-8b25-45c63b72a300` | Productivity Terminal |

---

## 2. Pravidlá pre Discovery (Advertising)
Aby bol Android schopný zariadenie nájsť v režime nízkej spotreby a spoľahlivo ho identifikovať, **MUSÍ** Advertising paket (Adv Data) obsahovať:

1.  **Local Name:** Musí začínať príslušným prefixom (napr. `RP-F-01`).
2.  **Service UUID List:** Musí explicitne obsahovať prislúchajúce **Service UUID** z tabuľky vyššie.
    *   *Poznámka: Ak UUID chýba v Advertising dátach, Android ho pri cielenom skenovaní ignoruje aj keď sa názov zhoduje!*

---

## 3. Charakteristiky a Protokol (RCP)

V rámci každej služby (A100) definujeme štandardné charakteristiky:

| Meno | UUID Suffix | Vlastnosti | Formát dát | Popis |
| :--- | :--- | :--- | :--- | :--- |
| **Telemetry** | `A101` | READ, NOTIFY | Binary (LE) | Real-time dáta senzorov |
| **Device Info** | `A102` | READ | JSON String | Diagnostické metadáta |
| **Command** | `A103` | WRITE | Binary | Riadiace príkazy |
| **Data Stream** | `A104` | NOTIFY | Binary / Text | Masívne prenosy (CSV/JSON) |

### 3.1 Spoločné Systémové Príkazy (Charakteristika A103)
Zjednotená mapa príkazov pre nízkoúrovňové riadenie všetkých uzlov:

| Hex | Príkaz | Payload / Popis |
| :--- | :--- | :--- |
| **Skupina 0x00** | **Systém** | |
| `0x01` | **REBOOT** | Okamžitý reštart ESP |
| `0x02` | **FACTORY_RESET** | Vymazanie NVS a súborového systému (LittleFS) |
| `0x03` | **DEEP_SLEEP** | Prechod do režimu hlbokého spánku |
| `0x04` | **SYS_INFO** | Vyžiadanie metadát (odošle JSON do A102) |
| **Skupina 0x10** | **Sieť (WiFi)** | |
| `0x11` | **WIFI_SCAN** | Spustí sken a výsledky pošle cez A104 (JSON) |
| `0x12` | **WIFI_CONNECT** | `[0x12][lenSSID][SSID][lenPWD][PWD]` |
| `0x13` | **WIFI_DISCONNECT** | Odpojenie od aktuálnej siete |
| `0x14` | **WIFI_LIST_SAVED** | Zoznam uložených sietí |
| **Skupina 0x20** | **Senzory** | |
| `0x21` | **SENSOR_FORCE** | Vynútenie okamžitého čítania a notifikácie |
| `0x22` | **WEATHER_FETCH** | Vynúti update počasia z API |
| `0x23` | **CSV_DUMP_SENS** | Spustí prenos historických dát senzorov cez A104 |
| `0x24` | **CSV_DUMP_WXT** | Spustí prenos dát počasia cez A104 |
| `0x25` | **LOG_CLEAR** | Vymazanie historických logov |
| `0x26` | **CALIBRATE** | `[0x26][4B float DHT_off][4B float BMP_off]` (Little Endian) |
| **Skupina 0x30** | **Lokalita** | |
| `0x31` | **SET_COORDS** | `[0x31][4B float Lat][4B float Lon]` (Little Endian) |
| `0x32` | **SET_ALIAS** | `[0x32][1B len][Alias String]` |

---

## 4. Špecifiká komponentov

### 4.1 Redpins Sense (RPS)
- **A101 Telemetry:** `[4B float Temp][4B float Hum][2B uint16 Pres]` (Všetko Little Endian).
- **Metadata (A102):** `{"hw":"S3-V1", "bat":85, "free_heap":124484, "uptime_s":2047, "wifi_connected":true, "ip":"192.168.0.102"}`

### 4.2 Redpins Focus (RPF)
Využíva JSON payloady pre komplexné operácie cez A103/A104.
- **Pomodoro Config:** `{"id":"RPF01", "cmd":"set_cfg", "work":25, "break":5}`
- **Vocab Push:** `{"id":"RPF01", "cmd":"push_vocab", "it":"Libertà", "sk":"Sloboda", "tag":"abstract"}`
- **Display Update:** `{"cmd":"update_wx", "temp":22.5, "hum":45}`
- **Notification Overlay:** `{"cmd":"show_notif", "msg":"Niekto zvoní!", "dur":5}`

---

## 5. Štandard pre Úložisko (Redpins Storage)
Zariadenia s SD kartou (Focus, Vision) dodržiavajú túto štruktúru:

- `/system/config.json` - Wi-Fi a Auth údaje.
- `/data/vocab/` - Slovníky (napre `it_sk.csv` - UTF-8, separátor `;`).
- `/data/logs/` - Logy udalostí (napre `focus.log` - `TIMESTAMP;EVENT;VALUE;NOTE`).
- `/data/assets/` - Grafika a fonty.

---
*Posledná aktualizácia: 2026-05-12 (Zanshin Update V2.1)*

## **6\. Architektúra Dátového Motoru & Delta Synchronizácie (RCP v2.1)**

**Priorita:** Extrémna optimalizácia synchronizácie a šetrenie zdrojov MCU (ESP32-S3).

### **6.1 Protokol Čiastočného Sťahovania (Delta Sync)**

Android Core už pri každom pripojení nesťahuje celú históriu. Príkazy pre vyžiadanie historických dát prijímajú voliteľný 4-bajtový parameter (Unix Timestamp), ktorý definuje bod začiatku prenosu.

* **Identifikátory príkazov:**  
  * 0x23 – Žiadosť o historické dáta senzorov (Senzory Core)  
  * 0x24 – Žiadosť o historické dáta meteostanice (Počasie)

#### **Štruktúra požiadavky (Client \-\> ESP32-S3)**

Dĺžka payloadu požiadavky je fixne 4 bajty.

| Bajt offset | Typ | Význam | Poznámka |
| :---- | :---- | :---- | :---- |
| 0x00 \- 0x03 | uint32\_t | Unix Timestamp (odkedy) | **Little Endian**. Ak je hodnota 0x00000000, ESP32 streamuje celý súbor od začiatku. |

#### **Štruktúra odpovede (ESP32-S3 \-\> Client)**

Každý stream začína 5-bajtovou hlavičkou, po ktorej nasledujú surové dáta v MTU blokoch.

| Bajt offset | Typ | Význam | Hodnota / Popis |
| :---- | :---- | :---- | :---- |
| 0x00 | uint8\_t | Potvrdenie príkazu | 0xA3 (pre 0x23) alebo 0xA4 (pre 0x24) |
| 0x01 \- 0x04 | uint32\_t | Celková dĺžka dát | Veľkosť payloadu v bajtoch (Little Endian). Ak nie sú nové dáta, hodnota je 0\. |
| 0x05+ | uint8\_t\[\] | Dátový stream | Surové bajty (CSV/Binary) zo súborového systému (LittleFS/SD). |

### **6.2 O(1) Zložitosť pre ESP32-S3 (Optimalizácia RAM)**

Pre zachovanie plynulosti procesora (renderovanie GUI, plynulosť RTOS taskov) sa implementuje stratégia priameho prístupu k súborom bez zbytočnej réžie.

1. **Rozhodnutie 6.1 (Čiastočné sťahovanie):** ESP32 na základe prijatého timestampu nájde presnú bajtovú pozíciu (offset) v súbore a streamuje len nové záznamy. Tým sa radikálne znižuje objem prenášaných dát cez BLE.  
2. **Rozhodnutie 6.2 (O(1) prístup):** V RAM sa nesmie iterovať celým súborom. ESP32 preskenuje bloky súboru iba jednorazovo (napr. pri prvom štarte alebo indexácii), nájde pozíciu a použije priamy file.seek(offset) pre okamžitý skok k novým dátam.  
3. **Implementácia:** Zvyšok dát sa streamuje ako surové bajty priamo do BLE stacku bez potreby alokovať veľké buffery v RAM.

Lineárne vyhľadávanie je nahradené binárnym vyhľadávaním nad súborom (v prípade fixných záznamov) alebo indexovou tabuľkou, čím sa dosahuje predvídateľná odozva systému bez ohľadu na veľkosť logovacieho súboru.

###
---
*Posledná aktualizácia: 2026-05-17 (Zanshin Update V2.1)*

## **7. Striktná Atomizácia a Sebaidentifikácia (RCP V2.3)**

**Cieľ:** Odstránenie stavovej nejednoznačnosti a podpora pre multi-device orchestráciu.

### **7.1 Striktná identifikácia streamu (Stream Tagging)**

Každý prúd dát na charakteristike **A104** musí začínať unikátnou binárnou hlavičkou (Mode Byte), ktorá nahrádza generický identifikátor `0xFE`.

| Mode Byte | Účel | Formát |
| :---- | :---- | :---- |
| **0xA3** | Local Sensor CSV Dump | CSV (ts;t;h;p;b) |
| **0xA4** | Weather API CSV Dump | CSV (ts;city;t;...) |
| **0xA5** | System Logs / Debug Dump | Text / CSV |
| **0xFD** | System JSON (Info/WiFi) | JSON String |

### **7.2 Povinný Binary Envelope (Data Framing)**

Každý prenos musí byť zabalený do fixnej 5-bajtovej obálky pre presné určenie konca prenosu bez potreby EOF terminátorov.

**Štruktúra hlavičky (Client <- ESP32):**

| Bajt offset | Typ | Význam | Poznámka |
| :---- | :---- | :---- | :---- |
| 0x00 | uint8\_t | Mode Byte | Pozri tabuľku 7.1 |
| 0x01 \- 0x04 | uint32\_t | Payload Size | Celková dĺžka dát v bajtoch (**Little Endian**) |

### **7.3 Časová integrita (NTP Guard)**

ESP32 **nesmie** odosielať historické CSV dáta (módy 0xA3, 0xA4), kým nemá úspešne synchronizovaný systémový čas cez NTP.
*   Ak čas nie je synchronizovaný, ESP32 vráti v hlavičke `Payload Size = 0`.
*   Android orchestrátor v tomto stave zobrazí upozornenie „Waiting for Time Sync“.

### **7.4 Verziovaný formát riadku (Dynamic Header)**

Každý CSV stream **MUSÍ** začať riadkom definujúcim stĺpce pre dynamický parsing na strane Androidu.
*   *Príklad SENS:* `ts;t;h;p;b` (timestamp, temp, hum, pres, bat)
*   *Príklad WXT:* `ts;city;t;h;p`

### **7.5 Prevencia kolízií**

*   ESP32 musí pred začatím nového streamu (napr. po príkaze 0x23) násilne ukončiť akýkoľvek prebiehajúci prenos na charakteristike A104.
*   Zásada: Jedna charakteristika = Jeden logický tok v reálnom čase.

---
*Posledná aktualizácia: 2026-06-08 (RCP V2.3 - Strict Integrity)*
