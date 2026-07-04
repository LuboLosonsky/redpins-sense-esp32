#include "gui_screen_weather.h"

#include <stdio.h>
#include <string.h>

#include "gui_colors.h"
#include "gui_helpers.h"
#include "gui_primitives.h"
#include "weather_client.h"

void gui_draw_screen_weather(GuiState& s, uint32_t now_ms) {
    (void)now_ms;

    float wt = 0;
    int wh = 0, wp = 0, wid = 0;
    if (weather_get_latest(&wt, &wh, &wp, &wid)) {
        char i_buf[8] = {0}, d_buf[8] = {0};

        // Layout - lavý stĺpec identický s COMPARE obrazovkou
        const int bx_l   = 10;
        const int bx_r   = 165;
        const int box_w  = 145;
        const int box_y  = 30;
        const int box_h1 = 82;   // riadok 1: teplota
        const int box_y2 = 117;  // riadok 2: vlhkosť
        const int box_h2 = 50;
        const int box_rh = (box_y2 - box_y) + box_h2;  // = 137, výška pravého boxu

        if (s.force_redraw) {
            // Ľavý stĺpec, riadok 1: Teplota API
            gui_draw_round_rect_empty(bx_l, box_y, box_w, box_h1, 4, WEATHER_DESC_FG);
            gui_draw_string(bx_l + 10, box_y + 5, "TEPLOTA API", WEATHER_DESC_FG,
                            THEME_BG, 1);
            gui_draw_icon_16x16(bx_l + box_w - 25, box_y + 3, i_thermometer,
                                WEATHER_DESC_FG, THEME_BG, 1);

            // Ľavý stĺpec, riadok 2: Vlhkosť API
            gui_draw_round_rect_empty(bx_l, box_y2, box_w, box_h2, 4, WEATHER_HUM_FG);
            gui_draw_string(bx_l + 10, box_y2 + 5, "VLHKOST API", WEATHER_HUM_FG,
                            THEME_BG, 1);
            gui_draw_icon_16x16(bx_l + box_w - 25, box_y2 + 3, i_drop,
                                WEATHER_HUM_FG, THEME_BG, 1);

            // Pravý stĺpec: ikona + popis
            gui_draw_round_rect_empty(bx_r, box_y, box_w, box_rh, 4, WEATHER_TITLE_FG);
        }

        // --- Teplota ---
        float wt_r = round1(wt);
        if (s.force_redraw || wt_r != s.cache_wt) {
            format_sensor_val(wt, i_buf, d_buf);
            int len_t = strlen(i_buf);
            int px_t  = bx_l + 15;
            gui_draw_rect(bx_l + 15, box_y + 35, 125, 32, THEME_BG);
            if (len_t == 1) px_t += 10;
            gui_draw_string(px_t, box_y + 35, i_buf, WEATHER_TEMP_FG, THEME_BG, 4);
            int dx_t = px_t + (len_t * 32);
            gui_draw_string(dx_t, box_y + 51, d_buf, COLOR_LIGHT_GRAY, THEME_BG, 2);
            int ux_t = dx_t + 36;
            gui_draw_rect(ux_t, box_y + 35, 4, 4, WEATHER_DESC_FG);
            gui_draw_rect(ux_t + 1, box_y + 36, 2, 2, THEME_BG);
            gui_draw_string(ux_t + 6, box_y + 35, "C", WEATHER_DESC_FG, THEME_BG, 2);
            s.cache_wt = wt_r;
        }

        // --- Vlhkosť ---
        if (s.force_redraw || wh != s.cache_wh) {
            char hv_buf[12];
            snprintf(hv_buf, sizeof(hv_buf), "%d %%", wh);
            gui_draw_rect(bx_l + 10, box_y2 + 22, 125, 20, THEME_BG);
            gui_draw_string(bx_l + 10, box_y2 + 24, hv_buf, WEATHER_HUM_FG,
                            THEME_BG, 2);
            s.cache_wh = wh;
        }

        // --- Ikona + popis počasia ---
        if (s.force_redraw || wid != s.cache_wid) {
            WeatherIconRef icon = get_weather_icon(wid);
            const char*    desc = get_weather_desc(wid);

            // Plocha pre ikonu: vnútro boxu minus oblasť textu dole (26px)
            const int icon_area_h = box_rh - 26;
            const int icon_area_y = box_y + 2;

            // Vymazanie plochy ikony a textu
            gui_draw_rect(bx_r + 2, icon_area_y, box_w - 4, icon_area_h, THEME_BG);
            gui_draw_rect(bx_r + 4, box_y + box_rh - 24, box_w - 8, 20, THEME_BG);

            // Ikona: centrovana horizontálne aj vertikálne v ploche ikony
            int ix = bx_r + (box_w - (int)icon.w) / 2;
            int iy = icon_area_y + (icon_area_h - (int)icon.h) / 2;
            if (iy < icon_area_y) iy = icon_area_y;
            gui_draw_bitmap_rgb565(ix, iy, icon.w, icon.h, icon.data);

            // Popis: centrovaný horizontálne, pevná pozícia dole v boxe
            int text_w = strlen(desc) * 16;  // scale 2 = 8*2 px/znak
            int tx     = bx_r + (box_w - text_w) / 2;
            int ty     = box_y + box_rh - 22;
            gui_draw_string(tx, ty, desc, WEATHER_TITLE_FG, THEME_BG, 2);

            s.cache_wid = wid;
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
