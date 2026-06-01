# Špecifikácia pinov a periférií pre dosku Waveshare ESP32-C6-Touch-LCD-1.47

Tento dokument obsahuje kompletný prepis mapovania pinov (pinout) pre vývojovú dosku Waveshare ESP32-C6 s integrovaným 1.47" displejom. Doska využíva zdieľané zbernice (SPI a I2C) medzi bočnými lištami a internými perifériami, čo vyžaduje správny manažment Chip Select (CS) pinov.

---

## 1. Fyzické piny na bočných lištách dosky

Piny sú dostupné na vonkajších otvoroch/lištoch mikrokontroléra.

### Ľavá strana (Zhora nadol)
| Pin / GPIO | Primárna funkcia / Označenie | Poznámka |
| :--- | :--- | :--- |
| **VBUS** | Napájanie z USB (5V) | Vstupno/výstupný napájací pin |
| **GND** | Spoločná zem | |
| **GPIO16** | `UART0_TX` | Sériová komunikácia (Tx) |
| **GPIO17** | `UART0_RX` | Sériová komunikácia (Rx) |
| **RST** | Reset | Tlačidlo / hardvérový reset |
| **GPIO1** | `SPI_SCLK` | **Zdieľaný:** Hardvérové hodiny pre LCD aj SD kartu |
| **GPIO2** | `SPI_MOSI` | **Zdieľaný:** Master Out Slave In pre LCD aj SD kartu |
| **GPIO3** | `SPI_MISO` | **Zdieľaný:** Master In Slave Out pre SD kartu |
| **GPIO4** | `SD_CS` / Voľný | Chip Select pre MicroSD kartu |
| **GPIO5** | `IMU_INT1` / Voľný | Prerušenie 1 pre IMU senzor QMI8658 |
| **GPIO6** | `IMU_INT2` / Voľný | Prerušenie 2 pre IMU senzor QMI8658 |

### Pravá strana (Zhora nadol)
| Pin / GPIO | Primárna funkcia / Označenie | Poznámka |
| :--- | :--- | :--- |
| **VBAT** | Napájanie z batérie | Vstup pre akumulátor |
| **GND** | Spoločná zem | |
| **GND** | Spoločná zem | |
| **3V3** | Napájanie 3.3V | Výstup z interného regulátora |
| **GPIO19** | `I2C_SCL` | **Zdieľaný:** Hodiny pre I2C zbernicu (IMU senzor / Touch) |
| **GPIO18** | `I2C_SDA` | **Zdieľaný:** Dáta pre I2C zbernicu (IMU senzor / Touch) |
| **GPIO13** | `USB_DP` | Natívne USB (D+) |
| **GPIO12** | `USB_DN` | Natívne USB (D-) |
| **GPIO9** | Voľný GPIO | |
| **GPIO8** | Voľný GPIO | |
| **GPIO7** | Voľný GPIO | |

---

## 2. Mapovanie interných periférií

Tieto piny sú interne prepojené priamo na plošnom spoji dosky. V aplikácii je potrebné inicializovať zbernice na týchto konkrétnych číslach.

### A. Displej (LCD)
Displej komunikuje cez hardvérové SPI. Piny `GPIO14`, `15`, `22` a `23` sú skryté vnútri dosky a nie sú vyvedené na bočné lišty.

* **GPIO7** $\rightarrow$ `LCD_CLK` (SPI Clock)
* **GPIO6** $\rightarrow$ `LCD_DIN` (SPI MOSI)
* **GPIO14** $\rightarrow$ `LCD_CS` (Chip Select pre LCD)
* **GPIO15** $\rightarrow$ `LCD_DC` (Data / Command)
* **GPIO21** $\rightarrow$ `LCD_RST` (Hardware Reset displeja)
* **GPIO22** $\rightarrow$ `LCD_BL` (Podsvietenie displeja - Backlight)

> *Poznámka k dotykovej vrstve:* Verzia bez dotykového skla piny pre dotyk (`TP_SDA` / `TP_SCL` na GPIO18/19) nevyužíva, piny slúžia čisto ako štandardná I2C zbernica vyvedená na pravú lištu.

### B. Slot pre MicroSD kartu
SD karta zdieľa s displejom rovnaké piny pre hodiny a dáta. Rozlišujú sa pomocou dedikovaných CS pinov.

* **GPIO6** $\rightarrow$ `SD_CLK` *(Zdieľaný s LCD)*
* **GPIO7** $\rightarrow$ `SD_MOSI` *(Zdieľaný s LCD)*
* **GPIO5** $\rightarrow$ `SD_MISO`
* **GPIO4** $\rightarrow$ `SD_CS` (Chip Select pre SD kartu, vyvedený na ľavú lištu)

### C. IMU Senzor (QMI8658 - Akcelerometer a Gyroskop)
Integrovaný 6-osový senzor polohy.

* **GPIO18** $\rightarrow$ `IMU_SDA` (I2C Data, zdieľaný na pravej lište)
* **GPIO19** $\rightarrow$ `IMU_SCL` (I2C Clock, zdieľaný na pravej lište)
* **GPIO5** $\rightarrow$ `IMU_INT1` (Prerušenie 1, vyvedené na ľavej lište)
* **GPIO6** $\rightarrow$ `IMU_INT2` (Prerušenie 2, vyvedené na ľavej lište)

---

## 3. Architektonický postreh a riešenie problémov s displejom

### Hardvérový konflikt (Zdieľané SPI)
Najčastejší dôvod, prečo displej po zapojení nereaguje alebo zobrazuje len čiernu obrazovku, je **zdieľanie pinov GPIO1 (Clock) a GPIO2 (MOSI) medzi displejom a SD kartou**.

Ak inicializujete iba driver displeja (napr. LovyanGFX, TFT_eSPI) a pin **GPIO4 (`SD_CS`)** zostane v stave s vysokou impedanciou (bude voľne „plávať“), MicroSD slot môže generovať šum a blokovať celú SPI zbernicu.

### Riešenie v kóde
Pri inicializácii firmvéru je nutné **striktne manuálne zadefinovať stavy oboch Chip Select pinov**, aby sa periférie navzájom nerušili:

1. Nastaviť pin **GPIO4 (`SD_CS`) do stavu HIGH** $\rightarrow$ tým sa kompletne deaktivuje SD karta a uvoľní zbernicu.
2. Nastaviť pin **GPIO14 (`LCD_CS`) do stavu LOW** $\rightarrow$ tým sa aktivuje komunikácia výhradne s displejom.

**Príklad ošetrenia v C++ (Arduino / ESP-IDF):**
```cpp
void setup() {
    // 1. Deaktivácia SD karty na zdieľanej zbernici
    pinMode(4, OUTPUT);
    digitalWrite(4, HIGH); 

    // 2. Aktivácia a reset LCD displeja
    pinMode(14, OUTPUT);
    digitalWrite(14, LOW); 

    // 3. Inicializácia samotnej knižnice displeja...
}