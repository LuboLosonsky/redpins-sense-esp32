#include "gui_screen_compare.h"

#include <stdio.h>
#include <string.h>

#include "gui_colors.h"
#include "gui_helpers.h"
#include "gui_primitives.h"
#include "sensor_core.h"
#include "weather_client.h"

void gui_draw_screen_compare(GuiState& s, uint32_t now_ms) {
    (void)now_ms;

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
        gui_draw_string(bx_t + 10, box_y + 5, "TEPLOTA SENZOR", DASH_TEMP_LBL,
                        THEME_BG, 1);
        gui_draw_icon_16x16(bx_t + box_w - 25, box_y + 3, i_thermometer,
                            DASH_TEMP_ICON, THEME_BG, 1);

        gui_draw_round_rect_empty(bx_h, box_y, box_w, box_h, 4,
                                  WEATHER_DESC_FG);
        gui_draw_string(bx_h + 10, box_y + 5, "TEPLOTA API", WEATHER_DESC_FG,
                        THEME_BG, 1);

        gui_draw_round_rect_empty(bx_t, box_y2, box_w, box_h2, 4,
                                  DASH_HUM_BORDER);
        gui_draw_string(bx_t + 10, box_y2 + 5, "VLHKOST SENZOR", DASH_HUM_LBL,
                        THEME_BG, 1);

        gui_draw_round_rect_empty(bx_h, box_y2, box_w, box_h2, 4,
                                  WEATHER_HUM_FG);
        gui_draw_string(bx_h + 10, box_y2 + 5, "VLHKOST API", WEATHER_HUM_FG,
                        THEME_BG, 1);

        if (!has_api) {
            gui_draw_string(bx_h + 15, box_y + 45, "CAKAM...", GRAPH_MUTED_FG,
                            THEME_BG, 1);
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

    // Teplota - OpenWeatherMap API
    if (has_api) {
        float wt_r = round1(wt);
        if (s.force_redraw || wt_r != s.cache_wt) {
            format_sensor_val(wt, i_buf, d_buf);
            int len_t = strlen(i_buf);
            int px_t = bx_h + 15;
            gui_draw_rect(bx_h + 15, box_y + 35, 125, 32, THEME_BG);
            if (len_t == 1) px_t += 10;
            gui_draw_string(px_t, box_y + 35, i_buf, WEATHER_TEMP_FG,
                            THEME_BG, 4);
            int dx_t = px_t + (len_t * 32);
            gui_draw_string(dx_t, box_y + 51, d_buf, COLOR_LIGHT_GRAY,
                            THEME_BG, 2);
            int ux_t = dx_t + 36;
            gui_draw_string(ux_t + 6, box_y + 35, "C", WEATHER_DESC_FG,
                            THEME_BG, 2);
            s.cache_wt = wt_r;
        }
    }

    // Vlhkosť - lokálny senzor (BME280)
    float h_r = round1(h);
    if (s.force_redraw || h_r != s.cache_h) {
        char hv_buf[12];
        snprintf(hv_buf, sizeof(hv_buf), "%.1f %%", h);
        gui_draw_rect(bx_t + 10, box_y2 + 22, 125, 20, THEME_BG);
        gui_draw_string(bx_t + 10, box_y2 + 24, hv_buf, DASH_HUM_VAL,
                        THEME_BG, 2);
        s.cache_h = h_r;
    }

    // Vlhkosť - OpenWeatherMap API
    if (has_api) {
        if (s.force_redraw || wh != s.cache_wh) {
            char hv_buf[12];
            snprintf(hv_buf, sizeof(hv_buf), "%d %%", wh);
            gui_draw_rect(bx_h + 10, box_y2 + 22, 125, 20, THEME_BG);
            gui_draw_string(bx_h + 10, box_y2 + 24, hv_buf, WEATHER_HUM_FG,
                            THEME_BG, 2);
            s.cache_wh = wh;
        }
    }
}
