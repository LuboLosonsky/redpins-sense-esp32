#include "gui_screen_system.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "display_hal.h"
#include "gui_colors.h"
#include "gui_primitives.h"
#include "sensor_core.h"
#include "storage.h"
#include "weather_client.h"
#include "wifi_scanner.h"

void gui_draw_screen_system(GuiState& s, uint32_t now_ms) {
    (void)s;
    (void)now_ms;

    app_config_t* cfg = app_config_get();
    int sy = 30;

    // Pravy stlpec: svetelny senzor (1. riadok)
    float lt = 0.0f, lh = 0.0f, lp = 0.0f, lux = 0.0f;
    sensor_core_get_latest_full(&lt, &lh, &lp, &lux);

    int sx = 170;
    gui_draw_string(sx, 30, "INTENZITA SVETLA:", SYS_LABEL_FG, THEME_BG, 1);
    char light_buf[24];
    snprintf(light_buf, sizeof(light_buf), "%6.2f lx", lux);
    gui_draw_string(sx, 42, light_buf, SYS_VALUE_FG, THEME_BG, 1);

    uint8_t brightness = display_hal_get_backlight_percent();
    char bri_buf[16];
    snprintf(bri_buf, sizeof(bri_buf), "%3u %%", brightness);
    gui_draw_string(sx, 54, "JAS DISPLEJA:", SYS_LABEL_FG, THEME_BG, 1);
    gui_draw_string(sx, 66, bri_buf, SYS_VALUE_FG, THEME_BG, 1);

    gui_draw_string(sx, 78, "AUTO JAS:", SYS_LABEL_FG, THEME_BG, 1);
    gui_draw_string(sx + 64, 78, cfg->auto_brightness ? "ON" : "OFF",
                    cfg->auto_brightness ? SYS_WIFI_OK_FG : COLOR_LIGHT_GRAY,
                    THEME_BG, 1);
    gui_draw_string(sx, 90, "Aktualizacia: ~1s", COLOR_LIGHT_GRAY, THEME_BG,
                    1);

    // --- ULOZISKO (LittleFS) - pravy stlpec, pod "Aktualizacia" ---
    gui_draw_string(sx, 104, "ULOZISKO:", SYS_LABEL_FG, THEME_BG, 1);

    size_t fs_total = 0, fs_used = 0;
    storage_get_fs_info(&fs_total, &fs_used);

    if (fs_total > 0) {
        char fs_buf[32];
        float used_kb = fs_used / 1024.0f;
        float total_kb = fs_total / 1024.0f;
        float used_pct = (fs_used * 100.0f) / fs_total;
        snprintf(fs_buf, sizeof(fs_buf), "%.0f/%.0f KB (%.0f%%)", used_kb,
                 total_kb, used_pct);
        gui_draw_string(sx, 116, fs_buf, SYS_VALUE_FG, THEME_BG, 1);

        // Minimalistický Progress bar
        int bar_w = 140;
        int fill_w = (int)(((float)fs_used / fs_total) * bar_w);
        gui_draw_rect(sx, 128, bar_w, 8, STATUS_BORDER_FG);
        if (fill_w > 0) gui_draw_rect(sx, 128, fill_w, 8, PROGRESS_FG);
    } else {
        gui_draw_string(sx, 116, "FS Error", SYS_WIFI_ERR_FG, THEME_BG, 1);
    }

    gui_draw_string(10, sy, "ZARIADENIE:", SYS_LABEL_FG, THEME_BG, 1);
    gui_draw_string(10, sy + 12, cfg->alias, SYS_VALUE_FG, THEME_BG, 1);
    sy += 30;

    gui_draw_string(10, sy, "WIFI STATUS:", SYS_LABEL_WIFI_FG, THEME_BG, 1);
    if (wifi_scanner_is_connected()) {
        char ssid[33];
        char ip[16];
        wifi_scanner_get_ssid(ssid, sizeof(ssid));
        wifi_scanner_get_ip(ip, sizeof(ip));
        char wifi_info[64];
        snprintf(wifi_info, sizeof(wifi_info), "%s", ssid);
        gui_draw_string(10, sy + 12, wifi_info, SYS_WIFI_OK_FG, THEME_BG, 1);
        snprintf(wifi_info, sizeof(wifi_info), "%s", ip);
        gui_draw_string(10, sy + 24, wifi_info, SYS_VALUE_FG, THEME_BG, 1);
        sy += 42;
    } else {
        gui_draw_string(10, sy + 12, "Odpojene", SYS_WIFI_ERR_FG, THEME_BG,
                        1);
        sy += 30;
    }

    gui_draw_string(10, sy, "LOKALITA (GPS):", SYS_LABEL_FG, THEME_BG, 1);
    char loc_info[64];
    char city[32];
    weather_get_city(city, sizeof(city));
    snprintf(loc_info, sizeof(loc_info), "%.4f, %.4f", cfg->lat, cfg->lon);
    gui_draw_string(10, sy + 12, loc_info, SYS_VALUE_FG, THEME_BG, 1);
    if (strlen(city) > 0 && strcmp(city, "Nezname") != 0) {
        gui_draw_string(10, sy + 24, city, COLOR_LIGHT_GRAY, THEME_BG, 1);
        sy += 42;
    } else {
        sy += 30;
    }

    gui_draw_string(10, sy, "KALIBRACIA:", SYS_LABEL_FG, THEME_BG, 1);
    char cal_info[64];
    snprintf(cal_info, sizeof(cal_info), "DHT: %+.1f | BMP: %+.1f",
             cfg->dht_temp_offset, cfg->bmp_temp_offset);
    gui_draw_string(10, sy + 12, cal_info, SYS_VALUE_FG, THEME_BG, 1);
}
