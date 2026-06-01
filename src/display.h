#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Inicializuje SPI zbernicu, DMA a samotný panel ST7789
esp_err_t display_init(void);

// Vymaže obrazovku (vyplní ju jednou farbou)
// color: RGB565 formát (napr. 0x0000 pre čiernu, 0xF800 pre červenú)
void display_clear(uint16_t color);

#ifdef __cplusplus
}
#endif