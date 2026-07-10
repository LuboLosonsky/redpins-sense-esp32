#pragma once
#include "esp_err.h"

// Inicializuje LittleFS a pripojí ho na VFS cestu "/data"
esp_err_t storage_init();

// Nájde offset podľa timestampu a odpošle surové CSV dáta cez BLE
void storage_sync_sensors(uint32_t since_timestamp);

// Nájde offset podľa timestampu a odpošle surové CSV dáta o počasí cez BLE
void storage_sync_weather(uint32_t since_timestamp);

// Násilne preruší aktuálne prebiehajúci prenos dát (Stream Collision Guard)
void storage_abort_stream();

// Pripíše nový záznam na koniec histórie senzorov
void storage_log_sensor_data(uint32_t timestamp, float t, float h, float p);

// Pripíše nový záznam na koniec histórie počasia
void storage_log_weather_data(uint32_t timestamp, const char* city, float t,
                              int h, int p);

// Načíta históriu teplôt za zadaný časový úsek (max_count chráni RAM pred
// pretečením)
int storage_get_temperature_history(uint32_t since_timestamp, float* temp_array,
                                    int max_count);

// Načíta históriu teplôt z OpenWeatherMap za zadaný časový úsek
int storage_get_weather_history(uint32_t since_timestamp, float* temp_array,
                                int max_count);

// Vráti celkovú veľkosť a využité miesto na partícii LittleFS
void storage_get_fs_info(size_t* total, size_t* used);

// Vráti: 1 (Stúpa), -1 (Klesá), 0 (Stabilný), -2 (Málo dát pre výpočet)
int storage_get_pressure_trend();

// Počet záznamov (bez hlavičky) aktuálne v sensor.csv / weather.csv.
// Počítané raz pri storage_init(), potom udržiavané inkrementálne pri
// každom zápise/rotácii - lacné volanie, netreba znova čítať súbor.
int storage_get_sensor_record_count(void);
int storage_get_weather_record_count(void);