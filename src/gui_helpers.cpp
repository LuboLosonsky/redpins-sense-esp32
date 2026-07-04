#include "gui_helpers.h"

#include <stdio.h>
#include <string.h>

// Pomocná funkcia na preklad OWM ID na krátky text
const char* get_weather_desc(int id) {
    if (id >= 200 && id < 300) return "BURKA";
    if (id >= 300 && id < 400) return "MRHOL";
    if (id >= 500 && id < 600) return "DAZD";
    if (id >= 600 && id < 700) return "SNEH";
    if (id >= 700 && id < 800) return "HMLA";
    if (id == 800) return "JASNO";
    if (id > 800 && id < 900) return "OBLAKY";
    return "NEZNAMO";
}

// Pomocná funkcia pre výpočet absolútnej vlhkosti (Magnus-Tetens rovnica v
// g/m3) Zanshin: Rýchla matematika, žiadne zbytočné pretečenia a volania von
// externých knižníc
float get_absolute_humidity(float t, float h) {
    float e = 6.112f * exp((17.67f * t) / (t + 243.5f));
    float pv = e * (h / 100.0f);
    return (2.16679f * pv * 100.0f) / (273.15f + t);
}

// OWM condition ID -> RGB565 ikona (day a night pouzivaju tu istu sadu)
// Ref: https://openweathermap.org/weather-conditions
WeatherIconRef get_weather_icon(int id) {
    if (id >= 200 && id < 300) return {icon_thunderstorm, ICON_THUNDERSTORM_W, ICON_THUNDERSTORM_H};
    if (id >= 300 && id < 400) return {icon_rain,         ICON_RAIN_W,         ICON_RAIN_H};
    if (id >= 500 && id < 600) {
        if (id == 511)         return {icon_snow,         ICON_SNOW_W,         ICON_SNOW_H};
        if (id <= 504)         return {icon_sun_shower,   ICON_SUN_SHOWER_W,   ICON_SUN_SHOWER_H};
        return                        {icon_rain,         ICON_RAIN_W,         ICON_RAIN_H};
    }
    if (id >= 600 && id < 700) return {icon_snow,         ICON_SNOW_W,         ICON_SNOW_H};
    if (id >= 700 && id < 800) return {icon_fog,          ICON_FOG_W,          ICON_FOG_H};
    if (id == 800)             return {icon_sun,          ICON_SUN_W,          ICON_SUN_H};
    if (id == 801)             return {icon_partly_cloudy,ICON_PARTLY_CLOUDY_W,ICON_PARTLY_CLOUDY_H};
    if (id >= 802 && id < 900) return {icon_clouds,       ICON_CLOUDS_W,       ICON_CLOUDS_H};
    return {icon_sun, ICON_SUN_W, ICON_SUN_H};
}

uint8_t map_lux_to_backlight_percent(float lux) {
    // BH1750 snima aj svetlo odrazene od displeja - v tme cita ~25-35 lux
    // napriek nulovemu ambientnemu osvetleniu. Odpocitame bias aby
    // tato reflexia neudrzovala jas nad minimalnou hodnotou.
    static const float LUX_DISPLAY_BIAS = 30.0f;
    float v = lux > LUX_DISPLAY_BIAS ? lux - LUX_DISPLAY_BIAS : 0.0f;

    if (v < 1.0f)    return 5;
    if (v < 2.0f)    return 8;
    if (v < 6.0f)    return 12;
    if (v < 15.0f)   return 18;
    if (v < 35.0f)   return 25;
    if (v < 80.0f)   return 33;
    if (v < 180.0f)  return 42;
    if (v < 400.0f)  return 53;
    if (v < 900.0f)  return 65;
    if (v < 2000.0f) return 78;
    if (v < 5000.0f) return 90;
    return 100;
}

// --- IKONY PRE SENZORY (16x16, 1bpp) ---
const uint8_t i_thermometer[32] = {
    0x03, 0xC0, 0x04, 0x20, 0x04, 0x20, 0x05, 0xA0, 0x05, 0xA0, 0x05,
    0xA0, 0x05, 0xA0, 0x05, 0xA0, 0x05, 0xA0, 0x09, 0x90, 0x13, 0xC8,
    0x13, 0xC8, 0x13, 0xC8, 0x09, 0x90, 0x07, 0xE0, 0x00, 0x00};

const uint8_t i_drop[32] = {
    0x01, 0x80, 0x03, 0xC0, 0x07, 0xE0, 0x0F, 0xF0, 0x1F, 0xF8, 0x3F,
    0xFC, 0x3F, 0xFC, 0x7F, 0xFE, 0x7F, 0xFE, 0x7F, 0xFE, 0x3F, 0xFC,
    0x3F, 0xFC, 0x1F, 0xF8, 0x0F, 0xF0, 0x07, 0xE0, 0x03, 0xC0};

// Pomocná funkcia na bezpečné formátovanie hodnoty senzora
void format_sensor_val(float val, char* int_buf, char* dec_buf) {
    char str[16];
    snprintf(str, sizeof(str), "%.1f", val);
    char* dot = strchr(str, '.');
    if (dot) {
        int len = dot - str;
        strncpy(int_buf, str, len);
        int_buf[len] = '\0';
        strncpy(dec_buf, dot, 2);
        dec_buf[2] = '\0';
        if (strcmp(int_buf, "-0") == 0)
            strcpy(int_buf, "-0");  // Zachová znak mínus
    } else {
        strcpy(int_buf, str);
        strcpy(dec_buf, ".0");
    }
}
