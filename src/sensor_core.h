#pragma once

// Inicializuje I2C a senzory BME280 + BH1750.
void sensor_core_init();

// Vyčíta dáta z BME280 (teplota/vlhkost/tlak v hPa).
bool sensor_core_read_bme280(float* temperature, float* humidity,
                             float* pressure_hpa);

// Vyčíta osvetlenie z BH1750 (lux).
bool sensor_core_read_bh1750(float* lux);

// Naštartuje autonómne vlákno (Task), ktoré vyčítava dáta a loguje ich do
// pamäte
void sensor_core_start_task();

// Okamžite vráti posledné namerané hodnoty (pre BLE a Displej, neblokujúco)
void sensor_core_get_latest(float* t, float* h);

// Rozšírený snapshot lokálnych senzorov.
void sensor_core_get_latest_full(float* t, float* h, float* p_hpa, float* lux);