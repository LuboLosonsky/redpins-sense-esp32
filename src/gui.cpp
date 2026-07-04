#include <stdio.h>
#include <time.h>

#include "app_config.h"
#include "ble_server.h"
#include "display_hal.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gui_colors.h"
#include "gui_helpers.h"
#include "gui_primitives.h"
#include "gui_screen_atmosphere.h"
#include "gui_screen_compare.h"
#include "gui_screen_graph.h"
#include "gui_screen_sensors.h"
#include "gui_screen_system.h"
#include "gui_screen_weather.h"
#include "gui_state.h"
#include "sensor_core.h"
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
    if (screen == 5) return 4;
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
        } else if (s.selected_option_idx == 2) {
            app_config_get()->auto_brightness = false;
            app_config_save();
            display_hal_set_backlight_percent(10);
        }
        // idx 3 je Back (nic nerobime)
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
        int phys_ok = gpio_get_level((gpio_num_t)BTN_OK_PIN);
        int phys_esc = gpio_get_level((gpio_num_t)BTN_ESC_PIN);
        int phys_up = gpio_get_level((gpio_num_t)BTN_UP_PIN);
        int phys_down = gpio_get_level((gpio_num_t)BTN_DOWN_PIN);

        // Zanshin: Pri otočení displeja o 180° sa otáčajú aj fyzické
        // tlačidlá - logická funkcia sa musí prehodiť, aby UP/DOWN/OK/ESC
        // sedeli s tým, čo je teraz "nahor"/"nadol" na obrazovke.
        int raw_ok, raw_esc, raw_up, raw_down;
        if (!s.s_display_rotated) {
            raw_ok = phys_down;
            raw_esc = phys_up;
            raw_up = phys_ok;
            raw_down = phys_esc;
        } else {
            raw_ok = phys_ok;
            raw_esc = phys_esc;
            raw_up = phys_up;
            raw_down = phys_down;
        }

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
        if (s.force_redraw || (now_ms - s.last_draw_time) >= redraw_period_ms) {
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
                const char* opt_sys[] = {"Otocit o 180", opt_auto_bri, "Jas: 10% test", "Back"};
                const char* opt_default[] = {"Back"};

                const char** options = opt_default;
                int count = 1;

                if (s.current_screen == 4) {
                    options = opt_graph;
                    count = 6;
                } else if (s.current_screen == 5) {
                    options = opt_sys;
                    count = 4;
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
                    gui_draw_screen_compare(s, now_ms);
                }
                // --- OBRAZOVKA 1: Hlavný Dashboard (Sensors) ---
                else if (s.current_screen == 1) {
                    gui_draw_screen_sensors(s, now_ms);
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
                                (s.filtered_lux * 0.75f) + (lux_cur * 0.25f);
                        }
                        uint8_t target =
                            map_lux_to_backlight_percent(s.filtered_lux);
                        uint8_t current = display_hal_get_backlight_percent();

                        // Hysteresis + slew-rate limit to prevent visible
                        // flicker when lux oscillates around a threshold.
                        int delta = (int)target - (int)current;
                        if (delta >= 2 || delta <= -2) {
                            if (delta > 6) delta = 6;
                            if (delta < -6) delta = -6;
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
                    gui_draw_screen_graph(s, now_ms);
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
