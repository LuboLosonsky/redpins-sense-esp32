#pragma once

#include <stdbool.h>
#include <stddef.h>

// Inicializuje hlavičku súboru počasia a spustí FreeRTOS task pre sťahovanie z
// API
void weather_client_init(void);

// Nacita posledny znamy zaznam z weather.csv do RAM cache BEZ WiFi/HTTP a
// bez spustenia weather_fetch_task. Pouzite pri fast-wake z MODE_LONG_LIFE,
// kde sa radio stack zamerne nespusta.
void weather_client_load_last_known(void);

// Získa posledné známe dáta o počasí z pamäte (vráti true, ak už nejaké máme)
bool weather_get_latest(float* temp, int* hum, int* press, int* icon_id);

// Získa dáta o znečistení ovzdušia (AQI a PM2.5) z pamäte
bool weather_get_aqi(int* aqi, float* pm25);

// Získanie názvu mesta (z poslednej OpenWeatherMap odpovede)
void weather_get_city(char* city_buf, size_t max_len);