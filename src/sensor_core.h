#pragma once

// Inicializuje zbernicu pre DHT11 senzor
void sensor_core_init();

// Vyčíta dáta z DHT11 (vráti true pri úspechu a skopíruje teplotu/vlhkosť)
bool sensor_core_read_dht11(float* temperature, float* humidity);

// Naštartuje autonómne vlákno (Task), ktoré vyčítava dáta a loguje ich do
// pamäte
void sensor_core_start_task();

// Okamžite vráti posledné namerané hodnoty (pre BLE a Displej, neblokujúco)
void sensor_core_get_latest(float* t, float* h);