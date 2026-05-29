# 📄 Štruktúra CSV Dát (Redpins Sense -> Core)

Tento dokument definuje presný formát CSV súborov, ktoré ESP32 (Sense) streamuje do Android aplikácie (Core) cez BLE `DATA_STREAM` (charakteristika A104).

## 📌 Všeobecné pravidlá pre Android Parser
- **Oddeľovač stĺpcov:** Bodkočiarka (`;`)
- **Kódovanie:** UTF-8
- **Desatinné čísla:** Vždy používajú **bodku** (`.`), nikdy nie čiarku. V Jave/Kotline parsujte výhradne cez `Locale.US` (napr. `String.format(Locale.US, ...)` alebo `Float.parseFloat()`).
- **Koniec prenosu:** Oznámený prijatím bajtu `0xFF` (podľa Rozhodnutia 5.4). Vtedy Android aplikácia uzavrie Room DB transakciu.
- **Robustnosť:** Parser musí ignorovať prázdne riadky a riadky, ktoré po volaní `.split(";")` nemajú očakávaný počet prvkov.

---

## 1. Senzorové Dáta (`/sensor.csv`)
Surové dáta čítané priamo z hardvérových senzorov (BMP085, DHT).
**Hlavička v súbore:** `timestamp;temp;hum;press`

| Index | Názov stĺpca | Java Typ | Popis | Príklad |
| :---: | :--- | :--- | :--- | :--- |
| 0 | `timestamp` | `long` | Unix epoch time v sekundách. | `1715421200` |
| 1 | `temp` | `float` | Teplota v °C (1 desatinné miesto). | `23.5` |
| 2 | `hum` | `float` | Vlhkosť v % (1 desatinné miesto). | `48.2` |
| 3 | `press` | `int` | Atmosférický tlak v hPa (celé číslo). Ak senzor zlyhá, posiela sa fallback `1013`. | `1013` |

**Príklad riadku z prúdu:**
`1715421200;23.5;48.2;1013`

---

## 2. Dáta Počasia z API (`/weather.csv`)
Dáta stiahnuté z externého meteorologického API a cachované v lokálnej pamäti.
**Hlavička v súbore:** `timestamp;city;temp;feels_like;pressure;humidity;wind_speed;description`

| Index | Názov stĺpca | Java Typ | Popis | Príklad |
| :---: | :--- | :--- | :--- | :--- |
| 0 | `timestamp` | `long` | Unix epoch time v sekundách. | `1715425000` |
| 1 | `city` | `String` | Názov mesta / lokality (bez oddeľovačov `;`). | `Bratislava` |
| 2 | `temp` | `float` | Aktuálna teplota v °C (1 desatinné miesto). | `18.2` |
| 3 | `feels_like`| `float` | Pocitová teplota v °C (1 desatinné miesto). | `17.5` |
| 4 | `pressure` | `int` | Tlak v hPa (celé číslo). | `1008` |
| 5 | `humidity` | `int` | Vlhkosť vzduchu v % (celé číslo). | `65` |
| 6 | `wind_speed`| `float` | Rýchlosť vetra v m/s (1 desatinné miesto). | `4.2` |
| 7 | `description`| `String` | Textový popis počasia (prípadne kód ikony, napr. "Clear sky"). | `Clear sky` |

**Príklad riadku z prúdu:**
`1715425000;Bratislava;18.2;17.5;1008;65;4.2;Clear sky`

---
*Poznámka pre Core tím: Prvý riadok každého súboru obsahuje textovú hlavičku. Váš parser by mal prvý prijatý riadok textu buď preskočiť, alebo overiť, či neobsahuje slovo "timestamp" predtým, než začne parsovať čísla.*