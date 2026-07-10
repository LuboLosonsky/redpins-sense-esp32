#pragma once

#include "power_manager.h"

// Adresovatelna RGB LED pod akrylom (WS2812, GPIO8, cez RMT). GPIO8 je
// napriek staremu komentaru v main.cpp volny - realne I2C piny su GPIO0/1
// (sensor_core.cpp).

// Inicializuje RMT + led_strip. Idempotentne - bezpecne volat viackrat.
void rgb_led_init(void);

// Nastavi farbu LED podla aktualneho napajacieho profilu:
// PERFORMANCE = modra, BALANCED = jantarova (tlmena), LONG_LIFE = zhasnuta.
void rgb_led_set_mode_indicator(PowerMode mode);

// Nastavi jas LED v percentach (0-100), skaluje zakladne farby z
// rgb_led_set_mode_indicator. Analogia k display_hal_set_backlight_percent.
void rgb_led_set_brightness_percent(uint8_t percent);

// Vrati aktualne nastaveny jas LED v percentach (0-100).
uint8_t rgb_led_get_brightness_percent(void);

// Zhasne LED (volat pred MODE_LONG_LIFE deep sleep).
void rgb_led_off(void);
