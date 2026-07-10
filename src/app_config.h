#pragma once

#include <stdbool.h>
#include <stdint.h>

// --- GLOBÁLNE KONŠTANTY ---

#define SENSOR_READ_INTERVAL_MS (5 * 1000)       // 5 sekúnd pre HMI
#define SENSOR_LOG_INTERVAL_MS (10 * 60 * 1000)  // 10 minút do CSV
#define WEATHER_FETCH_INTERVAL_MS \
    (20 * 60 * 1000)  // 20 minút (Max 3x za hodinu)

// Cesty k súborom na LittleFS
#define FILE_SENSOR_CSV "/data/sensor.csv"
#define FILE_WEATHER_CSV "/data/weather.csv"
#define FILE_APP_CONFIG "/data/config.json"

// --- ŠTRUKTÚRA NASTAVENÍ ---
typedef struct {
    float lat;
    float lon;
    char alias[64];
    char weather_api_key[40];

    // Kalibrácia
    float dht_temp_offset;
    float bmp_temp_offset;

    bool display_rotated;  // Orientácia displeja (otočenie o 180°)
    bool auto_brightness;  // Adaptivny jas podla lux (BH1750)
    uint8_t led_brightness_percent;  // Jas RGB LED indikatora (0-100)
} app_config_t;

// Inicializácia a načítanie nastavení z LittleFS
void app_config_init(void);

// Získanie pointera na aktuálnu konfiguráciu
app_config_t* app_config_get(void);

// Uloženie aktuálneho stavu na disk (LittleFS)
void app_config_save(void);