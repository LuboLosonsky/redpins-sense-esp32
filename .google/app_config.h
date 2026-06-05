#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdint.h>

// Maximálna dĺžka pre reťazce v konfigurácii
#define APP_CONFIG_MAX_STR_LEN 32

/**
 * @brief Štruktúra držiaca celú aplikačnú konfiguráciu.
 * Je to jediný zdroj pravdy (SSoT) pre nastavenia, ktoré sa ukladajú
 * do config.json na LittleFS.
 */
typedef struct {
    // Identita zariadenia
    char alias[APP_CONFIG_MAX_STR_LEN];

    // WiFi prihlasovacie údaje
    char wifi_ssid[APP_CONFIG_MAX_STR_LEN];
    char wifi_password[64];

    // Lokalita pre OpenWeatherMap API
    float lat;
    float lon;

    // Kalibračné ofsety pre senzory
    float dht_temp_offset;
    float bmp_press_offset;

    // Preferencie používateľa pre GUI
    int graph_days;         // Rozsah grafu (1, 3, 7 dní)
    int screen_interval_s;  // Rýchlosť preklápania obrazoviek (15, 30s)

} app_config_t;

/**
 * @brief Globálna inštancia konfigurácie, dostupná pre celý systém.
 * Po inicializácii cez app_config_init() obsahuje aktuálne hodnoty.
 */
extern app_config_t g_app_config;

/**
 * @brief Inicializuje konfiguračný manažér.
 * Načíta /data/config.json z LittleFS. Ak súbor neexistuje, vytvorí ho
 * s predvolenými hodnotami.
 */
void app_config_init();

/**
 * @brief Uloží aktuálny stav globálnej konfigurácie (g_app_config) do súboru
 * /data/config.json.
 */
void app_config_save();

#endif  // APP_CONFIG_H