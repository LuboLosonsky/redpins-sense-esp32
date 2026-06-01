#pragma once

#include <stdbool.h>
#include <stdint.h>

// --- GLOBÁLNE KONŠTANTY ---

// API a intervaly
#define WEATHER_API_URL                       \
    "http://api.openweathermap.org/data/2.5/" \
    "weather?id=3056508&appid=bf45f6a7032650a46b26e01d879cb436&units=metric"

#define SENSOR_READ_INTERVAL_MS (5 * 1000)          // 5 sekúnd pre HMI
#define SENSOR_LOG_INTERVAL_MS (10 * 60 * 1000)     // 10 minút do CSV
#define WEATHER_FETCH_INTERVAL_MS (30 * 60 * 1000)  // 30 minút

// Cesty k súborom na LittleFS
#define FILE_SENSOR_CSV "/data/sensor.csv"
#define FILE_WEATHER_CSV "/data/weather.csv"
#define FILE_APP_CONFIG "/data/config.json"

// --- ŠTRUKTÚRA NASTAVENÍ ---
typedef struct {
    float lat;
    float lon;
    char alias[64];

    // Kalibrácia
    float dht_temp_offset;
    float bmp_temp_offset;
} app_config_t;

// Inicializácia a načítanie nastavení z LittleFS
void app_config_init(void);

// Získanie pointera na aktuálnu konfiguráciu
app_config_t* app_config_get(void);

// Uloženie aktuálneho stavu na disk (LittleFS)
void app_config_save(void);