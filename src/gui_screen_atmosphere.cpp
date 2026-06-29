#include "gui_screen_atmosphere.h"

#include <stdio.h>
#include <string.h>

#include "gui_colors.h"
#include "gui_primitives.h"
#include "sensor_core.h"
#include "storage.h"
#include "weather_client.h"

void gui_draw_screen_atmosphere(GuiState& s, uint32_t now_ms) {
    (void)now_ms;

    float wt = 0;
    int wh = 0, wp = 0, wid = 0;
    if (weather_get_latest(&wt, &wh, &wp, &wid)) {
        int aqi = 0;
        float pm25 = 0;
        weather_get_aqi(&aqi, &pm25);

        float lt = 0, lh = 0, lp = 0, llux = 0;
        sensor_core_get_latest_full(&lt, &lh, &lp, &llux);
        int local_p = (int)lp;

        // Horný riadok: VONKAJSI / VNUTORNY tlak (layout zhodný s
        // obrazovkou SENSORS)
        int box_y = 30, box_w = 145, box_h = 82;
        int bx_t = 10;
        int bx_h = 165;
        // Spodný riadok: Kvalita ovzdušia (cez celú šírku)
        int box_y2 = 117, box_w2 = 300, box_h2 = 50, bx2 = 10;

        if (s.force_redraw) {
            gui_draw_round_rect_empty(bx_t, box_y, box_w, box_h, 4,
                                      WEATHER_PRESS_FG);
            gui_draw_string(bx_t + 10, box_y + 5, "VONKAJSI TLAK",
                            WEATHER_PRESS_FG, THEME_BG, 1);

            gui_draw_round_rect_empty(bx_h, box_y, box_w, box_h, 4,
                                      WEATHER_PRESS_FG);
            gui_draw_string(bx_h + 10, box_y + 5, "VNUTORNY TLAK",
                            WEATHER_PRESS_FG, THEME_BG, 1);

            gui_draw_round_rect_empty(bx2, box_y2, box_w2, box_h2, 4,
                                      COLOR_LIGHT_GRAY);
            gui_draw_string(bx2 + 10, box_y2 + 5, "KVALITA OVZDUSIA",
                            COLOR_LIGHT_GRAY, THEME_BG, 1);
        }

        // Vonkajší tlak (OpenWeatherMap)
        if (s.force_redraw || wp != s.cache_wp) {
            gui_draw_rect(bx_t + 10, box_y + 28, 125, 38, THEME_BG);

            char p_buf[16];
            snprintf(p_buf, sizeof(p_buf), "%d", wp);
            gui_draw_string(bx_t + 10, box_y + 32, p_buf, WEATHER_TITLE_FG,
                            THEME_BG, 3);

            int pw = strlen(p_buf) * 24;
            gui_draw_string(bx_t + 10 + pw + 6, box_y + 58, "hPa",
                            COLOR_LIGHT_GRAY, THEME_BG, 1);

            s.cache_wp = wp;
        }

        // Vnútorný tlak (lokálny BME280) + barometrický trend
        if (s.force_redraw || local_p != s.cache_local_p) {
            gui_draw_rect(bx_h + 10, box_y + 28, 125, 38, THEME_BG);

            char p_buf[16];
            snprintf(p_buf, sizeof(p_buf), "%d", local_p);
            gui_draw_string(bx_h + 10, box_y + 32, p_buf, WEATHER_TITLE_FG,
                            THEME_BG, 3);

            int pw = strlen(p_buf) * 24;
            gui_draw_string(bx_h + 10 + pw + 6, box_y + 58, "hPa",
                            COLOR_LIGHT_GRAY, THEME_BG, 1);

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
            gui_draw_string(bx_h + box_w - 28, box_y + 32, t_str, t_color,
                            THEME_BG, 3);

            s.cache_local_p = local_p;
        }

        // Kvalita ovzdušia (AQI + PM2.5)
        if (s.force_redraw || aqi != s.cache_aqi || pm25 != s.cache_pm25) {
            gui_draw_rect(bx2 + 10, box_y2 + 25, 280, 20, THEME_BG);

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

            gui_draw_string(bx2 + 10, box_y2 + 25, aqi_str, aqi_color,
                            THEME_BG, 2);

            char pm_buf[32];
            snprintf(pm_buf, sizeof(pm_buf), "PM2.5: %.1f ug/m3", pm25);
            gui_draw_string(bx2 + 140, box_y2 + 31, pm_buf, COLOR_LIGHT_GRAY,
                            THEME_BG, 1);

            s.cache_aqi = aqi;
            s.cache_pm25 = pm25;
        }
    } else {
        if (s.force_redraw) {
            gui_draw_string(10, 30, "API ERROR / WAITING:", WEATHER_TITLE_FG,
                            THEME_BG, 2);
        }
    }
}
