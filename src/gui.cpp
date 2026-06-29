#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "ble_server.h"
#include "display_hal.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "font8x8.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gui_colors.h"
#include "sensor_core.h"
#include "storage.h"
#include "weather_client.h"
#include "weather_icons.h"
#include "wifi_scanner.h"

static const char* TAG = "GUI";

#define BOOT_BUTTON_PIN 9
#define BTN_OK_PIN 2
#define BTN_ESC_PIN 3
#define BTN_UP_PIN 4
#define BTN_DOWN_PIN 5
#define GUI_BUTTON_DEBUG 0

#if GUI_BUTTON_DEBUG
#define BTN_LOGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#else
#define BTN_LOGI(...)                   \
    do {                                \
        if (0) {                        \
            ESP_LOGI(TAG, __VA_ARGS__); \
        }                               \
    } while (0)
#endif
extern esp_lcd_panel_handle_t panel_handle;
extern "C" void display_clear(uint16_t color);

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

// --- ŠPECIÁLNY RENDERER PRE WI-FI (Dynamické vrstvy) ---
void gui_draw_wifi_icon(int x, int y, int level, uint16_t active_fg,
                        uint16_t inactive_fg, uint16_t bg) {
    if (!panel_handle) return;
    int width = 16;
    int height = 16;

    if (x + width > LCD_H_RES) width = LCD_H_RES - x;
    if (height > GUI_BLOCK_LINES) height = GUI_BLOCK_LINES;
    if (y + height > LCD_V_RES) height = LCD_V_RES - y;
    if (width <= 0 || height <= 0) return;

    for (int i = 0; i < width * height; i++) {
        render_buffer[i] = __builtin_bswap16(bg);
    }

    for (int gy = 0; gy < 16; gy++) {
        uint16_t row = (i_wifi[gy * 2] << 8) | i_wifi[gy * 2 + 1];

        // Zanshin: Rozdelenie ikony podľa Y osi na Bodku, Stredný a Horný oblúk
        uint16_t fg = inactive_fg;
        if (level >= 1 && gy >= 11)
            fg = active_fg;  // Spodná bodka
        else if (level >= 2 && gy >= 6 && gy <= 10)
            fg = active_fg;  // Stredný oblúk
        else if (level >= 3 && gy <= 5)
            fg = active_fg;  // Horný oblúk

        for (int gx = 0; gx < 16; gx++) {
            if (row & (1 << (15 - gx))) {
                if (gx < width && gy < height) {
                    render_buffer[gy * width + gx] = __builtin_bswap16(fg);
                }
            }
        }
    }
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + width, y + height,
                              render_buffer);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static uint16_t pixel_buffer[1] __attribute__((aligned(4)));
static uint16_t last_pixel_color = 0xFFFF;

// --- ZANSHIN FIX: Rýchla vertikálna čiara a blending farieb pre plochy grafov
// ---
static uint16_t vline_buffer[120] __attribute__((aligned(4)));
static uint16_t last_vline_color = 0xFFFF;

void gui_draw_vline_fast(int x, int y, int h, uint16_t color) {
    if (!panel_handle || h <= 0 || h > 120) return;

    if (color != last_vline_color) {
        vTaskDelay(pdMS_TO_TICKS(5));  // Počkáme na DMA pred prepísaním
        for (int i = 0; i < 120; i++) {
            vline_buffer[i] = __builtin_bswap16(color);
        }
        last_vline_color = color;
    }
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 1, y + h, vline_buffer);
}

static uint16_t blend_color(uint16_t fg, uint16_t bg, float alpha) {
    uint8_t fg_r = (fg >> 11) & 0x1F, fg_g = (fg >> 5) & 0x3F, fg_b = fg & 0x1F;
    uint8_t bg_r = (bg >> 11) & 0x1F, bg_g = (bg >> 5) & 0x3F, bg_b = bg & 0x1F;
    uint8_t r = bg_r + (uint8_t)((fg_r - bg_r) * alpha);
    uint8_t g = bg_g + (uint8_t)((fg_g - bg_g) * alpha);
    uint8_t b = bg_b + (uint8_t)((fg_b - bg_b) * alpha);
    return (r << 11) | (g << 5) | b;
}

// --- ZANSHIN FIX: Rýchle vykreslenie 2x2 bodu (Optimalizácia pre graf) ---
static uint16_t point_buffer[4] __attribute__((aligned(4)));
static uint16_t last_point_color = 0xFFFF;

void gui_draw_point_fast(int x, int y, uint16_t color) {
    if (!panel_handle) return;
    if (x < 0 || y < 0 || x + 2 > LCD_H_RES || y + 2 > LCD_V_RES) return;
    if (color != last_point_color) {
        vTaskDelay(pdMS_TO_TICKS(5));
        for (int i = 0; i < 4; i++) point_buffer[i] = __builtin_bswap16(color);
        last_point_color = color;
    }
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 2, y + 2, point_buffer);
}

// --- JEDNODUCHÉ KRESLENIE TVAROV (Na graf) ---
void gui_draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (!panel_handle) return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;

    // Zanshin: Optimalizácia pre kreslenie jednotlivých pixelov (napr. v
    // grafe) Vyhneme sa zbytočnému oneskoreniu a zložitosti pre 1x1 obdĺžnik.
    if (w == 1 && h == 1) {
        // ZANSHIN FIX: Samostatný buffer pre čiary. Zabraňuje DMA šumu!
        if (color != last_pixel_color) {
            vTaskDelay(pdMS_TO_TICKS(
                5));  // Počkáme na odoslanie starej farby z fronty
            pixel_buffer[0] = __builtin_bswap16(color);
            last_pixel_color = color;
        }

        esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 1, y + 1,
                                  pixel_buffer);
        return;
    }

    // Dynamicky rozdelíme kreslenie na bloky, ak je obdĺžnik príliš vysoký pre
    // DMA buffer
    int max_lines = GUI_BLOCK_LINES;
    int lines_to_draw = (h > max_lines) ? max_lines : h;

    for (int i = 0; i < w * lines_to_draw; i++) {
        render_buffer[i] = __builtin_bswap16(color);
    }

    int current_y = y;
    while (h > 0) {
        int draw_h = (h > max_lines) ? max_lines : h;
        esp_lcd_panel_draw_bitmap(panel_handle, x, current_y, x + w,
                                  current_y + draw_h, render_buffer);
        current_y += draw_h;
        h -= draw_h;
        vTaskDelay(pdMS_TO_TICKS(2));  // Uvoľní rádio a zabráni zaduseniu DMA
    }

    // ZANSHIN FIX: Povinný flush! Počkáme, kým DMA reálne odošle tento blok.
    // Ak to neurobíme, ďalšia funkcia prepíše render_buffer počas odosielania.
    vTaskDelay(pdMS_TO_TICKS(15));
}

// --- ZANSHIN: Ultra-ľahký zaoblený rámček (Polomer 4px) ---
void gui_draw_round_rect_empty(int x, int y, int w, int h, int r,
                               uint16_t color) {
    if (!panel_handle || w <= 0 || h <= 0) return;

    // Horná a spodná hrana
    gui_draw_rect(x + r, y, w - 2 * r, 1, color);
    gui_draw_rect(x + r, y + h - 1, w - 2 * r, 1, color);

    // Ľavá a pravá hrana
    gui_draw_rect(x, y + r, 1, h - 2 * r, color);
    gui_draw_rect(x + w - 1, y + r, 1, h - 2 * r, color);

    if (r >= 4) {
        // Zrezanie rohov pre vizuálny efekt zaoblenia
        gui_draw_rect(x + 2, y + 1, 2, 1, color);
        gui_draw_rect(x + 1, y + 2, 1, 2, color);  // TL
        gui_draw_rect(x + w - 4, y + 1, 2, 1, color);
        gui_draw_rect(x + w - 2, y + 2, 1, 2, color);  // TR
        gui_draw_rect(x + 2, y + h - 2, 2, 1, color);
        gui_draw_rect(x + 1, y + h - 4, 1, 2, color);  // BL
        gui_draw_rect(x + w - 4, y + h - 2, 2, 1, color);
        gui_draw_rect(x + w - 2, y + h - 4, 1, 2, color);  // BR
    }
}

// Kreslenie čiary pomocou Bresenhamovho algoritmu (pixel po pixeli)
void gui_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int e2;

    while (1) {
        gui_draw_rect(x0, y0, 1, 1, color);  // Vďaka optimalizácii je to rýchle
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
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

// Pomocná funkcia pre výpočet absolútnej vlhkosti (Magnus-Tetens rovnica v
// g/m3) Zanshin: Rýchla matematika, žiadne zbytočné pretečenia a volania von
// externých knižníc
static float get_absolute_humidity(float t, float h) {
    float e = 6.112f * exp((17.67f * t) / (t + 243.5f));
    float pv = e * (h / 100.0f);
    return (2.16679f * pv * 100.0f) / (273.15f + t);
}

static uint8_t map_lux_to_backlight_percent(float lux) {
    // Tuned for indoor comfort: softer low-light response, no harsh jumps.
    if (lux < 2.0f) return 14;
    if (lux < 6.0f) return 17;
    if (lux < 15.0f) return 21;
    if (lux < 35.0f) return 26;
    if (lux < 80.0f) return 32;
    if (lux < 180.0f) return 39;
    if (lux < 400.0f) return 47;
    if (lux < 900.0f) return 56;
    if (lux < 2000.0f) return 66;
    if (lux < 5000.0f) return 77;
    return 88;
}

// --- IKONY PRE SENZORY (16x16, 1bpp) ---
static const uint8_t i_thermometer[32] = {
    0x03, 0xC0, 0x04, 0x20, 0x04, 0x20, 0x05, 0xA0, 0x05, 0xA0, 0x05,
    0xA0, 0x05, 0xA0, 0x05, 0xA0, 0x05, 0xA0, 0x09, 0x90, 0x13, 0xC8,
    0x13, 0xC8, 0x13, 0xC8, 0x09, 0x90, 0x07, 0xE0, 0x00, 0x00};

static const uint8_t i_drop[32] = {
    0x01, 0x80, 0x03, 0xC0, 0x07, 0xE0, 0x0F, 0xF0, 0x1F, 0xF8, 0x3F,
    0xFC, 0x3F, 0xFC, 0x7F, 0xFE, 0x7F, 0xFE, 0x7F, 0xFE, 0x3F, 0xFC,
    0x3F, 0xFC, 0x1F, 0xF8, 0x0F, 0xF0, 0x07, 0xE0, 0x03, 0xC0};

// Zaokrúhli na 1 desatinné miesto - presne na to, čo sa reálne zobrazuje.
// Cache musí porovnávať túto hodnotu, nie plnú float presnosť, inak
// EMA filter v sensor_core nikdy "nedobehne" na bitovo identickú hodnotu
// a displej by sa prekresľoval aj keď je zobrazený text rovnaký.
static inline float round1(float v) { return roundf(v * 10.0f) / 10.0f; }

// Pomocná funkcia na bezpečné formátovanie hodnoty senzora
static void format_sensor_val(float val, char* int_buf, char* dec_buf) {
    char str[16];
    snprintf(str, sizeof(str), "%.1f", val);
    char* dot = strchr(str, '.');
    if (dot) {
        int len = dot - str;
        strncpy(int_buf, str, len);
        int_buf[len] = '\0';
        strncpy(dec_buf, dot, 2);
        dec_buf[2] = '\0';
        if (strcmp(int_buf, "-0") == 0)
            strcpy(int_buf, "-0");  // Zachová znak mínus
    } else {
        strcpy(int_buf, str);
        strcpy(dec_buf, ".0");
    }
}

extern "C" void gui_task(void* arg) {
    ESP_LOGI(TAG, "GUI Task spustený (HMI Mode)");

    // Inicializácia BOOT tlačidla na GPIO9
    gpio_set_direction((gpio_num_t)BOOT_BUTTON_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)BOOT_BUTTON_PIN, GPIO_PULLUP_ONLY);

    // Externe tlacidla (aktivne v log. 0): OK, ESC, UP, DOWN
    gpio_set_direction((gpio_num_t)BTN_OK_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)BTN_OK_PIN, GPIO_PULLUP_ONLY);
    gpio_set_direction((gpio_num_t)BTN_ESC_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)BTN_ESC_PIN, GPIO_PULLUP_ONLY);
    gpio_set_direction((gpio_num_t)BTN_UP_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)BTN_UP_PIN, GPIO_PULLUP_ONLY);
    gpio_set_direction((gpio_num_t)BTN_DOWN_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)BTN_DOWN_PIN, GPIO_PULLUP_ONLY);

    BTN_LOGI("BTN RAW startup: BOOT=%d OK=%d ESC=%d UP=%d DOWN=%d",
             gpio_get_level((gpio_num_t)BOOT_BUTTON_PIN),
             gpio_get_level((gpio_num_t)BTN_OK_PIN),
             gpio_get_level((gpio_num_t)BTN_ESC_PIN),
             gpio_get_level((gpio_num_t)BTN_UP_PIN),
             gpio_get_level((gpio_num_t)BTN_DOWN_PIN));

    // Zanshin: Načítame a aplikujeme uloženú rotáciu EŠTE PRED bootovacou
    // obrazovkou. Predvolená "rovná" orientácia displeja vyžaduje (true, true).
    bool s_display_rotated = app_config_get()->display_rotated;
    esp_lcd_panel_mirror(panel_handle, !s_display_rotated, !s_display_rotated);

    // --- 1. BOOT SEQUENCE (Tvoj vysnívaný start-up log) ---
    display_clear(THEME_BG);
    vTaskDelay(pdMS_TO_TICKS(50));

    gui_draw_string(5, 10, "REDPINS OS", BOOT_TITLE_FG, THEME_BG, 2);
    vTaskDelay(pdMS_TO_TICKS(500));

    gui_draw_string(5, 40, "Starting system...", BOOT_TEXT_FG, THEME_BG, 1);
    vTaskDelay(pdMS_TO_TICKS(400));

    gui_draw_string(5, 55, "Bluetooth ... OK", BOOT_OK_FG, THEME_BG, 1);
    vTaskDelay(pdMS_TO_TICKS(400));

    gui_draw_string(5, 70, "WiFi ........ OK", BOOT_OK_FG, THEME_BG, 1);
    vTaskDelay(pdMS_TO_TICKS(400));

    bool weather_sensor_ok = sensor_core_weather_sensor_ok();
    gui_draw_string(
        5, 85,
        weather_sensor_ok ? "Weather senzor .. OK" : "Weather senzor .. FAIL",
        weather_sensor_ok ? BOOT_OK_FG : SYS_WIFI_ERR_FG, THEME_BG, 1);
    vTaskDelay(pdMS_TO_TICKS(1500));  // Necháme používateľa chvíľu sa pokochať

    display_clear(THEME_BG);
    vTaskDelay(pdMS_TO_TICKS(50));

    // --- 2. MAIN GUI LOOP (State Machine) ---
    int current_screen = 0;
    bool btn_last_state = true;
    uint32_t btn_press_time = 0;
    bool btn_long_pressed = false;
    bool ok_last_state = true;
    bool esc_last_state = true;
    bool up_last_state = true;
    bool down_last_state = true;
    uint32_t ok_press_time = 0;
    uint32_t esc_press_time = 0;
    uint32_t up_press_time = 0;
    uint32_t down_press_time = 0;
    uint32_t ok_last_event_ms = 0;
    uint32_t esc_last_event_ms = 0;
    uint32_t up_last_event_ms = 0;
    uint32_t down_last_event_ms = 0;

    bool is_in_options = false;
    int selected_option_idx = 0;
    int graph_range_days = 1;            // Sledovanie zvoleného rozsahu v dňoch
    int graph_flip_interval_ms = 30000;  // Interval preklápania grafov (IN/OUT)

    uint32_t last_draw_time = 0;
    bool force_redraw = true;

    uint32_t last_graph_redraw_time = 0;
    int graph_page = 0;  // 0 = IN (Senzor), 1 = OUT (OWM)
    int last_dot_x =
        -1;  // Sledovanie starej pozície bežiaceho bodu (Optimalizácia)
    uint32_t last_anim_sec = 999;  // Pre sledovanie zmeny sekúnd

    // --- CACHE PRE ZABRÁNENIE BLIKANIU ---
    float cache_t = -999, cache_h = -999;
    float cache_v_t = -999, cache_v_h = -999, cache_wt = -999;
    int cache_wh = -999, cache_wp = -999, cache_wid = -999;
    int cache_aqi = -999;
    float cache_pm25 = -999;
    int cache_local_p = -999;
    int cache_weather_sensor_ok = -1;
    uint32_t last_brightness_update_ms = 0;
    float filtered_lux = -1.0f;
    uint32_t last_btn_diag_ms = 0;
    int last_raw_boot = -1;
    int last_raw_ok = -1;
    int last_raw_esc = -1;
    int last_raw_up = -1;
    int last_raw_down = -1;

    auto get_option_count = [&](int screen) {
        if (screen == 4) return 6;
        if (screen == 5) return 3;
        return 1;
    };

    auto close_options_menu = [&]() {
        is_in_options = false;
        force_redraw = true;
        display_clear(THEME_BG);
    };

    auto open_options_menu = [&]() {
        if (current_screen == 4 || current_screen == 5) {
            is_in_options = true;
            selected_option_idx = 0;
            force_redraw = true;
            display_clear(THEME_BG);
        }
    };

    auto apply_selected_option = [&]() {
        if (current_screen == 4) {
            if (selected_option_idx == 0)
                graph_range_days = 1;
            else if (selected_option_idx == 1)
                graph_range_days = 3;
            else if (selected_option_idx == 2)
                graph_range_days = 7;
            else if (selected_option_idx == 3)
                graph_flip_interval_ms = 15000;
            else if (selected_option_idx == 4)
                graph_flip_interval_ms = 30000;
            // idx 5 je Back (nic nerobime)
        } else if (current_screen == 5) {
            if (selected_option_idx == 0) {
                s_display_rotated = !s_display_rotated;
                esp_lcd_panel_mirror(panel_handle, !s_display_rotated,
                                     !s_display_rotated);
                app_config_get()->display_rotated = s_display_rotated;
                app_config_save();
            } else if (selected_option_idx == 1) {
                app_config_get()->auto_brightness =
                    !app_config_get()->auto_brightness;
                app_config_save();
            }
            // idx 2 je Back (nic nerobime)
        }
    };

    auto next_screen = [&]() {
        current_screen = (current_screen + 1) % 6;
        force_redraw = true;
        display_clear(THEME_BG);
    };

    auto prev_screen = [&]() {
        current_screen = (current_screen + 5) % 6;
        force_redraw = true;
        display_clear(THEME_BG);
    };

    auto next_option = [&]() {
        int opt_count = get_option_count(current_screen);
        selected_option_idx = (selected_option_idx + 1) % opt_count;
        force_redraw = true;
    };

    auto prev_option = [&]() {
        int opt_count = get_option_count(current_screen);
        selected_option_idx = (selected_option_idx + opt_count - 1) % opt_count;
        force_redraw = true;
    };

    while (1) {
        uint32_t now_ms = esp_timer_get_time() / 1000;

        int raw_boot = gpio_get_level((gpio_num_t)BOOT_BUTTON_PIN);
        int raw_ok = gpio_get_level((gpio_num_t)BTN_OK_PIN);
        int raw_esc = gpio_get_level((gpio_num_t)BTN_ESC_PIN);
        int raw_up = gpio_get_level((gpio_num_t)BTN_UP_PIN);
        int raw_down = gpio_get_level((gpio_num_t)BTN_DOWN_PIN);

        if (raw_boot != last_raw_boot || raw_ok != last_raw_ok ||
            raw_esc != last_raw_esc || raw_up != last_raw_up ||
            raw_down != last_raw_down) {
            BTN_LOGI("BTN RAW CHG: BOOT=%d OK=%d ESC=%d UP=%d DOWN=%d",
                     raw_boot, raw_ok, raw_esc, raw_up, raw_down);
            last_raw_boot = raw_boot;
            last_raw_ok = raw_ok;
            last_raw_esc = raw_esc;
            last_raw_up = raw_up;
            last_raw_down = raw_down;
        }

        if ((now_ms - last_btn_diag_ms) >= 2000) {
            BTN_LOGI("BTN RAW: BOOT=%d OK=%d ESC=%d UP=%d DOWN=%d",
                     gpio_get_level((gpio_num_t)BOOT_BUTTON_PIN),
                     gpio_get_level((gpio_num_t)BTN_OK_PIN),
                     gpio_get_level((gpio_num_t)BTN_ESC_PIN),
                     gpio_get_level((gpio_num_t)BTN_UP_PIN),
                     gpio_get_level((gpio_num_t)BTN_DOWN_PIN));
            last_btn_diag_ms = now_ms;
        }

        // Čítanie tlačidla s detekciou krátkeho a dlhého stlačenia
        bool btn_state = (bool)raw_boot;
        if (btn_state == 0 &&
            btn_last_state == 1) {  // Detekcia stlačenia (hrana nadol)
            btn_press_time = now_ms;
            btn_long_pressed = false;
            BTN_LOGI("BTN BOOT DOWN");
        }

        // Dlhé stlačenie (800ms)
        if (btn_state == 0 && !btn_long_pressed &&
            (now_ms - btn_press_time > 800)) {
            btn_long_pressed = true;
            BTN_LOGI("BTN BOOT LONG (%ums)",
                     (unsigned)(now_ms - btn_press_time));
            if (is_in_options) {
                apply_selected_option();
                close_options_menu();
            } else {
                // Zanshin: Zamedzenie uviaznutia. Menu otvorime len tam, kde
                // realne su nastavenia (Grafy a System)
                open_options_menu();
            }
        }

        // Krátke stlačenie (uvoľnenie pred limitom)
        if (btn_state == 1 && btn_last_state == 0) {
            if (!btn_long_pressed &&
                (now_ms - btn_press_time > 50)) {  // 50ms debounce
                BTN_LOGI("BTN BOOT CLICK (%ums)",
                         (unsigned)(now_ms - btn_press_time));
                if (is_in_options) {
                    // Cyklovanie moznosti v menu
                    next_option();
                } else {
                    // Rotacia obrazoviek
                    next_screen();
                }
            }
        }
        btn_last_state = btn_state;

        // Externe OK tlacidlo: mimo menu otvori options, v menu potvrdi.
        bool ok_state = (bool)raw_ok;
        if (ok_state == 0 && ok_last_state == 1) {
            ok_press_time = now_ms;
            BTN_LOGI("BTN OK DOWN");
            if ((now_ms - ok_last_event_ms) > 80) {
                if (is_in_options) {
                    BTN_LOGI("BTN OK ACTION: confirm option");
                    apply_selected_option();
                    close_options_menu();
                } else {
                    BTN_LOGI("BTN OK ACTION: open options");
                    open_options_menu();
                }
                ok_last_event_ms = now_ms;
            }
        }
        if (ok_state == 1 && ok_last_state == 0) {
            BTN_LOGI("BTN OK UP");
        }
        ok_last_state = ok_state;

        // Externe ESC tlacidlo: v menu zrusi options bez potvrdenia.
        bool esc_state = (bool)raw_esc;
        if (esc_state == 0 && esc_last_state == 1) {
            esc_press_time = now_ms;
            BTN_LOGI("BTN ESC DOWN");
            if ((now_ms - esc_last_event_ms) > 80) {
                if (is_in_options) {
                    BTN_LOGI("BTN ESC ACTION: cancel options");
                    close_options_menu();
                }
                esc_last_event_ms = now_ms;
            }
        }
        if (esc_state == 1 && esc_last_state == 0) {
            BTN_LOGI("BTN ESC UP");
        }
        esc_last_state = esc_state;

        // Externe UP tlacidlo: obrazovky dopredu, v options pohyb hore.
        bool up_state = (bool)raw_up;
        if (up_state == 0 && up_last_state == 1) {
            up_press_time = now_ms;
            BTN_LOGI("BTN UP DOWN");
            if ((now_ms - up_last_event_ms) > 80) {
                if (is_in_options) {
                    BTN_LOGI("BTN UP ACTION: prev option");
                    prev_option();
                } else {
                    BTN_LOGI("BTN UP ACTION: prev screen");
                    prev_screen();
                }
                up_last_event_ms = now_ms;
            }
        }
        if (up_state == 1 && up_last_state == 0) {
            BTN_LOGI("BTN UP UP");
        }
        up_last_state = up_state;

        // Externe DOWN tlacidlo: obrazovky dozadu, v options pohyb dole.
        bool down_state = (bool)raw_down;
        if (down_state == 0 && down_last_state == 1) {
            down_press_time = now_ms;
            BTN_LOGI("BTN DOWN DOWN");
            if ((now_ms - down_last_event_ms) > 80) {
                if (is_in_options) {
                    BTN_LOGI("BTN DOWN ACTION: next option");
                    next_option();
                } else {
                    BTN_LOGI("BTN DOWN ACTION: next screen");
                    next_screen();
                }
                down_last_event_ms = now_ms;
            }
        }
        if (down_state == 1 && down_last_state == 0) {
            BTN_LOGI("BTN DOWN UP");
        }
        down_last_state = down_state;

        // Settings potrebuje sviznejsi refresh pre live lux hodnotu.
        uint32_t redraw_period_ms = 2000;
        if (!is_in_options && current_screen == 5) {
            redraw_period_ms = 1000;
        }

        // Prekreslíme len ak bolo stlačené tlačidlo, alebo uplynul refresh
        // interval.
        if (force_redraw || (now_ms - last_draw_time) >= redraw_period_ms) {
            if (force_redraw) {
                cache_t = -999;
                cache_h = -999;
                cache_v_t = -999;
                cache_v_h = -999;
                cache_wt = -999;
                cache_wh = -999;
                cache_wp = -999;
                cache_wid = -999;
                cache_aqi = -999;
                cache_pm25 = -999;
            }

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

            // Názov aktuálnej obrazovky (Zarovnaný doľava s paddingom na 12
            // znakov pre automatické vymazanie)
            const char* screen_titles[] = {"COMPARE",     "SENSORS",
                                           "WEATHER",     "ATMOSPHERE",
                                           "TEMPERATURE", "SYSTEM"};
            char title_buf[16];
            if (is_in_options) {
                snprintf(title_buf, sizeof(title_buf), "%-12s", "OPTIONS");
            } else {
                snprintf(title_buf, sizeof(title_buf), "%-12s",
                         screen_titles[current_screen]);
            }
            gui_draw_string(5, 5, title_buf, STATUS_FG, STATUS_BG, 2);

            // --- IKONY STAVU (BLE a WiFi) ---
            bool ble_conn = ble_server_is_connected();
            gui_draw_icon_16x16(195, 5, i_ble,
                                ble_conn ? STATUS_BLE_FG : STATUS_OFFLINE_FG,
                                STATUS_BG, 1);

            int rssi = wifi_scanner_get_rssi();
            int wifi_level = 0;
            if (wifi_scanner_is_connected()) {
                if (rssi > -60)
                    wifi_level = 3;
                else if (rssi > -75)
                    wifi_level = 2;
                else
                    wifi_level = 1;
            }

            // Vykreslíme WiFi ako klasické Smartfónové vlnky
            gui_draw_wifi_icon(216, 5, wifi_level, STATUS_WIFI_FG,
                               STATUS_OFFLINE_FG, STATUS_BG);

            gui_draw_string(240, 5, time_str, STATUS_FG, STATUS_BG,
                            2);  // Hodiny na doraz vpravo

            if (is_in_options) {
                // Vykreslenie Options kontextového menu
                const char* opt_graph[] = {"1 Den",       "3 Dni",
                                           "7 Dni",       "Rotacia 15s",
                                           "Rotacia 30s", "Back"};
                char opt_auto_bri[24];
                snprintf(opt_auto_bri, sizeof(opt_auto_bri), "Auto jas: %s",
                         app_config_get()->auto_brightness ? "ON" : "OFF");
                const char* opt_sys[] = {"Otocit o 180", opt_auto_bri, "Back"};
                const char* opt_default[] = {"Back"};

                const char** options = opt_default;
                int count = 1;

                if (current_screen == 4) {
                    options = opt_graph;
                    count = 6;
                } else if (current_screen == 5) {
                    options = opt_sys;
                    count = 3;
                }

                for (int i = 0; i < count; i++) {
                    uint16_t fg =
                        (i == selected_option_idx) ? THEME_BG : STATUS_FG;
                    uint16_t bg =
                        (i == selected_option_idx) ? STATUS_FG : THEME_BG;

                    // Podfarbenie aktívneho riadku pre lepšiu viditeľnosť
                    if (i == selected_option_idx) {
                        gui_draw_rect(10, 30 + i * 24, 200, 22, bg);
                    } else {
                        gui_draw_rect(10, 30 + i * 24, 200, 22, THEME_BG);
                    }
                    gui_draw_string(15, 33 + i * 24, options[i], fg, bg, 2);
                }
            } else {
                // --- OBRAZOVKA 0: COMPARE (Senzor vs API, 2x2 grid) ---
                if (current_screen == 0) {
                    float t = 0, h = 0;
                    sensor_core_get_latest(&t, &h);

                    float wt = 0;
                    int wh = 0, wp = 0, wid = 0;
                    bool has_api = weather_get_latest(&wt, &wh, &wp, &wid);

                    char i_buf[8] = {0}, d_buf[8] = {0};
                    int box_y = 30, box_w = 145, box_h = 82;
                    int bx_t = 10;
                    int bx_h = 165;
                    int box_y2 = 117, box_h2 = 50;

                    if (force_redraw) {
                        gui_draw_round_rect_empty(bx_t, box_y, box_w, box_h, 4,
                                                  DASH_TEMP_BORDER);
                        gui_draw_string(bx_t + 10, box_y + 5,
                                        "TEPLOTA SENZOR", DASH_TEMP_LBL,
                                        THEME_BG, 1);
                        gui_draw_icon_16x16(bx_t + box_w - 25, box_y + 3,
                                            i_thermometer, DASH_TEMP_ICON,
                                            THEME_BG, 1);

                        gui_draw_round_rect_empty(bx_h, box_y, box_w, box_h, 4,
                                                  WEATHER_DESC_FG);
                        gui_draw_string(bx_h + 10, box_y + 5, "TEPLOTA API",
                                        WEATHER_DESC_FG, THEME_BG, 1);

                        gui_draw_round_rect_empty(bx_t, box_y2, box_w, box_h2,
                                                  4, DASH_HUM_BORDER);
                        gui_draw_string(bx_t + 10, box_y2 + 5,
                                        "VLHKOST SENZOR", DASH_HUM_LBL,
                                        THEME_BG, 1);

                        gui_draw_round_rect_empty(bx_h, box_y2, box_w, box_h2,
                                                  4, WEATHER_HUM_FG);
                        gui_draw_string(bx_h + 10, box_y2 + 5, "VLHKOST API",
                                        WEATHER_HUM_FG, THEME_BG, 1);

                        if (!has_api) {
                            gui_draw_string(bx_h + 15, box_y + 45, "CAKAM...",
                                            GRAPH_MUTED_FG, THEME_BG, 1);
                            gui_draw_string(bx_h + 15, box_y2 + 24, "CAKAM...",
                                            GRAPH_MUTED_FG, THEME_BG, 1);
                        }
                    }

                    // Teplota - lokálny senzor (BME280)
                    float t_r = round1(t);
                    if (force_redraw || t_r != cache_t) {
                        format_sensor_val(t, i_buf, d_buf);
                        int len_t = strlen(i_buf);
                        int px_t = bx_t + 15;
                        gui_draw_rect(bx_t + 15, box_y + 35, 125, 32,
                                      THEME_BG);
                        if (len_t == 1) px_t += 10;
                        gui_draw_string(px_t, box_y + 35, i_buf, DASH_TEMP_VAL,
                                        THEME_BG, 4);
                        int dx_t = px_t + (len_t * 32);
                        gui_draw_string(dx_t, box_y + 51, d_buf, DASH_TEMP_DEC,
                                        THEME_BG, 2);
                        int ux_t = dx_t + 36;
                        gui_draw_rect(ux_t, box_y + 35, 4, 4, DASH_TEMP_UNIT);
                        gui_draw_rect(ux_t + 1, box_y + 36, 2, 2, THEME_BG);
                        gui_draw_string(ux_t + 6, box_y + 35, "C",
                                        DASH_TEMP_UNIT, THEME_BG, 2);
                        cache_t = t_r;
                    }

                    // Teplota - OpenWeatherMap API
                    if (has_api) {
                        float wt_r = round1(wt);
                        if (force_redraw || wt_r != cache_wt) {
                            format_sensor_val(wt, i_buf, d_buf);
                            int len_t = strlen(i_buf);
                            int px_t = bx_h + 15;
                            gui_draw_rect(bx_h + 15, box_y + 35, 125, 32,
                                          THEME_BG);
                            if (len_t == 1) px_t += 10;
                            gui_draw_string(px_t, box_y + 35, i_buf,
                                            WEATHER_TEMP_FG, THEME_BG, 4);
                            int dx_t = px_t + (len_t * 32);
                            gui_draw_string(dx_t, box_y + 51, d_buf,
                                            COLOR_LIGHT_GRAY, THEME_BG, 2);
                            int ux_t = dx_t + 36;
                            gui_draw_string(ux_t + 6, box_y + 35, "C",
                                            WEATHER_DESC_FG, THEME_BG, 2);
                            cache_wt = wt_r;
                        }
                    }

                    // Vlhkosť - lokálny senzor (BME280)
                    float h_r = round1(h);
                    if (force_redraw || h_r != cache_h) {
                        char hv_buf[12];
                        snprintf(hv_buf, sizeof(hv_buf), "%.1f %%", h);
                        gui_draw_rect(bx_t + 10, box_y2 + 22, 125, 20,
                                      THEME_BG);
                        gui_draw_string(bx_t + 10, box_y2 + 24, hv_buf,
                                        DASH_HUM_VAL, THEME_BG, 2);
                        cache_h = h_r;
                    }

                    // Vlhkosť - OpenWeatherMap API
                    if (has_api) {
                        if (force_redraw || wh != cache_wh) {
                            char hv_buf[12];
                            snprintf(hv_buf, sizeof(hv_buf), "%d %%", wh);
                            gui_draw_rect(bx_h + 10, box_y2 + 22, 125, 20,
                                          THEME_BG);
                            gui_draw_string(bx_h + 10, box_y2 + 24, hv_buf,
                                            WEATHER_HUM_FG, THEME_BG, 2);
                            cache_wh = wh;
                        }
                    }
                }
                // --- OBRAZOVKA 1: Hlavný Dashboard (Sensors) ---
                else if (current_screen == 1) {
                    float t = 0, h = 0;
                    sensor_core_get_latest(&t, &h);

                    char i_buf[8] = {0}, d_buf[8] = {0};
                    int box_y = 30, box_w = 145, box_h = 82;
                    int bx_t = 10;
                    int bx_h = 165;
                    int box_y2 = 117;
                    int box_w2 = 300;
                    int box_h2 = 50;
                    int bx2 = 10;

                    if (force_redraw) {
                        gui_draw_round_rect_empty(bx_t, box_y, box_w, box_h, 4,
                                                  DASH_TEMP_BORDER);
                        gui_draw_string(bx_t + 10, box_y + 5, "TEPLOTA",
                                        DASH_TEMP_LBL, THEME_BG, 1);
                        gui_draw_icon_16x16(bx_t + box_w - 25, box_y + 3,
                                            i_thermometer, DASH_TEMP_ICON,
                                            THEME_BG, 1);

                        gui_draw_round_rect_empty(bx_h, box_y, box_w, box_h, 4,
                                                  DASH_HUM_BORDER);
                        gui_draw_string(bx_h + 10, box_y + 5, "VLHKOST",
                                        DASH_HUM_LBL, THEME_BG, 1);
                        gui_draw_icon_16x16(bx_h + box_w - 25, box_y + 3,
                                            i_drop, DASH_HUM_ICON, THEME_BG, 1);

                        gui_draw_round_rect_empty(bx2, box_y2, box_w2, box_h2,
                                                  4, COLOR_LIGHT_GRAY);
                    }

                    float t_r = round1(t);
                    if (force_redraw || t_r != cache_t) {
                        format_sensor_val(t, i_buf, d_buf);
                        int len_t = strlen(i_buf);
                        int px_t = bx_t + 15;
                        gui_draw_rect(bx_t + 15, box_y + 35, 125, 32, THEME_BG);
                        if (len_t == 1) px_t += 10;
                        gui_draw_string(px_t, box_y + 35, i_buf, DASH_TEMP_VAL,
                                        THEME_BG, 4);
                        int dx_t = px_t + (len_t * 32);
                        gui_draw_string(dx_t, box_y + 51, d_buf, DASH_TEMP_DEC,
                                        THEME_BG, 2);
                        int ux_t = dx_t + 36;
                        gui_draw_rect(ux_t, box_y + 35, 4, 4, DASH_TEMP_UNIT);
                        gui_draw_rect(ux_t + 1, box_y + 36, 2, 2, THEME_BG);
                        gui_draw_string(ux_t + 6, box_y + 35, "C",
                                        DASH_TEMP_UNIT, THEME_BG, 2);
                        cache_t = t_r;
                    }

                    int weather_ok_now =
                        sensor_core_weather_sensor_ok() ? 1 : 0;
                    if (force_redraw ||
                        weather_ok_now != cache_weather_sensor_ok) {
                        gui_draw_rect(bx2 + 12, box_y2 + 3, 280, 9, THEME_BG);
                        gui_draw_string(
                            bx2 + 12, box_y2 + 3,
                            weather_ok_now ? "WEATHER SENZOR: OK"
                                           : "WEATHER SENZOR: FAIL",
                            weather_ok_now ? SYS_WIFI_OK_FG : SYS_WIFI_ERR_FG,
                            THEME_BG, 1);
                        cache_weather_sensor_ok = weather_ok_now;
                    }

                    float h_r = round1(h);
                    if (force_redraw || h_r != cache_h) {
                        format_sensor_val(h, i_buf, d_buf);
                        int len_h = strlen(i_buf);
                        int px_h = bx_h + 15;
                        gui_draw_rect(bx_h + 15, box_y + 35, 125, 32, THEME_BG);
                        if (len_h == 1) px_h += 10;
                        gui_draw_string(px_h, box_y + 35, i_buf, DASH_HUM_VAL,
                                        THEME_BG, 4);
                        int dx_h = px_h + (len_h * 32);
                        gui_draw_string(dx_h, box_y + 51, d_buf, DASH_HUM_DEC,
                                        THEME_BG, 2);
                        int ux_h = dx_h + 36;
                        gui_draw_string(ux_h, box_y + 35, "%", DASH_HUM_UNIT,
                                        THEME_BG, 2);
                        cache_h = h_r;
                    }

                    float wt = 0;
                    int wh = 0, wp = 0, wid = 0;

                    // Zobrazíme asistenta len ak už máme stiahnuté dáta počasia
                    if (weather_get_latest(&wt, &wh, &wp, &wid)) {
                        float wt_r = round1(wt);
                        if (force_redraw || t_r != cache_v_t ||
                            h_r != cache_v_h || wt_r != cache_wt ||
                            wh != cache_wh) {
                            float ah_in = get_absolute_humidity(t, h);
                            float ah_out = get_absolute_humidity(wt, (float)wh);

                            const char* vent_text = "---";
                            uint16_t vent_color = COLOR_LIGHT_GRAY;

                            if (ah_out < ah_in - 0.5f) {
                                vent_text = "ANO";
                                vent_color = SYS_WIFI_OK_FG;
                            } else if (ah_out > ah_in + 0.5f) {
                                vent_text = "NIE";
                                vent_color = SYS_WIFI_ERR_FG;
                            }

                            gui_draw_string(160 - (13 * 8) / 2, box_y2 + 13,
                                            "H2O VETRANIE:", COLOR_LIGHT_GRAY,
                                            THEME_BG, 1);
                            gui_draw_rect(15, box_y2 + 25, 270, 16, THEME_BG);
                            int vent_len = strlen(vent_text);
                            gui_draw_string(160 - (vent_len * 16) / 2,
                                            box_y2 + 25, vent_text, vent_color,
                                            THEME_BG, 2);

                            int left_center = 55;
                            char out_val[8];
                            snprintf(out_val, sizeof(out_val), "%.1f", ah_out);
                            int out_w = strlen(out_val) * 16;
                            int out_tot_w = out_w + 4 + 8;
                            int out_x = left_center - (out_tot_w / 2);
                            gui_draw_string(left_center - (5 * 8) / 2,
                                            box_y2 + 13, "VONKU",
                                            COLOR_LIGHT_GRAY, THEME_BG, 1);
                            gui_draw_string(out_x, box_y2 + 25, out_val,
                                            COLOR_LIGHT_GRAY, THEME_BG, 2);
                            gui_draw_string(out_x + out_w + 4, box_y2 + 33, "g",
                                            COLOR_LIGHT_GRAY, THEME_BG, 1);

                            int right_center = 265;
                            char in_val[8];
                            snprintf(in_val, sizeof(in_val), "%.1f", ah_in);
                            int in_w = strlen(in_val) * 16;
                            int in_tot_w = in_w + 4 + 8;
                            int in_x = right_center - (in_tot_w / 2);
                            gui_draw_string(right_center - (6 * 8) / 2,
                                            box_y2 + 13, "VNUTRI",
                                            COLOR_LIGHT_GRAY, THEME_BG, 1);
                            gui_draw_string(in_x, box_y2 + 25, in_val,
                                            COLOR_LIGHT_GRAY, THEME_BG, 2);
                            gui_draw_string(in_x + in_w + 4, box_y2 + 33, "g",
                                            COLOR_LIGHT_GRAY, THEME_BG, 1);

                            cache_v_t = t_r;
                            cache_v_h = h_r;
                            cache_wt = wt_r;
                            cache_wh = wh;
                        }
                    } else if (force_redraw) {
                        gui_draw_string(160 - (12 * 8) / 2, box_y2 + 21,
                                        "CAKAM NA API", GRAPH_MUTED_FG,
                                        THEME_BG, 1);
                    }
                }

                // --- OBRAZOVKA 2: Počasie (OpenWeather API) ---
                else if (current_screen == 2) {
                    float wt = 0;
                    int wh = 0, wp = 0, wid = 0;
                    if (weather_get_latest(&wt, &wh, &wp, &wid)) {
                        char i_buf[8] = {0}, d_buf[8] = {0};
                        int box_y = 30, box_w = 145, box_h = 105;

                        // --- RÁMČEK 1: Počasie a Teplota ---
                        int bx_t = 10;
                        int bx_h = 165;

                        if (force_redraw) {
                            gui_draw_round_rect_empty(bx_t, box_y, box_w, box_h,
                                                      4, WEATHER_DESC_FG);
                            gui_draw_round_rect_empty(bx_h, box_y, box_w, box_h,
                                                      4, WEATHER_HUM_FG);
                            gui_draw_string(bx_h + 10, box_y + 5, "VLHKOST",
                                            WEATHER_HUM_FG, THEME_BG, 1);
                            gui_draw_icon_16x16(bx_h + box_w - 25, box_y + 3,
                                                i_drop, WEATHER_HUM_FG,
                                                THEME_BG, 1);
                        }

                        if (force_redraw || wt != cache_wt ||
                            wid != cache_wid) {
                            gui_draw_rect(bx_t + 10, box_y + 5, 80, 16,
                                          THEME_BG);
                            gui_draw_string(bx_t + 10, box_y + 5,
                                            get_weather_desc(wid),
                                            WEATHER_DESC_FG, THEME_BG, 1);

                            const uint8_t* icon = w_cloud;
                            uint16_t i_color = ICON_CLOUD_FG;
                            if (wid >= 200 && wid < 300) {
                                icon = w_storm;
                                i_color = ICON_STORM_FG;
                            } else if (wid >= 300 && wid < 600) {
                                icon = w_rain;
                                i_color = ICON_RAIN_FG;
                            } else if (wid >= 600 && wid < 700) {
                                icon = w_snow;
                                i_color = ICON_SNOW_FG;
                            } else if (wid == 800) {
                                icon = w_sun;
                                i_color = ICON_SUN_FG;
                            } else if (wid > 800) {
                                icon = w_cloud;
                                i_color = ICON_CLOUD_FG;
                            }

                            gui_draw_rect(bx_t + box_w - 40, box_y + 5, 32, 32,
                                          THEME_BG);
                            gui_draw_icon_16x16(bx_t + box_w - 40, box_y + 5,
                                                icon, i_color, THEME_BG, 2);

                            format_sensor_val(wt, i_buf, d_buf);
                            int len_t = strlen(i_buf);
                            int px_t = bx_t + 15;
                            gui_draw_rect(bx_t + 15, box_y + 65, 125, 32,
                                          THEME_BG);
                            if (len_t == 1) px_t += 10;

                            gui_draw_string(px_t, box_y + 65, i_buf,
                                            WEATHER_TEMP_FG, THEME_BG, 4);
                            int dx_t = px_t + (len_t * 32);
                            gui_draw_string(dx_t, box_y + 81, d_buf,
                                            COLOR_LIGHT_GRAY, THEME_BG, 2);

                            int ux_t = dx_t + 36;
                            gui_draw_rect(ux_t, box_y + 65, 4, 4,
                                          WEATHER_DESC_FG);
                            gui_draw_rect(ux_t + 1, box_y + 66, 2, 2, THEME_BG);
                            gui_draw_string(ux_t + 6, box_y + 65, "C",
                                            WEATHER_DESC_FG, THEME_BG, 2);

                            cache_wt = wt;
                            cache_wid = wid;
                        }

                        if (force_redraw || wh != cache_wh) {
                            snprintf(i_buf, sizeof(i_buf), "%d", wh);
                            int len_h = strlen(i_buf);
                            int px_h = bx_h + 15;

                            gui_draw_rect(bx_h + 15, box_y + 65, 125, 32,
                                          THEME_BG);
                            if (len_h == 1) px_h += 10;

                            gui_draw_string(px_h, box_y + 65, i_buf,
                                            WEATHER_TEMP_FG, THEME_BG, 4);
                            int dx_h = px_h + (len_h * 32);
                            gui_draw_string(dx_h + 4, box_y + 81, "%",
                                            WEATHER_HUM_FG, THEME_BG, 2);

                            cache_wh = wh;
                        }
                    } else {
                        if (force_redraw) {
                            gui_draw_string(10, 30, "API ERROR / WAITING:",
                                            WEATHER_TITLE_FG, THEME_BG, 2);
                            gui_draw_string(10, 60, "Cakaj na sync...",
                                            WEATHER_TEMP_FG, THEME_BG, 2);
                        }
                    }
                }

                // Adaptivny jas displeja (smartfonovy styl, s plynulym
                // filtrom). Bezi nezavisle od vybratej obrazovky.
                if (app_config_get()->auto_brightness &&
                    (now_ms - last_brightness_update_ms) >= 1000) {
                    float t_cur = 0.0f, h_cur = 0.0f, p_cur = 0.0f,
                          lux_cur = 0.0f;
                    sensor_core_get_latest_full(&t_cur, &h_cur, &p_cur,
                                                &lux_cur);

                    if (lux_cur >= 0.0f) {
                        if (filtered_lux < 0.0f) {
                            filtered_lux = lux_cur;
                        } else {
                            filtered_lux =
                                (filtered_lux * 0.80f) + (lux_cur * 0.20f);
                        }
                        uint8_t target =
                            map_lux_to_backlight_percent(filtered_lux);
                        uint8_t current = display_hal_get_backlight_percent();

                        // Hysteresis + slew-rate limit to prevent visible
                        // flicker when lux oscillates around a threshold.
                        int delta = (int)target - (int)current;
                        if (delta >= 2 || delta <= -2) {
                            if (delta > 3) delta = 3;
                            if (delta < -3) delta = -3;
                            uint8_t next = (uint8_t)((int)current + delta);
                            display_hal_set_backlight_percent(next);
                        }
                    }
                    last_brightness_update_ms = now_ms;
                }
                // --- OBRAZOVKA 3: Ovzdušie a Tlak (Nová ATMOSPHERE) ---
                if (current_screen == 3) {
                    float wt = 0;
                    int wh = 0, wp = 0, wid = 0;
                    if (weather_get_latest(&wt, &wh, &wp, &wid)) {
                        int aqi = 0;
                        float pm25 = 0;
                        weather_get_aqi(&aqi, &pm25);

                        float lt = 0, lh = 0, lp = 0, llux = 0;
                        sensor_core_get_latest_full(&lt, &lh, &lp, &llux);
                        int local_p = (int)lp;

                        // Horný riadok: VONKAJSI / VNUTORNY tlak (layout
                        // zhodný s obrazovkou SENSORS)
                        int box_y = 30, box_w = 145, box_h = 82;
                        int bx_t = 10;
                        int bx_h = 165;
                        // Spodný riadok: Kvalita ovzdušia (cez celú šírku)
                        int box_y2 = 117, box_w2 = 300, box_h2 = 50, bx2 = 10;

                        if (force_redraw) {
                            gui_draw_round_rect_empty(bx_t, box_y, box_w,
                                                      box_h, 4,
                                                      WEATHER_PRESS_FG);
                            gui_draw_string(bx_t + 10, box_y + 5,
                                            "VONKAJSI TLAK", WEATHER_PRESS_FG,
                                            THEME_BG, 1);

                            gui_draw_round_rect_empty(bx_h, box_y, box_w,
                                                      box_h, 4,
                                                      WEATHER_PRESS_FG);
                            gui_draw_string(bx_h + 10, box_y + 5,
                                            "VNUTORNY TLAK", WEATHER_PRESS_FG,
                                            THEME_BG, 1);

                            gui_draw_round_rect_empty(bx2, box_y2, box_w2,
                                                      box_h2, 4,
                                                      COLOR_LIGHT_GRAY);
                            gui_draw_string(bx2 + 10, box_y2 + 5,
                                            "KVALITA OVZDUSIA",
                                            COLOR_LIGHT_GRAY, THEME_BG, 1);
                        }

                        // Vonkajší tlak (OpenWeatherMap)
                        if (force_redraw || wp != cache_wp) {
                            gui_draw_rect(bx_t + 10, box_y + 28, 125, 38,
                                          THEME_BG);

                            char p_buf[16];
                            snprintf(p_buf, sizeof(p_buf), "%d", wp);
                            gui_draw_string(bx_t + 10, box_y + 32, p_buf,
                                            WEATHER_TITLE_FG, THEME_BG, 3);

                            int pw = strlen(p_buf) * 24;
                            gui_draw_string(bx_t + 10 + pw + 6, box_y + 58,
                                            "hPa", COLOR_LIGHT_GRAY, THEME_BG,
                                            1);

                            cache_wp = wp;
                        }

                        // Vnútorný tlak (lokálny BME280) + barometrický
                        // trend
                        if (force_redraw || local_p != cache_local_p) {
                            gui_draw_rect(bx_h + 10, box_y + 28, 125, 38,
                                          THEME_BG);

                            char p_buf[16];
                            snprintf(p_buf, sizeof(p_buf), "%d", local_p);
                            gui_draw_string(bx_h + 10, box_y + 32, p_buf,
                                            WEATHER_TITLE_FG, THEME_BG, 3);

                            int pw = strlen(p_buf) * 24;
                            gui_draw_string(bx_h + 10 + pw + 6, box_y + 58,
                                            "hPa", COLOR_LIGHT_GRAY, THEME_BG,
                                            1);

                            int trend = storage_get_pressure_trend();
                            const char* t_str = "-";
                            uint16_t t_color = COLOR_LIGHT_GRAY;
                            if (trend == 1) {
                                t_str = "^";
                                t_color = SYS_WIFI_OK_FG;
                            } else if (trend == -1) {
                                t_str = "v";
                                t_color = SYS_WIFI_ERR_FG;
                            }
                            gui_draw_string(bx_h + box_w - 28, box_y + 32,
                                            t_str, t_color, THEME_BG, 3);

                            cache_local_p = local_p;
                        }

                        // Kvalita ovzdušia (AQI + PM2.5)
                        if (force_redraw || aqi != cache_aqi ||
                            pm25 != cache_pm25) {
                            gui_draw_rect(bx2 + 10, box_y2 + 25, 280, 20,
                                          THEME_BG);

                            const char* aqi_str = "NEZNAME";
                            uint16_t aqi_color = COLOR_LIGHT_GRAY;

                            if (aqi > 0 && aqi <= 2) {
                                aqi_str = "CISTE";
                                aqi_color = SYS_WIFI_OK_FG;
                            } else if (aqi == 3) {
                                aqi_str = "ZHORSENE";
                                aqi_color = COLOR_LIGHT_GRAY;
                            } else if (aqi > 3) {
                                aqi_str = "SMOG";
                                aqi_color = SYS_WIFI_ERR_FG;
                            }

                            gui_draw_string(bx2 + 10, box_y2 + 25, aqi_str,
                                            aqi_color, THEME_BG, 2);

                            char pm_buf[32];
                            snprintf(pm_buf, sizeof(pm_buf),
                                     "PM2.5: %.1f ug/m3", pm25);
                            gui_draw_string(bx2 + 140, box_y2 + 31, pm_buf,
                                            COLOR_LIGHT_GRAY, THEME_BG, 1);

                            cache_aqi = aqi;
                            cache_pm25 = pm25;
                        }
                    } else {
                        if (force_redraw) {
                            gui_draw_string(10, 30, "API ERROR / WAITING:",
                                            WEATHER_TITLE_FG, THEME_BG, 2);
                        }
                    }
                }
                // --- OBRAZOVKA 5: Systém a Nastavenia ---
                else if (current_screen == 5) {
                    app_config_t* cfg = app_config_get();
                    int sy = 30;

                    // Pravy stlpec: svetelny senzor (1. riadok)
                    float lt = 0.0f, lh = 0.0f, lp = 0.0f, lux = 0.0f;
                    sensor_core_get_latest_full(&lt, &lh, &lp, &lux);

                    int sx = 170;
                    gui_draw_string(sx, 30, "INTENZITA SVETLA:", SYS_LABEL_FG,
                                    THEME_BG, 1);
                    char light_buf[24];
                    snprintf(light_buf, sizeof(light_buf), "%6.2f lx", lux);
                    gui_draw_string(sx, 42, light_buf, SYS_VALUE_FG, THEME_BG,
                                    1);

                    uint8_t brightness = display_hal_get_backlight_percent();
                    char bri_buf[16];
                    snprintf(bri_buf, sizeof(bri_buf), "%3u %%", brightness);
                    gui_draw_string(sx, 54, "JAS DISPLEJA:", SYS_LABEL_FG,
                                    THEME_BG, 1);
                    gui_draw_string(sx, 66, bri_buf, SYS_VALUE_FG, THEME_BG, 1);

                    gui_draw_string(sx, 78, "AUTO JAS:", SYS_LABEL_FG, THEME_BG,
                                    1);
                    gui_draw_string(sx + 64, 78,
                                    cfg->auto_brightness ? "ON" : "OFF",
                                    cfg->auto_brightness ? SYS_WIFI_OK_FG
                                                         : COLOR_LIGHT_GRAY,
                                    THEME_BG, 1);
                    gui_draw_string(sx, 90, "Aktualizacia: ~1s",
                                    COLOR_LIGHT_GRAY, THEME_BG, 1);

                    // --- ULOZISKO (LittleFS) - pravy stlpec, pod
                    // "Aktualizacia" ---
                    gui_draw_string(sx, 104, "ULOZISKO:", SYS_LABEL_FG,
                                    THEME_BG, 1);

                    size_t fs_total = 0, fs_used = 0;
                    storage_get_fs_info(&fs_total, &fs_used);

                    if (fs_total > 0) {
                        char fs_buf[32];
                        float used_kb = fs_used / 1024.0f;
                        float total_kb = fs_total / 1024.0f;
                        float used_pct = (fs_used * 100.0f) / fs_total;
                        snprintf(fs_buf, sizeof(fs_buf), "%.0f/%.0f KB (%.0f%%)",
                                 used_kb, total_kb, used_pct);
                        gui_draw_string(sx, 116, fs_buf, SYS_VALUE_FG,
                                        THEME_BG, 1);

                        // Minimalistický Progress bar
                        int bar_w = 140;
                        int fill_w =
                            (int)(((float)fs_used / fs_total) * bar_w);
                        gui_draw_rect(sx, 128, bar_w, 8, STATUS_BORDER_FG);
                        if (fill_w > 0)
                            gui_draw_rect(sx, 128, fill_w, 8, PROGRESS_FG);
                    } else {
                        gui_draw_string(sx, 116, "FS Error", SYS_WIFI_ERR_FG,
                                        THEME_BG, 1);
                    }

                    gui_draw_string(10, sy, "ZARIADENIE:", SYS_LABEL_FG,
                                    THEME_BG, 1);
                    gui_draw_string(10, sy + 12, cfg->alias, SYS_VALUE_FG,
                                    THEME_BG, 1);
                    sy += 30;

                    gui_draw_string(10, sy, "WIFI STATUS:", SYS_LABEL_WIFI_FG,
                                    THEME_BG, 1);
                    if (wifi_scanner_is_connected()) {
                        char ssid[33];
                        char ip[16];
                        wifi_scanner_get_ssid(ssid, sizeof(ssid));
                        wifi_scanner_get_ip(ip, sizeof(ip));
                        char wifi_info[64];
                        snprintf(wifi_info, sizeof(wifi_info), "%s", ssid);
                        gui_draw_string(10, sy + 12, wifi_info, SYS_WIFI_OK_FG,
                                        THEME_BG, 1);
                        snprintf(wifi_info, sizeof(wifi_info), "%s", ip);
                        gui_draw_string(10, sy + 24, wifi_info, SYS_VALUE_FG,
                                        THEME_BG, 1);
                        sy += 42;
                    } else {
                        gui_draw_string(10, sy + 12, "Odpojene",
                                        SYS_WIFI_ERR_FG, THEME_BG, 1);
                        sy += 30;
                    }

                    gui_draw_string(10, sy, "LOKALITA (GPS):", SYS_LABEL_FG,
                                    THEME_BG, 1);
                    char loc_info[64];
                    char city[32];
                    weather_get_city(city, sizeof(city));
                    snprintf(loc_info, sizeof(loc_info), "%.4f, %.4f", cfg->lat,
                             cfg->lon);
                    gui_draw_string(10, sy + 12, loc_info, SYS_VALUE_FG,
                                    THEME_BG, 1);
                    if (strlen(city) > 0 && strcmp(city, "Nezname") != 0) {
                        gui_draw_string(10, sy + 24, city, COLOR_LIGHT_GRAY,
                                        THEME_BG, 1);
                        sy += 42;
                    } else {
                        sy += 30;
                    }

                    gui_draw_string(10, sy, "KALIBRACIA:", SYS_LABEL_FG,
                                    THEME_BG, 1);
                    char cal_info[64];
                    snprintf(cal_info, sizeof(cal_info),
                             "DHT: %+.1f | BMP: %+.1f", cfg->dht_temp_offset,
                             cfg->bmp_temp_offset);
                    gui_draw_string(10, sy + 12, cal_info, SYS_VALUE_FG,
                                    THEME_BG, 1);
                }
                // --- OBRAZOVKA 4: Graf teploty zo senzora (24h) ---
                else if (current_screen == 4) {
                    // Zanshin: Prekreslíme pri zmene obrazovky alebo
                    // každých 30 sekúnd pre rotáciu grafu.
                    bool time_to_flip = (now_ms - last_graph_redraw_time) >=
                                        graph_flip_interval_ms;
                    bool needs_graph_redraw = force_redraw || time_to_flip;

                    if (needs_graph_redraw) {
                        if (time_to_flip && !force_redraw) {
                            graph_page =
                                (graph_page + 1) % 2;  // Rotácia medzi IN a OUT
                        }
                        last_graph_redraw_time = now_ms;

                        // Vymažeme oblasť grafu iba vtedy, ak sme
                        // obrazovku práve neprepli (vtedy ju celú
                        // zmazal display_clear)
                        if (!force_redraw) {
                            gui_draw_rect(0, 25, LCD_H_RES, LCD_V_RES - 25,
                                          THEME_BG);
                        }

                        // Decentná sub-hlavička s identifikátorom (IN /
                        // OUT)
                        if (graph_page == 0) {
                            gui_draw_rect(10, 30, 8, 8, GRAPH_POINT_FG);
                            char g_title[16];
                            snprintf(g_title, sizeof(g_title), "IN (%dH)",
                                     graph_range_days * 24);
                            gui_draw_string(22, 30, g_title, GRAPH_POINT_FG,
                                            THEME_BG, 1);
                        } else {
                            gui_draw_rect(10, 30, 8, 8, GRAPH_POINT_OWM_FG);
                            char g_title[16];
                            snprintf(g_title, sizeof(g_title), "OUT (%dH)",
                                     graph_range_days * 24);
                            gui_draw_string(22, 30, g_title, GRAPH_POINT_OWM_FG,
                                            THEME_BG, 1);
                        }

                        time_t now_ts;
                        time(&now_ts);
                        uint32_t since_ts =
                            now_ts - (graph_range_days * 24 * 3600);

                        // Zanshin: Bezpečná dynamická alokácia, aby nám
                        // 7-dňové grafy neodstrelili Heap/Stack
                        int max_points = (graph_page == 0)
                                             ? (144 * graph_range_days)
                                             : (48 * graph_range_days);
                        if (max_points > 1500) max_points = 1500;

                        float* temps =
                            (float*)malloc(max_points * sizeof(float));
                        if (temps) {
                            int count = 0;
                            uint16_t line_color, point_color;

                            if (graph_page == 0) {
                                count = storage_get_temperature_history(
                                    since_ts, temps, max_points);
                                line_color = GRAPH_LINE_FG;
                                point_color = GRAPH_POINT_FG;
                            } else {
                                count = storage_get_weather_history(
                                    since_ts, temps, max_points);
                                line_color = GRAPH_LINE_OWM_FG;
                                point_color = GRAPH_POINT_OWM_FG;
                            }

                            if (count > 0) {
                                float t_min = temps[0], t_max = temps[0];
                                for (int i = 1; i < count; i++) {
                                    if (temps[i] < t_min) t_min = temps[i];
                                    if (temps[i] > t_max) t_max = temps[i];
                                }

                                // Bezpečnostná rezerva na okrajoch, aby
                                // sa linka nerezala
                                if (t_max - t_min < 2.0f) {
                                    t_max += 1.0f;
                                    t_min -= 1.0f;
                                }

                                int g_x = 42;  // Posunuté doprava pre hodnoty
                                               // osi (max 5 znakov = 40px)
                                int g_y = 65;
                                int g_w = 268;  // Zmenšené, aby sme
                                                // nepretiekli pravý
                                                // okraj obrazovky
                                int g_h = 100;

                                // Kreslenie X a Y osí (sivá farba)
                                gui_draw_rect(g_x, g_y, 2, g_h, GRAPH_AXIS_FG);
                                gui_draw_rect(g_x, g_y + g_h, g_w, 2,
                                              GRAPH_AXIS_FG);

                                char buf[16];
                                snprintf(buf, sizeof(buf), "%.1f", t_max);
                                gui_draw_string(2, g_y, buf,
                                                GRAPH_AXIS_VALUE_FG, THEME_BG,
                                                1);
                                snprintf(buf, sizeof(buf), "%.1f", t_min);
                                gui_draw_string(2, g_y + g_h - 10, buf,
                                                GRAPH_AXIS_VALUE_FG, THEME_BG,
                                                1);

                                float x_step = (float)(g_w - 5) /
                                               (count > 1 ? count - 1 : 1);

                                uint16_t fill_color =
                                    blend_color(line_color, THEME_BG, 0.30f);
                                int axis_bottom_y = g_y + g_h - 1;

                                // Krok 1: Vykreslenie výplne (plochy
                                // pod grafom)
                                if (count > 1) {
                                    int px_prev = g_x + 2;
                                    int py_prev = g_y + g_h - 2 -
                                                  (int)(((temps[0] - t_min) /
                                                         (t_max - t_min)) *
                                                        (g_h - 4));
                                    for (int i = 1; i < count; i++) {
                                        int px = g_x + 2 + (int)(i * x_step);
                                        int py = g_y + g_h - 2 -
                                                 (int)(((temps[i] - t_min) /
                                                        (t_max - t_min)) *
                                                       (g_h - 4));

                                        for (int x = px_prev; x < px; x++) {
                                            if (x >= g_x + g_w) break;
                                            float t = (float)(x - px_prev) /
                                                      (px - px_prev);
                                            int y = py_prev +
                                                    (int)(t * (py - py_prev));
                                            int rect_h = axis_bottom_y - y;
                                            if (rect_h > 0) {
                                                gui_draw_vline_fast(x, y + 1,
                                                                    rect_h,
                                                                    fill_color);
                                            }
                                        }
                                        px_prev = px;
                                        py_prev = py;
                                    }

                                    // Vykreslenie úplne posledného
                                    // stĺpca
                                    int rect_h = axis_bottom_y - py_prev;
                                    if (rect_h > 0 && px_prev < g_x + g_w) {
                                        gui_draw_vline_fast(px_prev,
                                                            py_prev + 1, rect_h,
                                                            fill_color);
                                    }
                                }

                                // Krok 1.5: Vykreslenie mriežky
                                // (Polnočné čiary prelomov dátumu)
                                struct tm timeinfo;
                                time_t since_ts_t = (time_t)since_ts;
                                localtime_r(&since_ts_t, &timeinfo);
                                timeinfo.tm_hour = 0;
                                timeinfo.tm_min = 0;
                                timeinfo.tm_sec = 0;
                                time_t midnight = mktime(&timeinfo);
                                if (midnight < since_ts_t) {
                                    midnight += 86400;  // Posun na prvú polnoc
                                                        // v rámci grafu
                                }

                                while (midnight < now_ts) {
                                    int mx =
                                        g_x + 2 +
                                        (int)(((float)(midnight - since_ts) /
                                               (now_ts - since_ts)) *
                                              (g_w - 5));
                                    if (mx > g_x && mx < g_x + g_w) {
                                        // 1px vertikálna čiara zhora
                                        // nadol (neprekrýva
                                        // horizontálnu os)
                                        gui_draw_vline_fast(mx, g_y, g_h,
                                                            GRAPH_GRID_FG);
                                    }
                                    midnight += 86400;  // Skok na ďalší deň
                                }

                                // Krok 2: Vykreslenie čiar
                                if (count > 1) {
                                    int px_prev = g_x + 2;
                                    int py_prev = g_y + g_h - 2 -
                                                  (int)(((temps[0] - t_min) /
                                                         (t_max - t_min)) *
                                                        (g_h - 4));
                                    for (int i = 1; i < count; i++) {
                                        int px = g_x + 2 + (int)(i * x_step);
                                        int py = g_y + g_h - 2 -
                                                 (int)(((temps[i] - t_min) /
                                                        (t_max - t_min)) *
                                                       (g_h - 4));
                                        gui_draw_line(px_prev, py_prev, px, py,
                                                      line_color);
                                        px_prev = px;
                                        py_prev = py;
                                    }
                                }

                                // Vykreslenie bodov
                                int last_px = -1, last_py = -1;
                                int anim_delay =
                                    2500 / count;  // Konštantný čas animácie
                                                   // ~2.5s pre plný graf
                                if (anim_delay < 2)
                                    anim_delay = 2;  // Bezpečný čas pre
                                                     // DMA radič
                                if (anim_delay > 20)
                                    anim_delay = 20;  // Zamedzenie zamrznutia
                                                      // pre málo hodnôt

                                for (int i = 0; i < count; i++) {
                                    int px = g_x + 2 + (int)(i * x_step);
                                    int py = g_y + g_h - 2 -
                                             (int)(((temps[i] - t_min) /
                                                    (t_max - t_min)) *
                                                   (g_h - 4));

                                    // Optimalizácia: Nekreslíme
                                    // viackrát na ten istý fyzický
                                    // pixel obrazovky
                                    if (px == last_px && py == last_py)
                                        continue;

                                    gui_draw_point_fast(px - 1, py - 1,
                                                        point_color);
                                    last_px = px;
                                    last_py = py;

                                    vTaskDelay(pdMS_TO_TICKS(anim_delay));

                                    // Responzivita: Ak používateľ
                                    // stlačí tlačidlo, okamžite
                                    // prerušíme kreslenie grafu
                                    if (gpio_get_level(
                                            (gpio_num_t)BOOT_BUTTON_PIN) == 0 ||
                                        gpio_get_level(
                                            (gpio_num_t)BTN_OK_PIN) == 0 ||
                                        gpio_get_level(
                                            (gpio_num_t)BTN_ESC_PIN) == 0 ||
                                        gpio_get_level(
                                            (gpio_num_t)BTN_UP_PIN) == 0 ||
                                        gpio_get_level(
                                            (gpio_num_t)BTN_DOWN_PIN) == 0) {
                                        break;
                                    }
                                }

                            } else {
                                gui_draw_string(10, 80, "Zatial malo dat...",
                                                GRAPH_MUTED_FG, THEME_BG, 2);
                            }
                            free(temps);
                        }
                    }
                }
            }

            // Vykreslíme tenkú sémantickú oddeľovaciu čiaru (len pri
            // celkovom pre-kreslení obrazovky)
            gui_draw_rect(0, 24, LCD_H_RES, 1, STATUS_BORDER_FG);

            force_redraw = false;
            last_draw_time = now_ms;
        }

        // --- ANIMÁCIA STATUS BARU & PROGRESS INDICATOR (Beží nezávisle
        // na 50ms) --- Znovu naštítame presný čas pre plynulosť po
        // dlhom renderovaní obsahu
        now_ms = esp_timer_get_time() / 1000;

        // Ak sme na obrazovke s grafom (a nie v options), vykreslíme
        // bežiaci bod zľava doprava iba raz za sekundu (odstránenie
        // blikania)
        if (current_screen == 4 && !is_in_options) {
            uint32_t elapsed = now_ms - last_graph_redraw_time;
            uint32_t elapsed_sec = elapsed / 1000;

            if (elapsed_sec != last_anim_sec) {
                last_anim_sec = elapsed_sec;

                uint32_t flip_sec = graph_flip_interval_ms / 1000;
                if (elapsed_sec > flip_sec) elapsed_sec = flip_sec;
                float progress = (float)elapsed_sec / (float)flip_sec;

                int dot_x =
                    (int)(progress * (LCD_H_RES - 10));  // Zľava doprava
                if (dot_x < 0) dot_x = 0;
                if (dot_x > LCD_H_RES - 10) dot_x = LCD_H_RES - 10;

                // Zanshin optimalizácia: Prekreslíme len ak sa bod
                // reálne posunul
                if (dot_x != last_dot_x) {
                    if (last_dot_x >= 0) {
                        // Zmažeme starý bod a obnovíme pod ním
                        // oddeľovaciu čiaru
                        gui_draw_rect(last_dot_x, 23, 10, 3, THEME_BG);
                        gui_draw_rect(last_dot_x, 24, 10, 1, STATUS_BORDER_FG);
                    }
                    // Vykreslíme nový bod
                    gui_draw_rect(dot_x, 23, 10, 3, PROGRESS_FG);
                    last_dot_x = dot_x;
                }
            }
        } else {
            last_dot_x = -1;  // Reset stavu, ak nie sme na grafe (aby
                              // sa neskôr vykreslil správne)
            last_anim_sec = 999;
        }

        // Krátka pauza (50ms) = blesková reakcia na tlačidlo +
        // neblokujeme FreeRTOS
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}