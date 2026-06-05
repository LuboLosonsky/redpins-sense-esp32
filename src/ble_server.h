#pragma once

#include <stddef.h>
#include <stdint.h>

// Spustí NimBLE stack, nastaví GAP/GATT a začne Advertising
void ble_server_init();

// Odošle telemetriu (A101) pripojenému zariadeniu ako NOTIFY
void ble_notify_telemetry(float t, float h, uint16_t p);

// Odošle chunk dát (CSV/JSON) cez DATA_STREAM (A104)
void ble_notify_datastream(const uint8_t* data, size_t length);

// Vráti true, ak je aktuálne pripojený nejaký BLE klient (Android)
bool ble_server_is_connected();