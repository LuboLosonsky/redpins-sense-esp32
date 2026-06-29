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

uint8_t map_lux_to_backlight_percent(float lux) {
    // Tuned for indoor comfort: softer low-light response, no harsh jumps.
    if (lux < 2.0f) return 14;
    if (lux < 6.0f) return 17;
    if (lux < 15.0f) return 21;
    if (lux < 35.0f) return 26;
    if (lux < 80.0f) return 32;
    if (lux < 180.0f) return 39;
    if (lux < 400.0f) return 47;
    if (lux < 900.0f) return 56;
    if (lux < 2000.0f) return 66;
    if (lux < 5000.0f) return 77;
    return 88;
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
