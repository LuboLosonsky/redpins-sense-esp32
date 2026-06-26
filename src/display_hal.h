#pragma once

#include <stdint.h>

#include "esp_err.h"

#define PIN_NUM_CLK 1
#define PIN_NUM_MOSI 2
#define PIN_NUM_MISO 3
#define PIN_NUM_DC 15
#define PIN_NUM_CS 14
#define PIN_NUM_RST 22
#define PIN_NUM_BCKL 23

#define LCD_H_RES 320
#define LCD_V_RES 172

// Maximálna veľkosť prenosu pre DMA v bajtoch (čiastočný buffer)
#define LCD_TRANSFER_SIZE (LCD_H_RES * 40 * sizeof(uint16_t))

// Inicializuje SPI a esp_lcd panel (ST7789)
esp_err_t display_hal_init(void);

// Nastavi jas podsvietenia v percentach (0-100).
void display_hal_set_backlight_percent(uint8_t percent);

// Vrati aktualny jas podsvietenia v percentach (0-100).
uint8_t display_hal_get_backlight_percent(void);