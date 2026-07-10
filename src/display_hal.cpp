#include "display_hal.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "DISP_HAL";

esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static bool s_backlight_ready = false;
static uint8_t s_backlight_percent = 31;

// --- OPRAVENÉ PINY (Z originálneho Waveshare dema) ---
#define WS_MOSI 6
#define WS_CLK 7
#define WS_CS 14
#define WS_DC 15
#define WS_RST 21
#define WS_BCKL 22
#define LCD_DRAW_BUFF_LINES 20

static uint32_t percent_to_duty(uint8_t percent) {
    if (percent > 100) percent = 100;
    return ((uint32_t)percent * 255U) / 100U;
}

esp_err_t display_hal_init(void) {
    ESP_LOGI(TAG, "Inicializácia SPI zbernice");

    // GPIO4 je pouzity ako I2C SDA pre externe senzory (BME280/BH1750).
    // Nesmie sa tu prepinat do OUTPUT modu, inak sa zablokuje I2C zbernica.

    // Zanshin: Pre-inicializácia LCD CS (Chip Select).
    // Zabezpečí, že ST7789 ignoruje akýkoľvek šum na zbernici počas bootu,
    // kým ESP-IDF neprevezme plnú kontrolu nad asynchrónnym SPI prenosom.
    gpio_reset_pin((gpio_num_t)WS_CS);
    gpio_set_direction((gpio_num_t)WS_CS, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)WS_CS, 1);  // HIGH = Neaktívny

    spi_bus_config_t buscfg = {};
    buscfg.sclk_io_num = WS_CLK;
    buscfg.mosi_io_num = WS_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = LCD_TRANSFER_SIZE;

    // Používame SPI2_HOST, pretože SPI1_HOST je obsadený pre Flash/LittleFS
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // Necháme zbernicu elektricky stabilizovať (ak je vložená SD karta)
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_LOGI(TAG, "Pripojenie LCD k SPI zbernici (ST7789)");
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.dc_gpio_num = WS_DC;
    io_config.cs_gpio_num = WS_CS;
    io_config.pclk_hz = 12 * 1000 * 1000;  // Z dema: 12 MHz
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    io_config.spi_mode = 0;
    io_config.trans_queue_depth = 10;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_config, &s_io_handle));

    ESP_LOGI(TAG, "Inštalácia ovládača ST7789");
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = WS_RST;
    panel_config.rgb_endian = LCD_RGB_ENDIAN_RGB;
    panel_config.bits_per_pixel = 16;

    ESP_ERROR_CHECK(
        esp_lcd_new_panel_st7789(s_io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

    // --- ZANSHIN FIX: Vernon ST7789T Magické registre ---
    ESP_LOGI(TAG, "Odosielam Vernon ST7789T power/gamma registre...");
    esp_lcd_panel_io_tx_param(s_io_handle, 0xB0, (uint8_t[]){0x00, 0xE8}, 2);
    esp_lcd_panel_io_tx_param(s_io_handle, 0xB2,
                              (uint8_t[]){0x0C, 0x0C, 0x00, 0x33, 0x33}, 5);
    esp_lcd_panel_io_tx_param(s_io_handle, 0xB7, (uint8_t[]){0x75}, 1);
    esp_lcd_panel_io_tx_param(s_io_handle, 0xBB, (uint8_t[]){0x1A}, 1);
    esp_lcd_panel_io_tx_param(s_io_handle, 0xC0, (uint8_t[]){0x80}, 1);
    esp_lcd_panel_io_tx_param(s_io_handle, 0xC2, (uint8_t[]){0x01, 0xFF}, 2);
    esp_lcd_panel_io_tx_param(s_io_handle, 0xC3, (uint8_t[]){0x13}, 1);
    esp_lcd_panel_io_tx_param(s_io_handle, 0xC4, (uint8_t[]){0x20}, 1);
    esp_lcd_panel_io_tx_param(s_io_handle, 0xC6, (uint8_t[]){0x0F}, 1);
    esp_lcd_panel_io_tx_param(s_io_handle, 0xD0, (uint8_t[]){0xA4, 0xA1}, 2);
    esp_lcd_panel_io_tx_param(
        s_io_handle, 0xE0,
        (uint8_t[]){0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44, 0x4E, 0x3A,
                    0x17, 0x18, 0x2F, 0x30},
        14);
    esp_lcd_panel_io_tx_param(
        s_io_handle, 0xE1,
        (uint8_t[]){0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4D, 0x38,
                    0x15, 0x16, 0x2C, 0x2E},
        14);
    esp_lcd_panel_io_tx_param(s_io_handle, 0x21, NULL,
                              0);  // INVON (Invert color hardvérovo)
    // ---------------------------------------------------

    // Zanshin: Pre Krajinu (Landscape) sa osi vymenia.
    // 172px šírka sa stáva výškou, takže 34px offset presúvame na Y os.
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 34));

    // Rotácia na šírku (Landscape)
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, true));

    // Zanshin: Finálna kalibrácia orientácie. Os X funguje perfektne (zľava
    // doprava), takže ju nechávame. Os Y (riadky a orientácia písmen) bola
    // naopak, takže ju invertujeme.
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));

    ESP_LOGI(TAG,
             "Zapínam podsvietenie (Backlight) cez PWM (redukcia presvitania)");
    // Inicializácia LEDC časovača
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_timer.timer_num = LEDC_TIMER_0;
    ledc_timer.duty_resolution = LEDC_TIMER_8_BIT;  // Rozsah jasu 0-255
    ledc_timer.freq_hz = 4000;  // 4 kHz pre čistý obraz bez blikania
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&ledc_timer);

    // Inicializácia LEDC kanála pre pin displeja
    ledc_channel_config_t ledc_channel = {};
    ledc_channel.speed_mode = LEDC_LOW_SPEED_MODE;
    ledc_channel.channel = LEDC_CHANNEL_0;
    ledc_channel.timer_sel = LEDC_TIMER_0;
    ledc_channel.intr_type = LEDC_INTR_DISABLE;
    ledc_channel.gpio_num = WS_BCKL;
    ledc_channel.duty = 80;  // Hodnota 80 z 255 (cca 31 % jasu). Dobrý
                             // kompromis medzi jasom a presvitaním.
    ledc_channel.hpoint = 0;
    ledc_channel_config(&ledc_channel);
    s_backlight_ready = true;
    s_backlight_percent = 31;

    // Zapnutie displeja (exit z režimu spánku)
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_LOGI(TAG, "LCD HAL inicializovaný úspešne.");
    return ESP_OK;
}

void display_hal_set_backlight_percent(uint8_t percent) {
    if (!s_backlight_ready) return;
    if (percent > 100) percent = 100;
    if (percent == s_backlight_percent) return;

    uint32_t duty = percent_to_duty(percent);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    s_backlight_percent = percent;
}

uint8_t display_hal_get_backlight_percent(void) { return s_backlight_percent; }

// MODE_BALANCED: znizenie obnovovacej frekvencie ST7789 na ~30Hz priamym
// zapisom do FRCTRL2 (0xC6), aby sa odlahcil interny oscilator displeja.
// eco=false obnovi povodnu hodnotu z "Vernon" inicializacnej sekvencie.
void display_hal_set_eco_framerate(bool eco) {
    if (!s_io_handle) return;
    uint8_t frctrl2 = eco ? 0x1F : 0x0F;
    esp_lcd_panel_io_tx_param(s_io_handle, 0xC6, &frctrl2, 1);
}

// MODE_LONG_LIFE: pred esp_deep_sleep_start() posle do ST7789 DISPOFF a
// SLPIN, aby panel neodoberal prud pocas hlbokeho spanku. Displej sa po
// prebudeni (studenom starte aj fast-wake) reinicializuje nanovo v
// display_hal_init(), takze netreba explicitny "wake" pandant.
void display_hal_enter_display_sleep(void) {
    if (!s_io_handle) return;
    esp_lcd_panel_io_tx_param(s_io_handle, 0x28, NULL, 0);  // DISPOFF
    esp_lcd_panel_io_tx_param(s_io_handle, 0x10, NULL, 0);  // SLPIN
}

// Zanshin: Globálna funkcia na vymazanie/výplň obrazovky (vyžadovaná pre
// gui_task)
extern "C" void display_clear(uint16_t color) {
    if (!panel_handle) return;

    // Ochrana pred asynchrónnym DMA pretečením (static)
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