#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char* TAG = "APP_CONFIG";
static const char* CONFIG_PATH = "/data/config.json";

// Globálna inštancia, inicializovaná na defaultné hodnoty
app_config_t g_app_config = {.alias = "Redpins Sense",
                             .wifi_ssid = "",
                             .wifi_password = "",
                             .lat = 48.1486,  // Default: Bratislava
                             .lon = 17.1077,
                             .dht_temp_offset = 0.0f,
                             .bmp_press_offset = 0.0f,
                             .graph_days = 1,
                             .screen_interval_s = 15};

static void create_default_config() {
    ESP_LOGW(TAG,
             "Súbor %s neexistuje alebo je poškodený. Vytváram novú "
             "konfiguráciu.",
             CONFIG_PATH);
    app_config_save();
}

void app_config_init() {
    FILE* f = fopen(CONFIG_PATH, "r");
    if (f == NULL) {
        create_default_config();
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* json_string = (char*)malloc(fsize + 1);
    fread(json_string, 1, fsize, f);
    fclose(f);
    json_string[fsize] = 0;

    cJSON* root = cJSON_Parse(json_string);
    if (root == NULL) {
        free(json_string);
        create_default_config();
        return;
    }

    cJSON* item;
    item = cJSON_GetObjectItem(root, "alias");
    if (cJSON_IsString(item))
        strncpy(g_app_config.alias, item->valuestring,
                sizeof(g_app_config.alias) - 1);

    item = cJSON_GetObjectItem(root, "wifi_ssid");
    if (cJSON_IsString(item))
        strncpy(g_app_config.wifi_ssid, item->valuestring,
                sizeof(g_app_config.wifi_ssid) - 1);

    item = cJSON_GetObjectItem(root, "wifi_password");
    if (cJSON_IsString(item))
        strncpy(g_app_config.wifi_password, item->valuestring,
                sizeof(g_app_config.wifi_password) - 1);

    item = cJSON_GetObjectItem(root, "lat");
    if (cJSON_IsNumber(item)) g_app_config.lat = item->valuedouble;

    item = cJSON_GetObjectItem(root, "lon");
    if (cJSON_IsNumber(item)) g_app_config.lon = item->valuedouble;

    item = cJSON_GetObjectItem(root, "dht_temp_offset");
    if (cJSON_IsNumber(item)) g_app_config.dht_temp_offset = item->valuedouble;

    item = cJSON_GetObjectItem(root, "bmp_press_offset");
    if (cJSON_IsNumber(item)) g_app_config.bmp_press_offset = item->valuedouble;

    item = cJSON_GetObjectItem(root, "graph_days");
    if (cJSON_IsNumber(item)) g_app_config.graph_days = item->valueint;

    item = cJSON_GetObjectItem(root, "screen_interval_s");
    if (cJSON_IsNumber(item)) g_app_config.screen_interval_s = item->valueint;

    cJSON_Delete(root);
    free(json_string);

    ESP_LOGI(TAG, "Konfigurácia úspešne načítaná z %s", CONFIG_PATH);
}

void app_config_save() {
    cJSON* root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "alias", g_app_config.alias);
    cJSON_AddStringToObject(root, "wifi_ssid", g_app_config.wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password", g_app_config.wifi_password);
    cJSON_AddNumberToObject(root, "lat", g_app_config.lat);
    cJSON_AddNumberToObject(root, "lon", g_app_config.lon);
    cJSON_AddNumberToObject(root, "dht_temp_offset",
                            g_app_config.dht_temp_offset);
    cJSON_AddNumberToObject(root, "bmp_press_offset",
                            g_app_config.bmp_press_offset);
    cJSON_AddNumberToObject(root, "graph_days", g_app_config.graph_days);
    cJSON_AddNumberToObject(root, "screen_interval_s",
                            g_app_config.screen_interval_s);

    char* json_string = cJSON_Print(root);

    FILE* f = fopen(CONFIG_PATH, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Chyba pri otváraní %s na zápis!", CONFIG_PATH);
    } else {
        fprintf(f, "%s", json_string);
        fclose(f);
        ESP_LOGI(TAG, "Konfigurácia uložená do %s", CONFIG_PATH);
    }

    cJSON_Delete(root);
    free(json_string);
}