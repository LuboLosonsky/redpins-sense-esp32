#pragma once
#include <stdint.h>

#include "gui_state.h"

// Obrazovka WEATHER: aktuálne počasie z OpenWeatherMap (teplota, vlhkosť,
// popis, ikona).
void gui_draw_screen_weather(GuiState& s, uint32_t now_ms);
