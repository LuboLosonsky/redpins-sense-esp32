#pragma once
#include <stdint.h>

#include "gui_state.h"

// Obrazovka SYSTEM: svetelný senzor/jas, úložisko, zariadenie, WiFi, GPS,
// kalibrácia.
void gui_draw_screen_system(GuiState& s, uint32_t now_ms);
