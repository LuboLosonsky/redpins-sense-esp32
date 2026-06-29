#include "gui_screen_graph.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "display_hal.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gui_colors.h"
#include "gui_primitives.h"
#include "storage.h"

void gui_draw_screen_graph(GuiState& s, uint32_t now_ms) {
    // Zanshin: Prekreslíme pri zmene obrazovky alebo každých 30 sekúnd
    // pre rotáciu grafu.
    bool time_to_flip = (now_ms - s.last_graph_redraw_time) >=
                        (uint32_t)s.graph_flip_interval_ms;
    bool needs_graph_redraw = s.force_redraw || time_to_flip;

    if (!needs_graph_redraw) return;

    if (time_to_flip && !s.force_redraw) {
        s.graph_page = (s.graph_page + 1) % 2;  // Rotácia medzi IN a OUT
    }
    s.last_graph_redraw_time = now_ms;

    // Vymažeme oblasť grafu iba vtedy, ak sme obrazovku práve neprepli
    // (vtedy ju celú zmazal display_clear)
    if (!s.force_redraw) {
        gui_draw_rect(0, 25, LCD_H_RES, LCD_V_RES - 25, THEME_BG);
    }

    // Decentná sub-hlavička s identifikátorom (IN / OUT)
    if (s.graph_page == 0) {
        gui_draw_rect(10, 30, 8, 8, GRAPH_POINT_FG);
        char g_title[16];
        snprintf(g_title, sizeof(g_title), "IN (%dH)", s.graph_range_days * 24);
        gui_draw_string(22, 30, g_title, GRAPH_POINT_FG, THEME_BG, 1);
    } else {
        gui_draw_rect(10, 30, 8, 8, GRAPH_POINT_OWM_FG);
        char g_title[16];
        snprintf(g_title, sizeof(g_title), "OUT (%dH)",
                 s.graph_range_days * 24);
        gui_draw_string(22, 30, g_title, GRAPH_POINT_OWM_FG, THEME_BG, 1);
    }

    time_t now_ts;
    time(&now_ts);
    uint32_t since_ts = now_ts - (s.graph_range_days * 24 * 3600);

    // Zanshin: Bezpečná dynamická alokácia, aby nám 7-dňové grafy
    // neodstrelili Heap/Stack
    int max_points = (s.graph_page == 0) ? (144 * s.graph_range_days)
                                         : (48 * s.graph_range_days);
    if (max_points > 1500) max_points = 1500;

    float* temps = (float*)malloc(max_points * sizeof(float));
    if (!temps) return;

    int count = 0;
    uint16_t line_color, point_color;

    if (s.graph_page == 0) {
        count = storage_get_temperature_history(since_ts, temps, max_points);
        line_color = GRAPH_LINE_FG;
        point_color = GRAPH_POINT_FG;
    } else {
        count = storage_get_weather_history(since_ts, temps, max_points);
        line_color = GRAPH_LINE_OWM_FG;
        point_color = GRAPH_POINT_OWM_FG;
    }

    if (count > 0) {
        float t_min = temps[0], t_max = temps[0];
        for (int i = 1; i < count; i++) {
            if (temps[i] < t_min) t_min = temps[i];
            if (temps[i] > t_max) t_max = temps[i];
        }

        // Bezpečnostná rezerva na okrajoch, aby sa linka nerezala
        if (t_max - t_min < 2.0f) {
            t_max += 1.0f;
            t_min -= 1.0f;
        }

        int g_x = 42;  // Posunuté doprava pre hodnoty osi (max 5 znakov =
                       // 40px)
        int g_y = 65;
        int g_w = 268;  // Zmenšené, aby sme nepretiekli pravý okraj
                        // obrazovky
        int g_h = 100;

        // Kreslenie X a Y osí (sivá farba)
        gui_draw_rect(g_x, g_y, 2, g_h, GRAPH_AXIS_FG);
        gui_draw_rect(g_x, g_y + g_h, g_w, 2, GRAPH_AXIS_FG);

        char buf[16];
        snprintf(buf, sizeof(buf), "%.1f", t_max);
        gui_draw_string(2, g_y, buf, GRAPH_AXIS_VALUE_FG, THEME_BG, 1);
        snprintf(buf, sizeof(buf), "%.1f", t_min);
        gui_draw_string(2, g_y + g_h - 10, buf, GRAPH_AXIS_VALUE_FG, THEME_BG,
                        1);

        float x_step = (float)(g_w - 5) / (count > 1 ? count - 1 : 1);

        uint16_t fill_color = blend_color(line_color, THEME_BG, 0.30f);
        int axis_bottom_y = g_y + g_h - 1;

        // Krok 1: Vykreslenie výplne (plochy pod grafom)
        if (count > 1) {
            int px_prev = g_x + 2;
            int py_prev = g_y + g_h - 2 -
                          (int)(((temps[0] - t_min) / (t_max - t_min)) *
                                (g_h - 4));
            for (int i = 1; i < count; i++) {
                int px = g_x + 2 + (int)(i * x_step);
                int py = g_y + g_h - 2 -
                         (int)(((temps[i] - t_min) / (t_max - t_min)) *
                               (g_h - 4));

                for (int x = px_prev; x < px; x++) {
                    if (x >= g_x + g_w) break;
                    float t = (float)(x - px_prev) / (px - px_prev);
                    int y = py_prev + (int)(t * (py - py_prev));
                    int rect_h = axis_bottom_y - y;
                    if (rect_h > 0) {
                        gui_draw_vline_fast(x, y + 1, rect_h, fill_color);
                    }
                }
                px_prev = px;
                py_prev = py;
            }

            // Vykreslenie úplne posledného stĺpca
            int rect_h = axis_bottom_y - py_prev;
            if (rect_h > 0 && px_prev < g_x + g_w) {
                gui_draw_vline_fast(px_prev, py_prev + 1, rect_h, fill_color);
            }
        }

        // Krok 1.5: Vykreslenie mriežky (Polnočné čiary prelomov dátumu)
        struct tm timeinfo;
        time_t since_ts_t = (time_t)since_ts;
        localtime_r(&since_ts_t, &timeinfo);
        timeinfo.tm_hour = 0;
        timeinfo.tm_min = 0;
        timeinfo.tm_sec = 0;
        time_t midnight = mktime(&timeinfo);
        if (midnight < since_ts_t) {
            midnight += 86400;  // Posun na prvú polnoc v rámci grafu
        }

        while (midnight < now_ts) {
            int mx = g_x + 2 +
                     (int)(((float)(midnight - since_ts) /
                            (now_ts - since_ts)) *
                           (g_w - 5));
            if (mx > g_x && mx < g_x + g_w) {
                // 1px vertikálna čiara zhora nadol (neprekrýva
                // horizontálnu os)
                gui_draw_vline_fast(mx, g_y, g_h, GRAPH_GRID_FG);
            }
            midnight += 86400;  // Skok na ďalší deň
        }

        // Krok 2: Vykreslenie čiar
        if (count > 1) {
            int px_prev = g_x + 2;
            int py_prev = g_y + g_h - 2 -
                          (int)(((temps[0] - t_min) / (t_max - t_min)) *
                                (g_h - 4));
            for (int i = 1; i < count; i++) {
                int px = g_x + 2 + (int)(i * x_step);
                int py = g_y + g_h - 2 -
                         (int)(((temps[i] - t_min) / (t_max - t_min)) *
                               (g_h - 4));
                gui_draw_line(px_prev, py_prev, px, py, line_color);
                px_prev = px;
                py_prev = py;
            }
        }

        // Vykreslenie bodov
        int last_px = -1, last_py = -1;
        int anim_delay = 2500 / count;  // Konštantný čas animácie ~2.5s
                                        // pre plný graf
        if (anim_delay < 2) anim_delay = 2;  // Bezpečný čas pre DMA radič
        if (anim_delay > 20)
            anim_delay = 20;  // Zamedzenie zamrznutia pre málo hodnôt

        for (int i = 0; i < count; i++) {
            int px = g_x + 2 + (int)(i * x_step);
            int py = g_y + g_h - 2 -
                     (int)(((temps[i] - t_min) / (t_max - t_min)) *
                           (g_h - 4));

            // Optimalizácia: Nekreslíme viackrát na ten istý fyzický
            // pixel obrazovky
            if (px == last_px && py == last_py) continue;

            gui_draw_point_fast(px - 1, py - 1, point_color);
            last_px = px;
            last_py = py;

            vTaskDelay(pdMS_TO_TICKS(anim_delay));

            // Responzivita: Ak používateľ stlačí tlačidlo, okamžite
            // prerušíme kreslenie grafu
            if (gpio_get_level((gpio_num_t)BOOT_BUTTON_PIN) == 0 ||
                gpio_get_level((gpio_num_t)BTN_OK_PIN) == 0 ||
                gpio_get_level((gpio_num_t)BTN_ESC_PIN) == 0 ||
                gpio_get_level((gpio_num_t)BTN_UP_PIN) == 0 ||
                gpio_get_level((gpio_num_t)BTN_DOWN_PIN) == 0) {
                break;
            }
        }

    } else {
        gui_draw_string(10, 80, "Zatial malo dat...", GRAPH_MUTED_FG,
                        THEME_BG, 2);
    }
    free(temps);
}
