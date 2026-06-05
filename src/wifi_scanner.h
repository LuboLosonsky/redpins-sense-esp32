#pragma once
#include <stddef.h>

// Spustí natívne WiFi skenovanie a odošle výsledky ako JSON do BLE DATA_STREAM
void wifi_scanner_scan_and_stream();

// Uloží údaje do NVS, nastaví Wi-Fi a spustí pripájanie
void wifi_scanner_connect(const char* ssid, const char* pwd);

// Vráti true, ak je zariadenie úspešne pripojené na Wi-Fi
bool wifi_scanner_is_connected();

// Vráti silu signálu (RSSI) v dBm. Ak nie je pripojené, vráti -100.
int wifi_scanner_get_rssi();

// Zapíše názov aktuálnej Wi-Fi siete (SSID) do buffra
void wifi_scanner_get_ssid(char* outBuffer, size_t maxLength);

// Zapíše aktuálnu IP adresu do buffra (ak nie je pripojené, vráti prázdny
// string)
void wifi_scanner_get_ip(char* outBuffer, size_t maxLength);

// Skúsi načítať uložené údaje z NVS a automaticky sa pripojiť (volané po
// štarte)
void wifi_scanner_auto_connect();