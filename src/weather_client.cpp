#include "weather_client.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_scanner.h"

static const char* TAG = "WEATHER_API";

// Globálne premenné na udržanie posledného známeho počasia pre GUI
static float s_w_temp = 0.0f;
static int s_w_hum = 0;
static int s_w_press = 0;
static int s_w_id = 0;
static bool s_w_valid = false;

bool weather_get_latest(float* temp, int* hum, int* press, int* icon_id) {
    if (!s_w_valid) return false;
    *temp = s_w_temp;
    *hum = s_w_hum;
    *press = s_w_press;
    *icon_id = s_w_id;
    return true;
}

static void weather_fetch_task(void* arg) {
    ESP_LOGI(TAG, "Weather Task spustený (Interval: 30 min)");

    while (1) {
        time_t now;
        time(&now);

        // Bežíme len ak je pripojená Wi-Fi a čas je zosynchronizovaný s NTP
        // (>2020)
        if (wifi_scanner_is_connected() && now > 1600000000) {
            ESP_LOGI(TAG, "Sťahujem aktuálne počasie z OpenWeatherMap...");

            app_config_t* cfg = app_config_get();
            char url[256];

            // Dynamické URL podľa GPS alebo fallback na hardcoded API z
            // hlavičky
            if (cfg->lat != 0.0f || cfg->lon != 0.0f) {
                snprintf(url, sizeof(url),
                         "http://api.openweathermap.org/data/2.5/"
                         "weather?lat=%.4f&lon=%.4f&appid="
                         "bf45f6a7032650a46b26e01d879cb436&units=metric",
                         cfg->lat, cfg->lon);
            } else {
                snprintf(url, sizeof(url), "%s", WEATHER_API_URL);
            }

            esp_http_client_config_t config = {};
            config.url = url;
            config.method = HTTP_METHOD_GET;
            config.timeout_ms = 10000;

            esp_http_client_handle_t client = esp_http_client_init(&config);
            esp_err_t err = esp_http_client_open(client, 0);

            if (err == ESP_OK) {
                esp_http_client_fetch_headers(client);

                char buffer[1024];
                int total_read = 0;
                int read_len;

                // Čítanie odpovede po chunkoch priamo do buffra (šetrenie
                // pamäte)
                while ((read_len = esp_http_client_read(
                            client, buffer + total_read,
                            sizeof(buffer) - total_read - 1)) > 0) {
                    total_read += read_len;
                }
                buffer[total_read] = '\0';  // Bezpečné ukončenie C-stringu

                if (total_read > 0) {
                    int status_code = esp_http_client_get_status_code(client);
                    ESP_LOGI(TAG, "HTTP Status: %d. Surová odpoveď API:\n%s",
                             status_code, buffer);

                    cJSON* root = cJSON_Parse(buffer);
                    if (root) {
                        cJSON* main_obj = cJSON_GetObjectItem(root, "main");
                        cJSON* weather_arr =
                            cJSON_GetObjectItem(root, "weather");

                        if (main_obj) {
                            float temp = cJSON_GetObjectItem(main_obj, "temp")
                                             ->valuedouble;
                            int hum = cJSON_GetObjectItem(main_obj, "humidity")
                                          ->valueint;
                            int press =
                                cJSON_GetObjectItem(main_obj, "pressure")
                                    ->valueint;

                            int weather_id = 800;  // Predvolené Clear Sky
                            if (cJSON_IsArray(weather_arr)) {
                                cJSON* w_item =
                                    cJSON_GetArrayItem(weather_arr, 0);
                                if (w_item) {
                                    cJSON* id_item =
                                        cJSON_GetObjectItem(w_item, "id");
                                    if (cJSON_IsNumber(id_item))
                                        weather_id = id_item->valueint;
                                }
                            }

                            ESP_LOGI(
                                TAG,
                                "OWM Úspech: %.1f°C, %d%%, %dhPa, IconID: %d",
                                temp, hum, press, weather_id);

                            FILE* f = fopen(FILE_WEATHER_CSV, "a");
                            if (f) {
                                fprintf(f, "%lu;%.1f;%d;%d;%d\n",
                                        (unsigned long)now, temp, hum, press,
                                        weather_id);
                                fclose(f);
                            }

                            // Uloženie do RAM pre zobrazenie v GUI Tasku
                            s_w_temp = temp;
                            s_w_hum = hum;
                            s_w_press = press;
                            s_w_id = weather_id;
                            s_w_valid = true;
                        }
                        cJSON_Delete(root);
                    } else {
                        ESP_LOGE(TAG, "Chyba parsovania JSON odpovede!");
                    }
                }
            } else {
                ESP_LOGE(TAG, "Chyba pripojenia k OpenWeatherMap API");
            }
            esp_http_client_cleanup(client);

            // Úspech alebo zlyhanie API - ďalší pokus o 30 minút
            vTaskDelay(pdMS_TO_TICKS(WEATHER_FETCH_INTERVAL_MS));
        } else {
            // Čakáme na Wi-Fi alebo SNTP čas, skúsime znova o 2 sekundy
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
}

void weather_client_init(void) {
    // Štart Tasku - priorita 3 (Dátové prenosy sú pod displejom a nad senzormi)
    xTaskCreate(weather_fetch_task, "weather_task", 4096, NULL, 3, NULL);
}