#include "app_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

static const char* TAG = "APP_CONFIG";

// Zanshin: Predvolené hodnoty (Žilina, Slovensko ako Single Source of Truth)
static app_config_t s_config = {.lat = 49.2231f,
                                .lon = 18.7394f,
                                .alias = "Redpins Sense",
                                .dht_temp_offset = 0.0f,
                                .bmp_temp_offset = 0.0f};

app_config_t* app_config_get(void) { return &s_config; }

void app_config_init(void) {
    ESP_LOGI(TAG, "Načítavam konfiguráciu z %s", FILE_APP_CONFIG);

    FILE* f = fopen(FILE_APP_CONFIG, "r");
    if (!f) {
        ESP_LOGW(TAG,
                 "Súbor neexistuje, použijú sa predvolené hodnoty a vytvorí sa "
                 "nový súbor.");
        app_config_save();
        return;
    }

    // Prečítame obsah súboru
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return;
    }

    char* buffer = (char*)malloc(size + 1);
    if (!buffer) {
        ESP_LOGE(TAG, "Nedostatok pamäte pre načítanie configu!");
        fclose(f);
        return;
    }

    fread(buffer, 1, size, f);
    buffer[size] = '\0';
    fclose(f);

    // ESP-IDF natívny JSON parser
    cJSON* json = cJSON_Parse(buffer);
    free(buffer);

    if (json) {
        cJSON* lat = cJSON_GetObjectItem(json, "lat");
        if (cJSON_IsNumber(lat)) s_config.lat = lat->valuedouble;

        cJSON* lon = cJSON_GetObjectItem(json, "lon");
        if (cJSON_IsNumber(lon)) s_config.lon = lon->valuedouble;

        cJSON* alias = cJSON_GetObjectItem(json, "alias");
        if (cJSON_IsString(alias) && alias->valuestring != NULL) {
            strncpy(s_config.alias, alias->valuestring,
                    sizeof(s_config.alias) - 1);
            s_config.alias[sizeof(s_config.alias) - 1] = '\0';
        }

        cJSON* dht_off = cJSON_GetObjectItem(json, "dht_off");
        if (cJSON_IsNumber(dht_off))
            s_config.dht_temp_offset = dht_off->valuedouble;

        cJSON* bmp_off = cJSON_GetObjectItem(json, "bmp_off");
        if (cJSON_IsNumber(bmp_off))
            s_config.bmp_temp_offset = bmp_off->valuedouble;

        cJSON_Delete(json);

        // Zanshin: Auto-migrácia, ak bol na disku starý config s nulovými
        // súradnicami
        if (s_config.lat == 0.0f && s_config.lon == 0.0f) {
            s_config.lat = 49.2231f;
            s_config.lon = 18.7394f;
            ESP_LOGI(TAG,
                     "Nulové súradnice detegované. Auto-migrujem na predvolenú "
                     "lokalitu (Žilina).");
            app_config_save();
        }

        ESP_LOGI(TAG, "Konfigurácia úspešne načítaná (Alias: %s)",
                 s_config.alias);
    } else {
        ESP_LOGE(TAG, "Chyba pri parsovaní config.json!");
    }
}

void app_config_save(void) {
    cJSON* json = cJSON_CreateObject();
    if (!json) return;

    // Konverzia do JSON štruktúry
    cJSON_AddNumberToObject(json, "lat", s_config.lat);
    cJSON_AddNumberToObject(json, "lon", s_config.lon);
    cJSON_AddStringToObject(json, "alias", s_config.alias);
    cJSON_AddNumberToObject(json, "dht_off", s_config.dht_temp_offset);
    cJSON_AddNumberToObject(json, "bmp_off", s_config.bmp_temp_offset);

    // Bezpečné uloženie bez zbytočného formátovania (šetríme miesto v LittleFS)
    char* json_str = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);

    if (json_str) {
        FILE* f = fopen(FILE_APP_CONFIG, "w");
        if (f) {
            fprintf(f, "%s", json_str);
            fclose(f);
            ESP_LOGI(TAG, "Konfigurácia uložená na disk: %s", json_str);
        } else {
            ESP_LOGE(TAG, "Chyba pri zápise do %s", FILE_APP_CONFIG);
        }
        free(json_str);  // Uvoľníme buffer vytvorený cJSON_Print...
    }
}