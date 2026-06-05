#pragma once
#include "esp_err.h"

// Inicializuje LittleFS a pripojí ho na VFS cestu "/data"
esp_err_t storage_init();

// Nájde offset podľa timestampu a odpošle surové CSV dáta cez BLE
void storage_sync_sensors(uint32_t since_timestamp);

// Nájde offset podľa timestampu a odpošle surové CSV dáta o počasí cez BLE
void storage_sync_weather(uint32_t since_timestamp);

// Pripíše nový záznam na koniec histórie senzorov
void storage_log_sensor_data(uint32_t timestamp, float t, float h, float p);

// Pripíše nový záznam na koniec histórie počasia
void storage_log_weather_data(uint32_t timestamp, float t, int h, int p,
                              int id);

// Načíta históriu teplôt za zadaný časový úsek (max_count chráni RAM pred
// pretečením)
int storage_get_temperature_history(uint32_t since_timestamp, float* temp_array,
                                    int max_count);

// Načíta históriu teplôt z OpenWeatherMap za zadaný časový úsek
int storage_get_weather_history(uint32_t since_timestamp, float* temp_array,
                                int max_count);

// Vráti celkovú veľkosť a využité miesto na partícii LittleFS
void storage_get_fs_info(size_t* total, size_t* used);