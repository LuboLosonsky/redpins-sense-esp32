#pragma once
#include "esp_err.h"

// Inicializuje LittleFS a pripojí ho na VFS cestu "/data"
esp_err_t storage_init();

// Nájde offset podľa timestampu a odpošle surové CSV dáta cez BLE
void storage_sync_sensors(uint32_t since_timestamp);

// Pripíše nový záznam na koniec histórie senzorov
void storage_log_sensor_data(uint32_t timestamp, float t, float h, float p);