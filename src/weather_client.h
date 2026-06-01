#pragma once

#include <stdbool.h>

// Inicializuje hlavičku súboru počasia a spustí FreeRTOS task pre sťahovanie z
// API
void weather_client_init(void);

// Získa posledné známe dáta o počasí z pamäte (vráti true, ak už nejaké máme)
bool weather_get_latest(float* temp, int* hum, int* press, int* icon_id);