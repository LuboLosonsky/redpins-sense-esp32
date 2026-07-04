#include "gui_primitives.h"

#include <stdlib.h>
#include <string.h>

#include "display_hal.h"
#include "esp_lcd_panel_ops.h"
#include "font8x8.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "weather_icons.h"

extern esp_lcd_panel_handle_t panel_handle;
extern "C" void display_clear(uint16_t color);

// Zanshin: Buffer sme zväčšili na 40 riadkov (13.7 KB).
// Poskytuje dostatok priestoru aj pre veľké 32px (Scale x4) písmená v jednom
// prenose!
#define GUI_BLOCK_LINES 40
// Zarovnanie na 4 bajty je kritické pre bezpečný chod ESP32-C6 DMA!
static uint16_t render_buffer[LCD_H_RES * GUI_BLOCK_LINES]
    __attribute__((aligned(4)));

// --- LIGHTWEIGHT TEXT RENDERER ---
// Sám natlačí bajty fontu priamo do DMA buffra a pošle ho do displeja.
// Zero-Heap Allocation!
void gui_draw_string(int x, int y, const char* str, uint16_t fg, uint16_t bg,
                     int scale) {
    if (!panel_handle) return;
    int len = strlen(str);
    if (len == 0) return;

    int width = len * 8 * scale;
    int height = 8 * scale;

    // Ochrana pred vykreslením mimo obrazovky / buffra
    if (x + width > LCD_H_RES) width = LCD_H_RES - x;
    if (height > GUI_BLOCK_LINES) height = GUI_BLOCK_LINES;
    if (y + height > LCD_V_RES) height = LCD_V_RES - y;
    if (width <= 0 || height <= 0) return;

    // 1. Vyplnenie obdĺžnika textu farbou pozadia
    for (int i = 0; i < width * height; i++) {
        render_buffer[i] = __builtin_bswap16(bg);
    }

    // 2. Vykreslenie pixelov písmen s dynamickým zväčšením
    int px = 0;
    for (int i = 0; i < len; i++) {
        char c = str[i];
        if (c < 32 || c > 126) c = '?';  // Záchrana pre neznáme znaky
        const uint8_t* glyph = font8x8[c - 32];

        for (int gy = 0; gy < 8; gy++) {
            uint8_t row = glyph[gy];
            for (int gx = 0; gx < 8; gx++) {
                if (row & (1 << (7 - gx))) {  // Extrakcia bitov sprava doľava
                    for (int sy = 0; sy < scale; sy++) {
                        for (int sx = 0; sx < scale; sx++) {
                            int buf_x = px + gx * scale + sx;
                            int buf_y = gy * scale + sy;
                            if (buf_x < width && buf_y < height) {
                                render_buffer[buf_y * width + buf_x] =
                                    __builtin_bswap16(fg);
                            }
                        }
                    }
                }
            }
        }
        px += 8 * scale;
        if (px >= width) break;
    }

    // 3. Odoslanie do DMA asynchrónne
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + width, y + height,
                              render_buffer);
    // Kritické: Pauza pre radič, aby stihol odoslať buffer predtým, než ho
    // prepíše ďalšie slovo
    vTaskDelay(pdMS_TO_TICKS(10));
}

// --- IKONOVÝ RENDERER (16x16 -> Custom Scale) ---
void gui_draw_icon_16x16(int x, int y, const uint8_t* icon, uint16_t fg,
                         uint16_t bg, int scale) {
    if (!panel_handle) return;
    int width = 16 * scale;
    int height = 16 * scale;

    // Ochrana hraníc
    if (x + width > LCD_H_RES) width = LCD_H_RES - x;
    if (height > GUI_BLOCK_LINES) height = GUI_BLOCK_LINES;
    if (y + height > LCD_V_RES) height = LCD_V_RES - y;
    if (width <= 0 || height <= 0) return;

    for (int i = 0; i < width * height; i++) {
        render_buffer[i] = __builtin_bswap16(bg);
    }

    for (int gy = 0; gy < 16; gy++) {
        uint16_t row = (icon[gy * 2] << 8) | icon[gy * 2 + 1];
        for (int gx = 0; gx < 16; gx++) {
            if (row & (1 << (15 - gx))) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int buf_x = gx * scale + sx;
                        int buf_y = gy * scale + sy;
                        if (buf_x < width && buf_y < height) {
                            render_buffer[buf_y * width + buf_x] =
                                __builtin_bswap16(fg);
                        }
                    }
                }
            }
        }
    }
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + width, y + height,
                              render_buffer);
    vTaskDelay(pdMS_TO_TICKS(10));
}

// --- ŠPECIÁLNY RENDERER PRE WI-FI (Dynamické vrstvy) ---
void gui_draw_wifi_icon(int x, int y, int level, uint16_t active_fg,
                        uint16_t inactive_fg, uint16_t bg) {
    if (!panel_handle) return;
    int width = 16;
    int height = 16;

    if (x + width > LCD_H_RES) width = LCD_H_RES - x;
    if (height > GUI_BLOCK_LINES) height = GUI_BLOCK_LINES;
    if (y + height > LCD_V_RES) height = LCD_V_RES - y;
    if (width <= 0 || height <= 0) return;

    for (int i = 0; i < width * height; i++) {
        render_buffer[i] = __builtin_bswap16(bg);
    }

    for (int gy = 0; gy < 16; gy++) {
        uint16_t row = (i_wifi[gy * 2] << 8) | i_wifi[gy * 2 + 1];

        // Zanshin: Rozdelenie ikony podľa Y osi na Bodku, Stredný a Horný oblúk
        uint16_t fg = inactive_fg;
        if (level >= 1 && gy >= 11)
            fg = active_fg;  // Spodná bodka
        else if (level >= 2 && gy >= 6 && gy <= 10)
            fg = active_fg;  // Stredný oblúk
        else if (level >= 3 && gy <= 5)
            fg = active_fg;  // Horný oblúk

        for (int gx = 0; gx < 16; gx++) {
            if (row & (1 << (15 - gx))) {
                if (gx < width && gy < height) {
                    render_buffer[gy * width + gx] = __builtin_bswap16(fg);
                }
            }
        }
    }
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + width, y + height,
                              render_buffer);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static uint16_t pixel_buffer[1] __attribute__((aligned(4)));
static uint16_t last_pixel_color = 0xFFFF;

// --- ZANSHIN FIX: Rýchla vertikálna čiara a blending farieb pre plochy grafov
// ---
static uint16_t vline_buffer[120] __attribute__((aligned(4)));
static uint16_t last_vline_color = 0xFFFF;

void gui_draw_vline_fast(int x, int y, int h, uint16_t color) {
    if (!panel_handle || h <= 0 || h > 120) return;

    if (color != last_vline_color) {
        vTaskDelay(pdMS_TO_TICKS(5));  // Počkáme na DMA pred prepísaním
        for (int i = 0; i < 120; i++) {
            vline_buffer[i] = __builtin_bswap16(color);
        }
        last_vline_color = color;
    }
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 1, y + h, vline_buffer);
}

uint16_t blend_color(uint16_t fg, uint16_t bg, float alpha) {
    uint8_t fg_r = (fg >> 11) & 0x1F, fg_g = (fg >> 5) & 0x3F, fg_b = fg & 0x1F;
    uint8_t bg_r = (bg >> 11) & 0x1F, bg_g = (bg >> 5) & 0x3F, bg_b = bg & 0x1F;
    uint8_t r = bg_r + (uint8_t)((fg_r - bg_r) * alpha);
    uint8_t g = bg_g + (uint8_t)((fg_g - bg_g) * alpha);
    uint8_t b = bg_b + (uint8_t)((fg_b - bg_b) * alpha);
    return (r << 11) | (g << 5) | b;
}

// --- ZANSHIN FIX: Rýchle vykreslenie 2x2 bodu (Optimalizácia pre graf) ---
static uint16_t point_buffer[4] __attribute__((aligned(4)));
static uint16_t last_point_color = 0xFFFF;

void gui_draw_point_fast(int x, int y, uint16_t color) {
    if (!panel_handle) return;
    if (x < 0 || y < 0 || x + 2 > LCD_H_RES || y + 2 > LCD_V_RES) return;
    if (color != last_point_color) {
        vTaskDelay(pdMS_TO_TICKS(5));
        for (int i = 0; i < 4; i++) point_buffer[i] = __builtin_bswap16(color);
        last_point_color = color;
    }
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 2, y + 2, point_buffer);
}

// --- JEDNODUCHÉ KRESLENIE TVAROV (Na graf) ---
void gui_draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (!panel_handle) return;
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > LCD_H_RES) w = LCD_H_RES - x;
    if (y + h > LCD_V_RES) h = LCD_V_RES - y;
    if (w <= 0 || h <= 0) return;

    // Zanshin: Optimalizácia pre kreslenie jednotlivých pixelov (napr. v
    // grafe) Vyhneme sa zbytočnému oneskoreniu a zložitosti pre 1x1 obdĺžnik.
    if (w == 1 && h == 1) {
        // ZANSHIN FIX: Samostatný buffer pre čiary. Zabraňuje DMA šumu!
        if (color != last_pixel_color) {
            vTaskDelay(pdMS_TO_TICKS(
                5));  // Počkáme na odoslanie starej farby z fronty
            pixel_buffer[0] = __builtin_bswap16(color);
            last_pixel_color = color;
        }

        esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + 1, y + 1,
                                  pixel_buffer);
        return;
    }

    // Dynamicky rozdelíme kreslenie na bloky, ak je obdĺžnik príliš vysoký pre
    // DMA buffer
    int max_lines = GUI_BLOCK_LINES;
    int lines_to_draw = (h > max_lines) ? max_lines : h;

    for (int i = 0; i < w * lines_to_draw; i++) {
        render_buffer[i] = __builtin_bswap16(color);
    }

    int current_y = y;
    while (h > 0) {
        int draw_h = (h > max_lines) ? max_lines : h;
        esp_lcd_panel_draw_bitmap(panel_handle, x, current_y, x + w,
                                  current_y + draw_h, render_buffer);
        current_y += draw_h;
        h -= draw_h;
        vTaskDelay(pdMS_TO_TICKS(2));  // Uvoľní rádio a zabráni zaduseniu DMA
    }

    // ZANSHIN FIX: Povinný flush! Počkáme, kým DMA reálne odošle tento blok.
    // Ak to neurobíme, ďalšia funkcia prepíše render_buffer počas odosielania.
    vTaskDelay(pdMS_TO_TICKS(15));
}

// --- ZANSHIN: Ultra-ľahký zaoblený rámček (Polomer 4px) ---
void gui_draw_round_rect_empty(int x, int y, int w, int h, int r,
                               uint16_t color) {
    if (!panel_handle || w <= 0 || h <= 0) return;

    // Horná a spodná hrana
    gui_draw_rect(x + r, y, w - 2 * r, 1, color);
    gui_draw_rect(x + r, y + h - 1, w - 2 * r, 1, color);

    // Ľavá a pravá hrana
    gui_draw_rect(x, y + r, 1, h - 2 * r, color);
    gui_draw_rect(x + w - 1, y + r, 1, h - 2 * r, color);

    if (r >= 4) {
        // Zrezanie rohov pre vizuálny efekt zaoblenia
        gui_draw_rect(x + 2, y + 1, 2, 1, color);
        gui_draw_rect(x + 1, y + 2, 1, 2, color);  // TL
        gui_draw_rect(x + w - 4, y + 1, 2, 1, color);
        gui_draw_rect(x + w - 2, y + 2, 1, 2, color);  // TR
        gui_draw_rect(x + 2, y + h - 2, 2, 1, color);
        gui_draw_rect(x + 1, y + h - 4, 1, 2, color);  // BL
        gui_draw_rect(x + w - 4, y + h - 2, 2, 1, color);
        gui_draw_rect(x + w - 2, y + h - 4, 1, 2, color);  // BR
    }
}

void gui_draw_bitmap_rgb565(int x, int y, int w, int h, const uint16_t* data) {
    if (!panel_handle || w <= 0 || h <= 0) return;
    if (x < 0 || y < 0 || x + w > LCD_H_RES || y + h > LCD_V_RES) return;
    if (w * h > LCD_H_RES * GUI_BLOCK_LINES) return;
    for (int i = 0; i < w * h; i++) {
        render_buffer[i] = __builtin_bswap16(data[i]);
    }
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h, render_buffer);
    vTaskDelay(pdMS_TO_TICKS(15));
}

// Kreslenie čiary pomocou Bresenhamovho algoritmu (pixel po pixeli)
void gui_draw_line(int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int e2;

    while (1) {
        gui_draw_rect(x0, y0, 1, 1, color);  // Vďaka optimalizácii je to rýchle
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}
