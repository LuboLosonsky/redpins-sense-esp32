#pragma once
#include <stdint.h>

#include "gui_state.h"

// Obrazovka TEMPERATURE: 24h/3d/7d graf teploty (lokálny senzor IN / OWM
// OUT, prepínané každých 15s/30s), s animovaným vykresľovaním bodov.
void gui_draw_screen_graph(GuiState& s, uint32_t now_ms);
