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
#include "gui_helpers.h"
#include "gui_primitives.h"
#include "gui_screen_atmosphere.h"
#include "gui_screen_system.h"
#include "gui_screen_weather.h"
#include "gui_state.h"
#include "sensor_core.h"
#include "storage.h"
#include "weather_client.h"
#include "weather_icons.h"
#include "wifi_scanner.h"

static const char* TAG = "GUI";

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

// --- STAVOVÝ AUTOMAT (Obrazovky / Options menu) ---
static int gui_get_option_count(int screen) {
    if (screen == 4) return 6;
    if (screen == 5) return 3;
    return 1;
}

static void gui_close_options_menu(GuiState& s) {
    s.is_in_options = false;
    s.force_redraw = true;
    display_clear(THEME_BG);
}

static void gui_open_options_menu(GuiState& s) {
    if (s.current_screen == 4 || s.current_screen == 5) {
        s.is_in_options = true;
        s.selected_option_idx = 0;
        s.force_redraw = true;
        display_clear(THEME_BG);
    }
}

static void gui_apply_selected_option(GuiState& s) {
    if (s.current_screen == 4) {
        if (s.selected_option_idx == 0)
            s.graph_range_days = 1;
        else if (s.selected_option_idx == 1)
            s.graph_range_days = 3;
        else if (s.selected_option_idx == 2)
            s.graph_range_days = 7;
        else if (s.selected_option_idx == 3)
            s.graph_flip_interval_ms = 15000;
        else if (s.selected_option_idx == 4)
            s.graph_flip_interval_ms = 30000;
        // idx 5 je Back (nic nerobime)
    } else if (s.current_screen == 5) {
        if (s.selected_option_idx == 0) {
            s.s_display_rotated = !s.s_display_rotated;
            esp_lcd_panel_mirror(panel_handle, !s.s_display_rotated,
                                 !s.s_display_rotated);
            app_config_get()->display_rotated = s.s_display_rotated;
            app_config_save();
        } else if (s.selected_option_idx == 1) {
            app_config_get()->auto_brightness =
                !app_config_get()->auto_brightness;
            app_config_save();
        }
        // idx 2 je Back (nic nerobime)
    }
}

static void gui_next_screen(GuiState& s) {
    s.current_screen = (s.current_screen + 1) % 6;
    s.force_redraw = true;
    display_clear(THEME_BG);
}

static void gui_prev_screen(GuiState& s) {
    s.current_screen = (s.current_screen + 5) % 6;
    s.force_redraw = true;
    display_clear(THEME_BG);
}

static void gui_next_option(GuiState& s) {
    int opt_count = gui_get_option_count(s.current_screen);
    s.selected_option_idx = (s.selected_option_idx + 1) % opt_count;
    s.force_redraw = true;
}

static void gui_prev_option(GuiState& s) {
    int opt_count = gui_get_option_count(s.current_screen);
    s.selected_option_idx = (s.selected_option_idx + opt_count - 1) % opt_count;
    s.force_redraw = true;
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

    GuiState s = {};

    // Zanshin: Načítame a aplikujeme uloženú rotáciu EŠTE PRED bootovacou
    // obrazovkou. Predvolená "rovná" orientácia displeja vyžaduje (true, true).
    s.s_display_rotated = app_config_get()->display_rotated;
    esp_lcd_panel_mirror(panel_handle, !s.s_display_rotated,
                        !s.s_display_rotated);

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
    s.current_screen = 0;
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

    s.is_in_options = false;
    s.selected_option_idx = 0;
    s.graph_range_days = 1;            // Sledovanie zvoleného rozsahu v dňoch
    s.graph_flip_interval_ms = 30000;  // Interval preklápania grafov (IN/OUT)

    s.last_draw_time = 0;
    s.force_redraw = true;

    s.last_graph_redraw_time = 0;
    s.graph_page = 0;  // 0 = IN (Senzor), 1 = OUT (OWM)
    s.last_dot_x =
        -1;  // Sledovanie starej pozície bežiaceho bodu (Optimalizácia)
    s.last_anim_sec = 999;  // Pre sledovanie zmeny sekúnd

    // --- CACHE PRE ZABRÁNENIE BLIKANIU ---
    s.cache_t = -999;
    s.cache_h = -999;
    s.cache_v_t = -999;
    s.cache_v_h = -999;
    s.cache_wt = -999;
    s.cache_wh = -999;
    s.cache_wp = -999;
    s.cache_wid = -999;
    s.cache_aqi = -999;
    s.cache_pm25 = -999;
    s.cache_local_p = -999;
    s.cache_weather_sensor_ok = -1;
    s.last_brightness_update_ms = 0;
    s.filtered_lux = -1.0f;
    uint32_t last_btn_diag_ms = 0;
    int last_raw_boot = -1;
    int last_raw_ok = -1;
    int last_raw_esc = -1;
    int last_raw_up = -1;
    int last_raw_down = -1;

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
            if (s.is_in_options) {
                gui_apply_selected_option(s);
                gui_close_options_menu(s);
            } else {
                // Zanshin: Zamedzenie uviaznutia. Menu otvorime len tam, kde
                // realne su nastavenia (Grafy a System)
                gui_open_options_menu(s);
            }
        }

        // Krátke stlačenie (uvoľnenie pred limitom)
        if (btn_state == 1 && btn_last_state == 0) {
            if (!btn_long_pressed &&
                (now_ms - btn_press_time > 50)) {  // 50ms debounce
                BTN_LOGI("BTN BOOT CLICK (%ums)",
                         (unsigned)(now_ms - btn_press_time));
                if (s.is_in_options) {
                    // Cyklovanie moznosti v menu
                    gui_next_option(s);
                } else {
                    // Rotacia obrazoviek
                    gui_next_screen(s);
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
                if (s.is_in_options) {
                    BTN_LOGI("BTN OK ACTION: confirm option");
                    gui_apply_selected_option(s);
                    gui_close_options_menu(s);
                } else {
                    BTN_LOGI("BTN OK ACTION: open options");
                    gui_open_options_menu(s);
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
                if (s.is_in_options) {
                    BTN_LOGI("BTN ESC ACTION: cancel options");
                    gui_close_options_menu(s);
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
                if (s.is_in_options) {
                    BTN_LOGI("BTN UP ACTION: prev option");
                    gui_prev_option(s);
                } else {
                    BTN_LOGI("BTN UP ACTION: prev screen");
                    gui_prev_screen(s);
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
                if (s.is_in_options) {
                    BTN_LOGI("BTN DOWN ACTION: next option");
                    gui_next_option(s);
                } else {
                    BTN_LOGI("BTN DOWN ACTION: next screen");
                    gui_next_screen(s);
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
        if (!s.is_in_options && s.current_screen == 5) {
            redraw_period_ms = 1000;
        }

        // Prekreslíme len ak bolo stlačené tlačidlo, alebo uplynul refresh
        // interval.
        if (s.force_redraw ||
            (now_ms - s.last_draw_time) >= redraw_period_ms) {
            if (s.force_redraw) {
                s.cache_t = -999;
                s.cache_h = -999;
                s.cache_v_t = -999;
                s.cache_v_h = -999;
                s.cache_wt = -999;
                s.cache_wh = -999;
                s.cache_wp = -999;
                s.cache_wid = -999;
                s.cache_aqi = -999;
                s.cache_pm25 = -999;
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
            if (s.is_in_options) {
                snprintf(title_buf, sizeof(title_buf), "%-12s", "OPTIONS");
            } else {
                snprintf(title_buf, sizeof(title_buf), "%-12s",
                         screen_titles[s.current_screen]);
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

            if (s.is_in_options) {
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

                if (s.current_screen == 4) {
                    options = opt_graph;
                    count = 6;
                } else if (s.current_screen == 5) {
                    options = opt_sys;
                    count = 3;
                }

                for (int i = 0; i < count; i++) {
                    uint16_t fg =
                        (i == s.selected_option_idx) ? THEME_BG : STATUS_FG;
                    uint16_t bg =
                        (i == s.selected_option_idx) ? STATUS_FG : THEME_BG;

                    // Podfarbenie aktívneho riadku pre lepšiu viditeľnosť
                    if (i == s.selected_option_idx) {
                        gui_draw_rect(10, 30 + i * 24, 200, 22, bg);
                    } else {
                        gui_draw_rect(10, 30 + i * 24, 200, 22, THEME_BG);
                    }
                    gui_draw_string(15, 33 + i * 24, options[i], fg, bg, 2);
                }
            } else {
                // --- OBRAZOVKA 0: COMPARE (Senzor vs API, 2x2 grid) ---
                if (s.current_screen == 0) {
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

                    if (s.force_redraw) {
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
                    if (s.force_redraw || t_r != s.cache_t) {
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
                        s.cache_t = t_r;
                    }

                    // Teplota - OpenWeatherMap API
                    if (has_api) {
                        float wt_r = round1(wt);
                        if (s.force_redraw || wt_r != s.cache_wt) {
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
                            s.cache_wt = wt_r;
                        }
                    }

                    // Vlhkosť - lokálny senzor (BME280)
                    float h_r = round1(h);
                    if (s.force_redraw || h_r != s.cache_h) {
                        char hv_buf[12];
                        snprintf(hv_buf, sizeof(hv_buf), "%.1f %%", h);
                        gui_draw_rect(bx_t + 10, box_y2 + 22, 125, 20,
                                      THEME_BG);
                        gui_draw_string(bx_t + 10, box_y2 + 24, hv_buf,
                                        DASH_HUM_VAL, THEME_BG, 2);
                        s.cache_h = h_r;
                    }

                    // Vlhkosť - OpenWeatherMap API
                    if (has_api) {
                        if (s.force_redraw || wh != s.cache_wh) {
                            char hv_buf[12];
                            snprintf(hv_buf, sizeof(hv_buf), "%d %%", wh);
                            gui_draw_rect(bx_h + 10, box_y2 + 22, 125, 20,
                                          THEME_BG);
                            gui_draw_string(bx_h + 10, box_y2 + 24, hv_buf,
                                            WEATHER_HUM_FG, THEME_BG, 2);
                            s.cache_wh = wh;
                        }
                    }
                }
                // --- OBRAZOVKA 1: Hlavný Dashboard (Sensors) ---
                else if (s.current_screen == 1) {
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

                    if (s.force_redraw) {
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
                    if (s.force_redraw || t_r != s.cache_t) {
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
                        s.cache_t = t_r;
                    }

                    int weather_ok_now = sensor_core_weather_sensor_ok() ? 1
                                                                         : 0;
                    if (s.force_redraw ||
                        weather_ok_now != s.cache_weather_sensor_ok) {
                        gui_draw_rect(bx2 + 12, box_y2 + 3, 280, 9, THEME_BG);
                        gui_draw_string(
                            bx2 + 12, box_y2 + 3,
                            weather_ok_now ? "WEATHER SENZOR: OK"
                                           : "WEATHER SENZOR: FAIL",
                            weather_ok_now ? SYS_WIFI_OK_FG : SYS_WIFI_ERR_FG,
                            THEME_BG, 1);
                        s.cache_weather_sensor_ok = weather_ok_now;
                    }

                    float h_r = round1(h);
                    if (s.force_redraw || h_r != s.cache_h) {
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
                        s.cache_h = h_r;
                    }

                    float wt = 0;
                    int wh = 0, wp = 0, wid = 0;

                    // Zobrazíme asistenta len ak už máme stiahnuté dáta počasia
                    if (weather_get_latest(&wt, &wh, &wp, &wid)) {
                        float wt_r = round1(wt);
                        if (s.force_redraw || t_r != s.cache_v_t ||
                            h_r != s.cache_v_h || wt_r != s.cache_wt ||
                            wh != s.cache_wh) {
                            float ah_in = get_absolute_humidity(t, h);
                            float ah_out =
                                get_absolute_humidity(wt, (float)wh);

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
                            gui_draw_string(out_x + out_w + 4, box_y2 + 33,
                                            "g", COLOR_LIGHT_GRAY, THEME_BG,
                                            1);

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

                            s.cache_v_t = t_r;
                            s.cache_v_h = h_r;
                            s.cache_wt = wt_r;
                            s.cache_wh = wh;
                        }
                    } else if (s.force_redraw) {
                        gui_draw_string(160 - (12 * 8) / 2, box_y2 + 21,
                                        "CAKAM NA API", GRAPH_MUTED_FG,
                                        THEME_BG, 1);
                    }
                }

                // --- OBRAZOVKA 2: Počasie (OpenWeather API) ---
                else if (s.current_screen == 2) {
                    gui_draw_screen_weather(s, now_ms);
                }

                // Adaptivny jas displeja (smartfonovy styl, s plynulym
                // filtrom). Bezi nezavisle od vybratej obrazovky.
                if (app_config_get()->auto_brightness &&
                    (now_ms - s.last_brightness_update_ms) >= 1000) {
                    float t_cur = 0.0f, h_cur = 0.0f, p_cur = 0.0f,
                          lux_cur = 0.0f;
                    sensor_core_get_latest_full(&t_cur, &h_cur, &p_cur,
                                                &lux_cur);

                    if (lux_cur >= 0.0f) {
                        if (s.filtered_lux < 0.0f) {
                            s.filtered_lux = lux_cur;
                        } else {
                            s.filtered_lux =
                                (s.filtered_lux * 0.80f) + (lux_cur * 0.20f);
                        }
                        uint8_t target =
                            map_lux_to_backlight_percent(s.filtered_lux);
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
                    s.last_brightness_update_ms = now_ms;
                }
                // --- OBRAZOVKA 3: Ovzdušie a Tlak (Nová ATMOSPHERE) ---
                if (s.current_screen == 3) {
                    gui_draw_screen_atmosphere(s, now_ms);
                }
                // --- OBRAZOVKA 5: Systém a Nastavenia ---
                else if (s.current_screen == 5) {
                    gui_draw_screen_system(s, now_ms);
                }
                // --- OBRAZOVKA 4: Graf teploty zo senzora (24h) ---
                else if (s.current_screen == 4) {
                    // Zanshin: Prekreslíme pri zmene obrazovky alebo
                    // každých 30 sekúnd pre rotáciu grafu.
                    bool time_to_flip =
                        (now_ms - s.last_graph_redraw_time) >=
                        (uint32_t)s.graph_flip_interval_ms;
                    bool needs_graph_redraw = s.force_redraw || time_to_flip;

                    if (needs_graph_redraw) {
                        if (time_to_flip && !s.force_redraw) {
                            s.graph_page = (s.graph_page + 1) %
                                          2;  // Rotácia medzi IN a OUT
                        }
                        s.last_graph_redraw_time = now_ms;

                        // Vymažeme oblasť grafu iba vtedy, ak sme
                        // obrazovku práve neprepli (vtedy ju celú
                        // zmazal display_clear)
                        if (!s.force_redraw) {
                            gui_draw_rect(0, 25, LCD_H_RES, LCD_V_RES - 25,
                                          THEME_BG);
                        }

                        // Decentná sub-hlavička s identifikátorom (IN /
                        // OUT)
                        if (s.graph_page == 0) {
                            gui_draw_rect(10, 30, 8, 8, GRAPH_POINT_FG);
                            char g_title[16];
                            snprintf(g_title, sizeof(g_title), "IN (%dH)",
                                     s.graph_range_days * 24);
                            gui_draw_string(22, 30, g_title, GRAPH_POINT_FG,
                                            THEME_BG, 1);
                        } else {
                            gui_draw_rect(10, 30, 8, 8, GRAPH_POINT_OWM_FG);
                            char g_title[16];
                            snprintf(g_title, sizeof(g_title), "OUT (%dH)",
                                     s.graph_range_days * 24);
                            gui_draw_string(22, 30, g_title,
                                            GRAPH_POINT_OWM_FG, THEME_BG, 1);
                        }

                        time_t now_ts;
                        time(&now_ts);
                        uint32_t since_ts =
                            now_ts - (s.graph_range_days * 24 * 3600);

                        // Zanshin: Bezpečná dynamická alokácia, aby nám
                        // 7-dňové grafy neodstrelili Heap/Stack
                        int max_points = (s.graph_page == 0)
                                             ? (144 * s.graph_range_days)
                                             : (48 * s.graph_range_days);
                        if (max_points > 1500) max_points = 1500;

                        float* temps =
                            (float*)malloc(max_points * sizeof(float));
                        if (temps) {
                            int count = 0;
                            uint16_t line_color, point_color;

                            if (s.graph_page == 0) {
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
                                                gui_draw_vline_fast(
                                                    x, y + 1, rect_h,
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
                                                            py_prev + 1,
                                                            rect_h,
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
                                        gui_draw_line(px_prev, py_prev, px,
                                                      py, line_color);
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
                                    if (gpio_get_level((gpio_num_t)
                                                            BOOT_BUTTON_PIN) ==
                                            0 ||
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

            s.force_redraw = false;
            s.last_draw_time = now_ms;
        }

        // --- ANIMÁCIA STATUS BARU & PROGRESS INDICATOR (Beží nezávisle
        // na 50ms) --- Znovu naštítame presný čas pre plynulosť po
        // dlhom renderovaní obsahu
        now_ms = esp_timer_get_time() / 1000;

        // Ak sme na obrazovke s grafom (a nie v options), vykreslíme
        // bežiaci bod zľava doprava iba raz za sekundu (odstránenie
        // blikania)
        if (s.current_screen == 4 && !s.is_in_options) {
            uint32_t elapsed = now_ms - s.last_graph_redraw_time;
            uint32_t elapsed_sec = elapsed / 1000;

            if (elapsed_sec != s.last_anim_sec) {
                s.last_anim_sec = elapsed_sec;

                uint32_t flip_sec = s.graph_flip_interval_ms / 1000;
                if (elapsed_sec > flip_sec) elapsed_sec = flip_sec;
                float progress = (float)elapsed_sec / (float)flip_sec;

                int dot_x =
                    (int)(progress * (LCD_H_RES - 10));  // Zľava doprava
                if (dot_x < 0) dot_x = 0;
                if (dot_x > LCD_H_RES - 10) dot_x = LCD_H_RES - 10;

                // Zanshin optimalizácia: Prekreslíme len ak sa bod
                // reálne posunul
                if (dot_x != s.last_dot_x) {
                    if (s.last_dot_x >= 0) {
                        // Zmažeme starý bod a obnovíme pod ním
                        // oddeľovaciu čiaru
                        gui_draw_rect(s.last_dot_x, 23, 10, 3, THEME_BG);
                        gui_draw_rect(s.last_dot_x, 24, 10, 1,
                                      STATUS_BORDER_FG);
                    }
                    // Vykreslíme nový bod
                    gui_draw_rect(dot_x, 23, 10, 3, PROGRESS_FG);
                    s.last_dot_x = dot_x;
                }
            }
        } else {
            s.last_dot_x = -1;  // Reset stavu, ak nie sme na grafe (aby
                              // sa neskôr vykreslil správne)
            s.last_anim_sec = 999;
        }

        // Krátka pauza (50ms) = blesková reakcia na tlačidlo +
        // neblokujeme FreeRTOS
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
