#include "gui_screen_weather.h"

#include <stdio.h>
#include <string.h>

#include "gui_colors.h"
#include "gui_helpers.h"
#include "gui_primitives.h"
#include "weather_client.h"
#include "weather_icons.h"

void gui_draw_screen_weather(GuiState& s, uint32_t now_ms) {
    (void)now_ms;

    float wt = 0;
    int wh = 0, wp = 0, wid = 0;
    if (weather_get_latest(&wt, &wh, &wp, &wid)) {
        char i_buf[8] = {0}, d_buf[8] = {0};
        int box_y = 30, box_w = 145, box_h = 105;

        // --- RÁMČEK 1: Počasie a Teplota ---
        int bx_t = 10;
        int bx_h = 165;

        if (s.force_redraw) {
            gui_draw_round_rect_empty(bx_t, box_y, box_w, box_h, 4,
                                      WEATHER_DESC_FG);
            gui_draw_round_rect_empty(bx_h, box_y, box_w, box_h, 4,
                                      WEATHER_HUM_FG);
            gui_draw_string(bx_h + 10, box_y + 5, "VLHKOST", WEATHER_HUM_FG,
                            THEME_BG, 1);
            gui_draw_icon_16x16(bx_h + box_w - 25, box_y + 3, i_drop,
                                WEATHER_HUM_FG, THEME_BG, 1);
        }

        if (s.force_redraw || wt != s.cache_wt || wid != s.cache_wid) {
            gui_draw_rect(bx_t + 10, box_y + 5, 80, 16, THEME_BG);
            gui_draw_string(bx_t + 10, box_y + 5, get_weather_desc(wid),
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

            gui_draw_rect(bx_t + box_w - 40, box_y + 5, 32, 32, THEME_BG);
            gui_draw_icon_16x16(bx_t + box_w - 40, box_y + 5, icon, i_color,
                                THEME_BG, 2);

            format_sensor_val(wt, i_buf, d_buf);
            int len_t = strlen(i_buf);
            int px_t = bx_t + 15;
            gui_draw_rect(bx_t + 15, box_y + 65, 125, 32, THEME_BG);
            if (len_t == 1) px_t += 10;

            gui_draw_string(px_t, box_y + 65, i_buf, WEATHER_TEMP_FG,
                            THEME_BG, 4);
            int dx_t = px_t + (len_t * 32);
            gui_draw_string(dx_t, box_y + 81, d_buf, COLOR_LIGHT_GRAY,
                            THEME_BG, 2);

            int ux_t = dx_t + 36;
            gui_draw_rect(ux_t, box_y + 65, 4, 4, WEATHER_DESC_FG);
            gui_draw_rect(ux_t + 1, box_y + 66, 2, 2, THEME_BG);
            gui_draw_string(ux_t + 6, box_y + 65, "C", WEATHER_DESC_FG,
                            THEME_BG, 2);

            s.cache_wt = wt;
            s.cache_wid = wid;
        }

        if (s.force_redraw || wh != s.cache_wh) {
            snprintf(i_buf, sizeof(i_buf), "%d", wh);
            int len_h = strlen(i_buf);
            int px_h = bx_h + 15;

            gui_draw_rect(bx_h + 15, box_y + 65, 125, 32, THEME_BG);
            if (len_h == 1) px_h += 10;

            gui_draw_string(px_h, box_y + 65, i_buf, WEATHER_TEMP_FG,
                            THEME_BG, 4);
            int dx_h = px_h + (len_h * 32);
            gui_draw_string(dx_h + 4, box_y + 81, "%", WEATHER_HUM_FG,
                            THEME_BG, 2);

            s.cache_wh = wh;
        }
    } else {
        if (s.force_redraw) {
            gui_draw_string(10, 30, "API ERROR / WAITING:", WEATHER_TITLE_FG,
                            THEME_BG, 2);
            gui_draw_string(10, 60, "Cakaj na sync...", WEATHER_TEMP_FG,
                            THEME_BG, 2);
        }
    }
}
