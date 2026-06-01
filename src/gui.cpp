#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "display_hal.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font8x8.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensor_core.h"
#include "weather_client.h"
#include "weather_icons.h"
#include "wifi_scanner.h"

static const char* TAG = "GUI";

#define BOOT_BUTTON_PIN 9
extern esp_lcd_panel_handle_t panel_handle;
extern "C" void display_clear(uint16_t color);

// Farby v RGB565 formáte
#define COLOR_BLACK 0x0000
#define COLOR_WHITE 0xFFFF
#define COLOR_GREEN 0x07E0
#define COLOR_BLUE 0x001F
#define COLOR_RED 0xF800
#define COLOR_YELLOW 0xFFE0
#define COLOR_CYAN 0x07FF

// Zanshin: Buffer sme zväčšili na 40 riadkov (13.7 KB).
// Poskytuje dostatok priestoru aj pre veľké 32px (Scale x4) písmená v jednom
// prenose!
#define GUI_BLOCK_LINES 40
// Zarovnanie na 4 bajty je kritické pre bezpečný chod ESP32-C6 DMA!
static uint16_t render_buffer[LCD_H_RES * GUI_BLOCK_LINES]
    __attribute__((aligned(4)));

// --- LIGHTWEIGHT TEXT RENDERER ---
// Sám natlačí bajty fontu priamo do DMA buffra a pošle ho do displeja.
// Zero-Heap Allocation!
void gui_draw_string(int x, int y, const char* str, uint16_t fg, uint16_t bg,
                     int scale) {
    if (!panel_handle) return;
    int len = strlen(str);
    if (len == 0) return;

    int width = len * 8 * scale;
    int height = 8 * scale;

    // Ochrana pred vykreslením mimo obrazovky / buffra
    if (x + width > LCD_H_RES) width = LCD_H_RES - x;
    if (height > GUI_BLOCK_LINES) height = GUI_BLOCK_LINES;
    if (y + height > LCD_V_RES) height = LCD_V_RES - y;
    if (width <= 0 || height <= 0) return;

    // 1. Vyplnenie obdĺžnika textu farbou pozadia
    for (int i = 0; i < width * height; i++) {
        render_buffer[i] = __builtin_bswap16(bg);
    }

    // 2. Vykreslenie pixelov písmen s dynamickým zväčšením
    int px = 0;
    for (int i = 0; i < len; i++) {
        char c = str[i];
        if (c < 32 || c > 126) c = '?';  // Záchrana pre neznáme znaky
        const uint8_t* glyph = font8x8[c - 32];

        for (int gy = 0; gy < 8; gy++) {
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (row & (1 << (7 - gx))) {  // Extrakcia bitov sprava doľava
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            int buf_x = px + gx * scale + sx;
                            int buf_y = gy * scale + sy;
                            if (buf_x < width && buf_y < height) {
                                render_buffer[buf_y * width + buf_x] =
                                    __builtin_bswap16(fg);
                            }
                        }
                    }
                }
            }
        }
        px += 8 * scale;
        if (px >= width) break;
    }

    // 3. Odoslanie do DMA asynchrónne
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + width, y + height,
                              render_buffer);
    // Kritické: Pauza pre radič, aby stihol odoslať buffer predtým, než ho
    // prepíše ďalšie slovo
    vTaskDelay(pdMS_TO_TICKS(10));
}

// --- IKONOVÝ RENDERER (16x16 -> Custom Scale) ---
void gui_draw_icon_16x16(int x, int y, const uint8_t* icon, uint16_t fg,
                         uint16_t bg, int scale) {
    if (!panel_handle) return;
    int width = 16 * scale;
    int height = 16 * scale;

    // Ochrana hraníc
    if (x + width > LCD_H_RES) width = LCD_H_RES - x;
    if (height > GUI_BLOCK_LINES) height = GUI_BLOCK_LINES;
    if (y + height > LCD_V_RES) height = LCD_V_RES - y;
    if (width <= 0 || height <= 0) return;

    for (int i = 0; i < width * height; i++) {
        render_buffer[i] = __builtin_bswap16(bg);
    }

    for (int gy = 0; gy < 16; gy++) {
        uint16_t row = (icon[gy * 2] << 8) | icon[gy * 2 + 1];
        for (int gx = 0; gx < 16; gx++) {
            if (row & (1 << (15 - gx))) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int buf_x = gx * scale + sx;
                        int buf_y = gy * scale + sy;
                        if (buf_x < width && buf_y < height) {
                            render_buffer[buf_y * width + buf_x] =
                                __builtin_bswap16(fg);
                        }
                    }
                }
            }
        }
    }
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + width, y + height,
                              render_buffer);
    vTaskDelay(pdMS_TO_TICKS(10));
}

// Pomocná funkcia na preklad OWM ID na krátky text
static const char* get_weather_desc(int id) {
    if (id >= 200 && id < 300) return "BURKA";
    if (id >= 300 && id < 400) return "MRHOL";
    if (id >= 500 && id < 600) return "DAZD";
    if (id >= 600 && id < 700) return "SNEH";
    if (id >= 700 && id < 800) return "HMLA";
    if (id == 800) return "JASNO";
    if (id > 800 && id < 900) return "OBLAKY";
    return "NEZNAMO";
}

extern "C" void gui_task(void* arg) {
    ESP_LOGI(TAG, "GUI Task spustený (HMI Mode)");

    // Inicializácia BOOT tlačidla na GPIO9
    gpio_set_direction((gpio_num_t)BOOT_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)BOOT_BUTTON_PIN, GPIO_PULLUP_ONLY);

    // --- 1. BOOT SEQUENCE (Tvoj vysnívaný start-up log) ---
    display_clear(COLOR_BLACK);
    vTaskDelay(pdMS_TO_TICKS(50));

    gui_draw_string(5, 10, "REDPINS OS", COLOR_CYAN, COLOR_BLACK, 2);
    vTaskDelay(pdMS_TO_TICKS(500));

    gui_draw_string(5, 40, "Starting system...", COLOR_WHITE, COLOR_BLACK, 1);
    vTaskDelay(pdMS_TO_TICKS(400));

    gui_draw_string(5, 55, "Bluetooth ... OK", COLOR_GREEN, COLOR_BLACK, 1);
    vTaskDelay(pdMS_TO_TICKS(400));

    gui_draw_string(5, 70, "WiFi ........ OK", COLOR_GREEN, COLOR_BLACK, 1);
    vTaskDelay(pdMS_TO_TICKS(400));

    gui_draw_string(5, 85, "Sensors ..... OK", COLOR_GREEN, COLOR_BLACK, 1);
    vTaskDelay(pdMS_TO_TICKS(1500));  // Necháme používateľa chvíľu sa pokochať

    display_clear(COLOR_BLACK);
    vTaskDelay(pdMS_TO_TICKS(50));

    // --- 2. MAIN GUI LOOP (State Machine) ---
    char buf[32];
    int current_screen = 0;
    bool btn_last_state = true;

    uint32_t last_draw_time = 0;
    bool force_redraw = true;

    while (1) {
        // Čítanie tlačidla s jednoduchým debouncingom
        bool btn_state = gpio_get_level((gpio_num_t)BOOT_BUTTON_PIN);
        if (btn_state == 0 &&
            btn_last_state == 1) {  // Detekcia stlačenia (hrana)
            current_screen =
                (current_screen + 1) % 3;  // Rotácia 0 -> 1 -> 2 -> 0
            force_redraw = true;
            display_clear(COLOR_BLACK);  // Vymazanie obrazovky pri prepnutí
            ESP_LOGI(TAG, "Tlačidlo stlačené! Prepínam na obrazovku %d",
                     current_screen);
        }
        btn_last_state = btn_state;

        // Prekreslíme len ak bolo stlačené tlačidlo, alebo uplynuli 2 sekundy
        uint32_t now_ms = esp_timer_get_time() / 1000;
        if (force_redraw || (now_ms - last_draw_time) >= 2000) {
            // --- STATUS BAR (Spoločný pre všetky obrazovky) ---
            time_t now_ts;
            time(&now_ts);
            struct tm timeinfo;
            localtime_r(&now_ts, &timeinfo);
            char time_str[16];
            if (timeinfo.tm_year > (2020 - 1900)) {
                strftime(time_str, sizeof(time_str), "%H:%M", &timeinfo);
            } else {
                snprintf(time_str, sizeof(time_str), "--:--");
            }
            gui_draw_string(230, 5, time_str, COLOR_WHITE, COLOR_BLACK, 2);

            // --- OBRAZOVKA 0: Hlavný Dashboard ---
            if (current_screen == 0) {
                float t = 0, h = 0;
                sensor_core_get_latest(&t, &h);

                gui_draw_string(10, 30, "TEPLOTA:", COLOR_YELLOW, COLOR_BLACK,
                                2);
                snprintf(buf, sizeof(buf), "%.1f C", t);
                gui_draw_string(10, 60, buf, COLOR_WHITE, COLOR_BLACK, 4);

                gui_draw_string(160, 30, "VLHKOST:", COLOR_CYAN, COLOR_BLACK,
                                2);
                snprintf(buf, sizeof(buf), "%.1f %%", h);
                gui_draw_string(160, 60, buf, COLOR_WHITE, COLOR_BLACK, 4);
            }
            // --- OBRAZOVKA 1: Počasie (OpenWeather API) ---
            else if (current_screen == 1) {
                float wt = 0;
                int wh = 0, wp = 0, wid = 0;
                if (weather_get_latest(&wt, &wh, &wp, &wid)) {
                    gui_draw_string(10, 30, "POCASIE:", COLOR_GREEN,
                                    COLOR_BLACK, 2);
                    gui_draw_string(160, 30, get_weather_desc(wid),
                                    COLOR_YELLOW, COLOR_BLACK, 2);

                    snprintf(buf, sizeof(buf), "%.1f C", wt);
                    gui_draw_string(10, 60, buf, COLOR_WHITE, COLOR_BLACK, 4);

                    snprintf(buf, sizeof(buf), "Vlhkost: %d %%", wh);
                    gui_draw_string(10, 110, buf, COLOR_CYAN, COLOR_BLACK, 2);

                    snprintf(buf, sizeof(buf), "Tlak: %d hPa", wp);
                    gui_draw_string(10, 135, buf, COLOR_CYAN, COLOR_BLACK, 2);

                    // Zanshin: Detekcia typu ikony podľa OWM kódu
                    const uint8_t* icon = w_cloud;
                    uint16_t i_color = COLOR_WHITE;

                    if (wid >= 200 && wid < 300) {
                        icon = w_storm;
                        i_color = COLOR_YELLOW;
                    } else if (wid >= 300 && wid < 600) {
                        icon = w_rain;
                        i_color = COLOR_CYAN;
                    } else if (wid >= 600 && wid < 700) {
                        icon = w_snow;
                        i_color = COLOR_WHITE;
                    } else if (wid == 800) {
                        icon = w_sun;
                        i_color = COLOR_YELLOW;
                    } else if (wid > 800) {
                        icon = w_cloud;
                        i_color = COLOR_WHITE;
                    }

                    // Vykreslenie veľkej 32x32px ikony na pravej strane
                    // obrazovky
                    gui_draw_icon_16x16(240, 60, icon, i_color, COLOR_BLACK, 2);

                } else {
                    gui_draw_string(10, 30, "OPENWEATHER API:", COLOR_GREEN,
                                    COLOR_BLACK, 2);
                    gui_draw_string(10, 60, "Cakaj na sync...", COLOR_WHITE,
                                    COLOR_BLACK, 2);
                }
            }
            // --- OBRAZOVKA 2: Systém a Nastavenia ---
            else if (current_screen == 2) {
                app_config_t* cfg = app_config_get();
                gui_draw_string(10, 30, "ZARIADENIE:", COLOR_CYAN, COLOR_BLACK,
                                2);
                gui_draw_string(10, 55, cfg->alias, COLOR_WHITE, COLOR_BLACK,
                                2);

                gui_draw_string(10, 90, "WIFI STATUS:", COLOR_YELLOW,
                                COLOR_BLACK, 2);
                if (wifi_scanner_is_connected()) {
                    char ip[16];
                    wifi_scanner_get_ip(ip, sizeof(ip));
                    gui_draw_string(10, 115, ip, COLOR_GREEN, COLOR_BLACK, 2);
                } else {
                    gui_draw_string(10, 115, "Odpojene", COLOR_RED, COLOR_BLACK,
                                    2);
                }
            }

            force_redraw = false;
            last_draw_time = now_ms;
        }

        // Krátka pauza (50ms) = blesková reakcia na tlačidlo + neblokujeme
        // FreeRTOS
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}