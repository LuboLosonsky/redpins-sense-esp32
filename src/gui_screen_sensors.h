#pragma once
#include <stdint.h>

#include "gui_state.h"

// Obrazovka SENSORS: hlavný dashboard - teplota a vlhkosť z lokálneho
// senzora, stav weather senzora, a ventilačný asistent (porovnanie
// absolútnej vlhkosti vnútri/vonku).
void gui_draw_screen_sensors(GuiState& s, uint32_t now_ms);
