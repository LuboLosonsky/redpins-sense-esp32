#include "display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_cpu.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "DISPLAY";

// --- KONFIGURÁCIA PINOV (Uprav podľa schémy k tvojej WS doske) ---
// Upozornenie: GPIO 18 používaš pre DHT11, takže tu nesmie byť!
#define LCD_HOST SPI2_HOST
#define LCD_PIN_MOSI 6  // Z dema: EXAMPLE_PIN_NUM_MOSI
#define LCD_PIN_CLK \
    7  // Z dema: EXAMPLE_PIN_NUM_SCLK (Reverzne voči Waveshare!)
#define LCD_PIN_CS 14        // LCD Chip Select (Interne)
#define LCD_PIN_DC 15        // Z dema: EXAMPLE_PIN_NUM_LCD_DC
#define LCD_PIN_RST 21       // Z dema: EXAMPLE_PIN_NUM_LCD_RST
#define LCD_PIN_BK_LIGHT 22  // Z dema: EXAMPLE_PIN_NUM_BK_LIGHT

// Pin pre hardvérovú deaktiváciu SD karty (Zamedzenie SPI konfliktu)
#define SD_PIN_CS 4

#define LCD_H_RES 172
#define LCD_V_RES 320

// Parciálny DMA buffer (20 riadkov = 172 * 20 * 2 byty = 6880 bytov)
// Toto je náš kompromis pre šetrenie SRAM.
#define LCD_DRAW_BUFF_LINES 20

static esp_lcd_panel_handle_t panel_handle = NULL;

esp_err_t display_init(void) {
    ESP_LOGI(TAG, "Inicializujem SPI pre originál Waveshare (ST7789) s DMA...");

    // 0. Zanshin: Hardvérová arbitráž SPI zbernice
    // Odpojenie SD karty z linky (CS HIGH)
    gpio_reset_pin((gpio_num_t)SD_PIN_CS);
    gpio_set_direction((gpio_num_t)SD_PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)SD_PIN_CS, 1);

    // Uvoľnenie defaultných JTAG pinov 14 (MTMS) a 15 (MTDO) pre potreby GPIO
    gpio_reset_pin((gpio_num_t)LCD_PIN_CS);
    gpio_reset_pin((gpio_num_t)LCD_PIN_DC);
    // Úmyselne nenastavujeme stavy (HIGH/LOW), nechávame to na esp_lcd driver

    // 1. Nastavenie podsvietenia (Ak je pripojené na GPIO)
    if (LCD_PIN_BK_LIGHT >= 0) {
        gpio_config_t bk_gpio_config = {
            .pin_bit_mask = (1ULL << LCD_PIN_BK_LIGHT),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&bk_gpio_config);
        gpio_set_level((gpio_num_t)LCD_PIN_BK_LIGHT, 1);  // Zapnúť podsvietenie
    }

    // 2. Inicializácia SPI zbernice
    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = LCD_PIN_CLK;
    buscfg.mosi_io_num = LCD_PIN_MOSI;
    buscfg.miso_io_num = -1;  // Displej iba prijíma dáta
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    // max_transfer_sz je veľkosť nášho parciálneho buffra
    buscfg.max_transfer_sz = LCD_H_RES * LCD_DRAW_BUFF_LINES * sizeof(uint16_t);

    esp_err_t ret = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Zlyhala inicializácia SPI zbernice!");
        return ret;
    }

    // 3. Konfigurácia IO panela
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_io_num = LCD_PIN_DC;
    io_config.cs_io_num = LCD_PIN_CS;
    io_config.pclk_hz = 12 * 1000 * 1000;  // Z dema: EXAMPLE_LCD_PIXEL_CLOCK_HZ
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode =
        0;  // Zanshin: Návrat k natívnemu Mode 0, Mód 3 blokoval komunikáciu
    io_config.trans_queue_depth = 10;

    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                   &io_config, &io_handle);
    if (ret != ESP_OK) return ret;

    // 4. Inicializácia ST7789
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = LCD_PIN_RST;
    panel_config.rgb_endian = LCD_RGB_ENDIAN_RGB;
    panel_config.bits_per_pixel = 16;

    // Použijeme ST7789 driver ako základ pre MIPI DCS príkazy (RAMWR a pod.)
    ret = esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
    if (ret != ESP_OK) return ret;

    // Inicializácia a prebudenie displeja
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);

    // --- ZANSHIN FIX: Vernon ST7789T Magické registre priamo z Dema ---
    ESP_LOGI(TAG, "Odosielam Vernon ST7789T power/gamma registre...");
    esp_lcd_panel_io_tx_param(io_handle, 0xB0, (uint8_t[]){0x00, 0xE8}, 2);
    esp_lcd_panel_io_tx_param(io_handle, 0xB2,
                              (uint8_t[]){0x0C, 0x0C, 0x00, 0x33, 0x33}, 5);
    esp_lcd_panel_io_tx_param(io_handle, 0xB7, (uint8_t[]){0x75}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xBB, (uint8_t[]){0x1A}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC0, (uint8_t[]){0x80}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC2, (uint8_t[]){0x01, 0xFF}, 2);
    esp_lcd_panel_io_tx_param(io_handle, 0xC3, (uint8_t[]){0x13}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC4, (uint8_t[]){0x20}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xC6, (uint8_t[]){0x0F}, 1);
    esp_lcd_panel_io_tx_param(io_handle, 0xD0, (uint8_t[]){0xA4, 0xA1}, 2);
    esp_lcd_panel_io_tx_param(
        io_handle, 0xE0,
        (uint8_t[]){0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44, 0x4E, 0x3A,
                    0x17, 0x18, 0x2F, 0x30},
        14);
    esp_lcd_panel_io_tx_param(
        io_handle, 0xE1,
        (uint8_t[]){0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4D, 0x38,
                    0x15, 0x16, 0x2C, 0x2E},
        14);
    esp_lcd_panel_io_tx_param(io_handle, 0x21, NULL, 0);  // INVON
    // ------------------------------------------------------------------

    // DÔLEŽITÉ pre JD9853 aj ST7789 (fyzicky 172x320 vs 240x320 GRAM)
    esp_lcd_panel_set_gap(panel_handle, 34, 0);

    esp_lcd_panel_invert_color(panel_handle, true);

    // Nastavenie rotácie (prispôsob si podľa orientácie krabičky)
    esp_lcd_panel_swap_xy(panel_handle, false);
    esp_lcd_panel_mirror(panel_handle, false, false);

    // Zapnutie vykresľovania
    esp_lcd_panel_disp_on_off(panel_handle, true);

    ESP_LOGI(TAG, "Displej úspešne inicializovaný. Vyplňujem na červeno...");
    display_clear(0xF800);  // 0xF800 je RGB565 Červená, displej by mal okamžite
                            // svietiť farbou.

    return ESP_OK;
}

void display_clear(uint16_t color) {
    if (!panel_handle) return;

    // ZANSHIN FIX: Asynchrónna pasca pamäte!
    // Tento buffer nesmieme dealokovať, kým ho DMA radič fyzicky neodošle.
    // Použijeme statickú alokáciu (jednorazovo), aby RAM zostala pre DMA
    // platná.
    static uint16_t* clear_buf = NULL;
    if (!clear_buf) {
        clear_buf = (uint16_t*)heap_caps_malloc(
            LCD_H_RES * LCD_DRAW_BUFF_LINES * sizeof(uint16_t), MALLOC_CAP_DMA);
    }
    if (!clear_buf) return;

    for (int i = 0; i < LCD_H_RES * LCD_DRAW_BUFF_LINES; i++) {
        clear_buf[i] = __builtin_bswap16(color);
    }

    for (int y = 0; y < LCD_V_RES; y += LCD_DRAW_BUFF_LINES) {
        esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES,
                                  y + LCD_DRAW_BUFF_LINES, clear_buf);
    }
}