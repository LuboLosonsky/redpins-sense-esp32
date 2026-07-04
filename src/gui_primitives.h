#pragma once
#include <stdint.h>

// --- LIGHTWEIGHT DRAW PRIMITIVES (Zero-Heap, priamy DMA prenos) ---
// Spoločný "toolkit" pre všetky gui_screen_*.cpp moduly.

void gui_draw_string(int x, int y, const char* str, uint16_t fg, uint16_t bg,
                     int scale);
void gui_draw_icon_16x16(int x, int y, const uint8_t* icon, uint16_t fg,
                         uint16_t bg, int scale);
void gui_draw_wifi_icon(int x, int y, int level, uint16_t active_fg,
                        uint16_t inactive_fg, uint16_t bg);
void gui_draw_vline_fast(int x, int y, int h, uint16_t color);
void gui_draw_point_fast(int x, int y, uint16_t color);
void gui_draw_rect(int x, int y, int w, int h, uint16_t color);
void gui_draw_round_rect_empty(int x, int y, int w, int h, int r,
                               uint16_t color);
void gui_draw_line(int x0, int y0, int x1, int y1, uint16_t color);

// Zmiešanie dvoch RGB565 farieb (napr. priehľadná výplň pod čiarou grafu).
uint16_t blend_color(uint16_t fg, uint16_t bg, float alpha);

// Bitmapa RGB565 (napr. ikona počasia). 0x0000 = čierna = THEME_BG (žiadna
// špeciálna transparencia nie je potrebná na dark-mode displeji).
void gui_draw_bitmap_rgb565(int x, int y, int w, int h, const uint16_t* data);
