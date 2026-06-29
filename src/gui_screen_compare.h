#pragma once
#include <stdint.h>

#include "gui_state.h"

// Obrazovka COMPARE: 2x2 grid porovnávajúci lokálny senzor (BME280) s
// OpenWeatherMap API - teplota a vlhkosť oboch zdrojov vedľa seba.
void gui_draw_screen_compare(GuiState& s, uint32_t now_ms);
