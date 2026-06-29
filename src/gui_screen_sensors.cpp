#include "gui_screen_sensors.h"

#include <stdio.h>
#include <string.h>

#include "gui_colors.h"
#include "gui_helpers.h"
#include "gui_primitives.h"
#include "sensor_core.h"
#include "weather_client.h"

void gui_draw_screen_sensors(GuiState& s, uint32_t now_ms) {
    (void)now_ms;

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
        gui_draw_string(bx_t + 10, box_y + 5, "TEPLOTA", DASH_TEMP_LBL,
                        THEME_BG, 1);
        gui_draw_icon_16x16(bx_t + box_w - 25, box_y + 3, i_thermometer,
                            DASH_TEMP_ICON, THEME_BG, 1);

        gui_draw_round_rect_empty(bx_h, box_y, box_w, box_h, 4,
                                  DASH_HUM_BORDER);
        gui_draw_string(bx_h + 10, box_y + 5, "VLHKOST", DASH_HUM_LBL,
                        THEME_BG, 1);
        gui_draw_icon_16x16(bx_h + box_w - 25, box_y + 3, i_drop,
                            DASH_HUM_ICON, THEME_BG, 1);

        gui_draw_round_rect_empty(bx2, box_y2, box_w2, box_h2, 4,
                                  COLOR_LIGHT_GRAY);
    }

    float t_r = round1(t);
    if (s.force_redraw || t_r != s.cache_t) {
        format_sensor_val(t, i_buf, d_buf);
        int len_t = strlen(i_buf);
        int px_t = bx_t + 15;
        gui_draw_rect(bx_t + 15, box_y + 35, 125, 32, THEME_BG);
        if (len_t == 1) px_t += 10;
        gui_draw_string(px_t, box_y + 35, i_buf, DASH_TEMP_VAL, THEME_BG, 4);
        int dx_t = px_t + (len_t * 32);
        gui_draw_string(dx_t, box_y + 51, d_buf, DASH_TEMP_DEC, THEME_BG, 2);
        int ux_t = dx_t + 36;
        gui_draw_rect(ux_t, box_y + 35, 4, 4, DASH_TEMP_UNIT);
        gui_draw_rect(ux_t + 1, box_y + 36, 2, 2, THEME_BG);
        gui_draw_string(ux_t + 6, box_y + 35, "C", DASH_TEMP_UNIT, THEME_BG,
                        2);
        s.cache_t = t_r;
    }

    int weather_ok_now = sensor_core_weather_sensor_ok() ? 1 : 0;
    if (s.force_redraw || weather_ok_now != s.cache_weather_sensor_ok) {
        gui_draw_rect(bx2 + 12, box_y2 + 3, 280, 9, THEME_BG);
        gui_draw_string(
            bx2 + 12, box_y2 + 3,
            weather_ok_now ? "WEATHER SENZOR: OK" : "WEATHER SENZOR: FAIL",
            weather_ok_now ? SYS_WIFI_OK_FG : SYS_WIFI_ERR_FG, THEME_BG, 1);
        s.cache_weather_sensor_ok = weather_ok_now;
    }

    float h_r = round1(h);
    if (s.force_redraw || h_r != s.cache_h) {
        format_sensor_val(h, i_buf, d_buf);
        int len_h = strlen(i_buf);
        int px_h = bx_h + 15;
        gui_draw_rect(bx_h + 15, box_y + 35, 125, 32, THEME_BG);
        if (len_h == 1) px_h += 10;
        gui_draw_string(px_h, box_y + 35, i_buf, DASH_HUM_VAL, THEME_BG, 4);
        int dx_h = px_h + (len_h * 32);
        gui_draw_string(dx_h, box_y + 51, d_buf, DASH_HUM_DEC, THEME_BG, 2);
        int ux_h = dx_h + 36;
        gui_draw_string(ux_h, box_y + 35, "%", DASH_HUM_UNIT, THEME_BG, 2);
        s.cache_h = h_r;
    }

    float wt = 0;
    int wh = 0, wp = 0, wid = 0;

    // Zobrazíme asistenta len ak už máme stiahnuté dáta počasia
    if (weather_get_latest(&wt, &wh, &wp, &wid)) {
        float wt_r = round1(wt);
        if (s.force_redraw || t_r != s.cache_v_t || h_r != s.cache_v_h ||
            wt_r != s.cache_wt || wh != s.cache_wh) {
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
                            "H2O VETRANIE:", COLOR_LIGHT_GRAY, THEME_BG, 1);
            gui_draw_rect(15, box_y2 + 25, 270, 16, THEME_BG);
            int vent_len = strlen(vent_text);
            gui_draw_string(160 - (vent_len * 16) / 2, box_y2 + 25, vent_text,
                            vent_color, THEME_BG, 2);

            int left_center = 55;
            char out_val[8];
            snprintf(out_val, sizeof(out_val), "%.1f", ah_out);
            int out_w = strlen(out_val) * 16;
            int out_tot_w = out_w + 4 + 8;
            int out_x = left_center - (out_tot_w / 2);
            gui_draw_string(left_center - (5 * 8) / 2, box_y2 + 13, "VONKU",
                            COLOR_LIGHT_GRAY, THEME_BG, 1);
            gui_draw_string(out_x, box_y2 + 25, out_val, COLOR_LIGHT_GRAY,
                            THEME_BG, 2);
            gui_draw_string(out_x + out_w + 4, box_y2 + 33, "g",
                            COLOR_LIGHT_GRAY, THEME_BG, 1);

            int right_center = 265;
            char in_val[8];
            snprintf(in_val, sizeof(in_val), "%.1f", ah_in);
            int in_w = strlen(in_val) * 16;
            int in_tot_w = in_w + 4 + 8;
            int in_x = right_center - (in_tot_w / 2);
            gui_draw_string(right_center - (6 * 8) / 2, box_y2 + 13,
                            "VNUTRI", COLOR_LIGHT_GRAY, THEME_BG, 1);
            gui_draw_string(in_x, box_y2 + 25, in_val, COLOR_LIGHT_GRAY,
                            THEME_BG, 2);
            gui_draw_string(in_x + in_w + 4, box_y2 + 33, "g",
                            COLOR_LIGHT_GRAY, THEME_BG, 1);

            s.cache_v_t = t_r;
            s.cache_v_h = h_r;
            s.cache_wt = wt_r;
            s.cache_wh = wh;
        }
    } else if (s.force_redraw) {
        gui_draw_string(160 - (12 * 8) / 2, box_y2 + 21, "CAKAM NA API",
                        GRAPH_MUTED_FG, THEME_BG, 1);
    }
}
