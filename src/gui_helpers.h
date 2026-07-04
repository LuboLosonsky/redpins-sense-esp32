#pragma once
#include <math.h>
#include <stdint.h>
#include "weather_icons.h"

// --- ZDIEĽANÉ POMOCNÉ FUNKCIE A IKONY PRE GUI MODULY ---

// Preklad OpenWeatherMap ID na krátky text (napr. pre obrazovku WEATHER).
const char* get_weather_desc(int id);

// RGB565 ikona počasia pre dané OWM condition ID.
// data = nullptr ak ID nie je rozpoznané (nesmie nastať pri bežných OWM hodnotách).
struct WeatherIconRef {
    const uint16_t* data;
    uint8_t w;
    uint8_t h;
};
WeatherIconRef get_weather_icon(int id);

// Magnus-Tetens rovnica pre absolútnu vlhkosť (g/m3) - používa COMPARE a
// SENSORS (ventilačný asistent).
float get_absolute_humidity(float t, float h);

// Mapovanie intenzity osvetlenia (lux) na % jasu podsvietenia (SYSTEM
// auto-jas).
uint8_t map_lux_to_backlight_percent(float lux);

// Zaokrúhli na 1 desatinné miesto - presne na to, čo sa reálne zobrazuje.
// Cache musí porovnávať túto hodnotu, nie plnú float presnosť, inak
// EMA filter v sensor_core nikdy "nedobehne" na bitovo identickú hodnotu
// a displej by sa prekresľoval aj keď je zobrazený text rovnaký.
inline float round1(float v) { return roundf(v * 10.0f) / 10.0f; }

// Bezpečné formátovanie hodnoty senzora na celočíselnú a desatinnú časť
// (pre veľké scale-x4 fonty na Dashboard/COMPARE obrazovkách).
void format_sensor_val(float val, char* int_buf, char* dec_buf);

// --- IKONY PRE SENZORY (16x16, 1bpp) ---
extern const uint8_t i_thermometer[32];
extern const uint8_t i_drop[32];
