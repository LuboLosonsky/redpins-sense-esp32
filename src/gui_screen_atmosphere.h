#pragma once
#include <stdint.h>

#include "gui_state.h"

// Obrazovka ATMOSPHERE: vonkajší/vnútorný tlak (s barometrickým trendom) a
// kvalita ovzdušia (AQI/PM2.5).
void gui_draw_screen_atmosphere(GuiState& s, uint32_t now_ms);
