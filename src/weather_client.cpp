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
#include "storage.h"
#include "wifi_scanner.h"

static const char* TAG = "WEATHER_API";

// Globálne premenné na udržanie posledného známeho počasia pre GUI
static float s_w_temp = 0.0f;
static int s_w_hum = 0;
static int s_w_press = 0;
static int s_w_id = 0;
static char s_w_city[32] = "Nezname";
static int s_w_aqi = 0;
static float s_w_pm25 = 0.0f;
static bool s_w_valid = false;

bool weather_get_latest(float* temp, int* hum, int* press, int* icon_id) {
    if (!s_w_valid) return false;
    *temp = s_w_temp;
    *hum = s_w_hum;
    *press = s_w_press;
    *icon_id = s_w_id;
    return true;
}

bool weather_get_aqi(int* aqi, float* pm25) {
    if (!s_w_valid || s_w_aqi == 0) return false;
    *aqi = s_w_aqi;
    *pm25 = s_w_pm25;
    return true;
}

void weather_get_city(char* city_buf, size_t max_len) {
    if (city_buf && max_len > 0) {
        strncpy(city_buf, s_w_city, max_len - 1);
        city_buf[max_len - 1] = '\0';
    }
}

// Zanshin: Načíta poslednú známu hodnotu z disku, aby GUI po štarte neukazovalo
// API Error
static void weather_load_cache() {
    FILE* f = fopen(FILE_WEATHER_CSV, "r");
    if (!f) return;

    char line_buf[128];
    char last_line[128] = "";

    // Rýchly prechod na posledný riadok súboru
    while (fgets(line_buf, sizeof(line_buf), f)) {
        if (strlen(line_buf) > 10) {
            strncpy(last_line, line_buf, sizeof(last_line));
        }
    }
    fclose(f);

    if (strlen(last_line) > 0) {
        char* p1 = strchr(last_line, ';');
        if (p1) {
            char* p2 = strchr(p1 + 1, ';');
            if (p2) {
                char* p3 = strchr(p2 + 1, ';');
                if (p3) {
                    char* p4 = strchr(p3 + 1, ';');
                    if (p4) {
                        int len = p2 - (p1 + 1);
                        if (len > 0 && len < sizeof(s_w_city)) {
                            strncpy(s_w_city, p1 + 1, len);
                            s_w_city[len] = '\0';
                        } else {
                            strcpy(s_w_city, "Nezname");
                        }
                        s_w_temp = atof(p2 + 1);
                        s_w_hum = atoi(p3 + 1);
                        s_w_press = atoi(p4 + 1);
                        s_w_id = 0;  // Default fallback (po štarte ukáže oblak,
                                     // kým nepríde API update)
                        s_w_valid = true;
                        ESP_LOGI(
                            TAG,
                            "Načítaná cache z disku: %s, %.1f°C, %d%%, %dhPa",
                            s_w_city, s_w_temp, s_w_hum, s_w_press);
                    }
                }
            }
        }
    }
}

// Pomocná funkcia na odstránenie slovenskej/českej diakritiky (UTF-8 -> ASCII)
// Zanshin: In-place modifikácia, žiadna alokácia na Heape
static void remove_diacritics(char* str) {
    if (!str) return;
    char* read = str;
    char* write = str;

    while (*read) {
        unsigned char c = (unsigned char)*read;
        if (c < 128) {  // Štandardné ASCII (A-Z, a-z)
            *write++ = *read++;
        } else if (c == 0xC3) {  // 2-bajtová UTF-8 sekvencia
            read++;
            unsigned char c2 = (unsigned char)*read;
            if (!c2) break;
            switch (c2) {
                case 0xA1:
                case 0xA4:
                    *write++ = 'a';
                    break;  // á, ä
                case 0x81:
                case 0x84:
                    *write++ = 'A';
                    break;  // Á, Ä
                case 0xA9:
                    *write++ = 'e';
                    break;  // é
                case 0x89:
                    *write++ = 'E';
                    break;  // É
                case 0xAD:
                    *write++ = 'i';
                    break;  // í
                case 0x8D:
                    *write++ = 'I';
                    break;  // Í
                case 0xB3:
                case 0xB4:
                    *write++ = 'o';
                    break;  // ó, ô
                case 0x93:
                case 0x94:
                    *write++ = 'O';
                    break;  // Ó, Ô
                case 0xBA:
                    *write++ = 'u';
                    break;  // ú
                case 0x9A:
                    *write++ = 'U';
                    break;  // Ú
                case 0xBD:
                    *write++ = 'y';
                    break;  // ý
                case 0x9D:
                    *write++ = 'Y';
                    break;  // Ý
                default:
                    *write++ = '?';
            }
            read++;
        } else if (c == 0xC4) {
            read++;
            unsigned char c2 = (unsigned char)*read;
            if (!c2) break;
            switch (c2) {
                case 0x8D:
                    *write++ = 'c';
                    break;  // č
                case 0x8C:
                    *write++ = 'C';
                    break;  // Č
                case 0x8F:
                    *write++ = 'd';
                    break;  // ď
                case 0x8E:
                    *write++ = 'D';
                    break;  // Ď
                case 0xBE:
                case 0xBA:
                    *write++ = 'l';
                    break;  // ľ, ĺ
                case 0xBD:
                case 0xB9:
                    *write++ = 'L';
                    break;  // Ľ, Ĺ
                default:
                    *write++ = '?';
            }
            read++;
        } else if (c == 0xC5) {
            read++;
            unsigned char c2 = (unsigned char)*read;
            if (!c2) break;
            switch (c2) {
                case 0x88:
                    *write++ = 'n';
                    break;  // ň
                case 0x87:
                    *write++ = 'N';
                    break;  // Ň
                case 0x95:
                    *write++ = 'r';
                    break;  // ŕ
                case 0x94:
                    *write++ = 'R';
                    break;  // Ŕ
                case 0xA1:
                    *write++ = 's';
                    break;  // š
                case 0xA0:
                    *write++ = 'S';
                    break;  // Š
                case 0xA5:
                    *write++ = 't';
                    break;  // ť
                case 0xA4:
                    *write++ = 'T';
                    break;  // Ť
                case 0xBE:
                    *write++ = 'z';
                    break;  // ž
                case 0xBD:
                    *write++ = 'Z';
                    break;  // Ž
                default:
                    *write++ = '?';
            }
            read++;
        } else {
            // Bezpečné preskočenie neznámych viacbajtových znakov
            if ((c & 0xE0) == 0xC0)
                read += 2;
            else if ((c & 0xF0) == 0xE0)
                read += 3;
            else if ((c & 0xF8) == 0xF0)
                read += 4;
            else
                read++;
            *write++ = '?';
        }
    }
    *write = '\0';  // Vždy korektne uzavrieme reťazec
}

static void weather_fetch_task(void* arg) {
    ESP_LOGI(TAG, "Weather Task spustený (Interval: 20 min / 3x za hodinu)");

    int last_fetch_min = -1;

    while (1) {
        time_t now;
        time(&now);

        // Bežíme len ak je pripojená Wi-Fi a čas je zosynchronizovaný s NTP
        // (>2020)
        if (wifi_scanner_is_connected() && now > 1600000000) {
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);

            // Striktné plánovanie na minúty (00, 20, 40), aby sme pri
            // reštartoch neplytvali limitmi
            if (last_fetch_min == -1 || (timeinfo.tm_min % 20 == 0 &&
                                         timeinfo.tm_min != last_fetch_min)) {
                last_fetch_min = timeinfo.tm_min;
                ESP_LOGI(TAG,
                         "Sťahujem OpenWeatherMap (Plánovač: %02d:%02d)...",
                         timeinfo.tm_hour, timeinfo.tm_min);

                app_config_t* cfg = app_config_get();
                char url[256];

                // Dynamické URL podľa GPS a kľúča z config.json
                snprintf(url, sizeof(url),
                         "http://api.openweathermap.org/data/2.5/"
                         "weather?lat=%.4f&lon=%.4f&appid=%s&units=metric",
                         cfg->lat, cfg->lon, cfg->weather_api_key);

                ESP_LOGI(TAG, "Pripravené API URL: %s", url);

                esp_http_client_config_t config = {};
                config.url = url;
                config.method = HTTP_METHOD_GET;
                config.timeout_ms = 10000;

                esp_http_client_handle_t client = esp_http_client_init(&config);
                esp_err_t err = esp_http_client_open(client, 0);

                if (err == ESP_OK) {
                    esp_http_client_fetch_headers(client);

                    // Zanshin: Odpoveď z OWM s presnými GPS súradnicami je
                    // výrazne väčšia! Bezpečne alokujeme 3KB buffer na Heape,
                    // aby nedošlo k pretečeniu Stacku.
                    int buf_size = 3072;
                    char* buffer = (char*)malloc(buf_size);

                    if (buffer) {
                        int total_read = 0;
                        int read_len;

                        while ((read_len = esp_http_client_read(
                                    client, buffer + total_read,
                                    buf_size - total_read - 1)) > 0) {
                            total_read += read_len;
                            if (total_read >= buf_size - 1) {
                                ESP_LOGW(TAG,
                                         "Varovanie: Odpoveď API je príliš "
                                         "dlhá, môže dôjsť k orezaniu!");
                                break;
                            }
                        }
                        buffer[total_read] =
                            '\0';  // Bezpečné ukončenie C-stringu
                        ESP_LOGI(TAG, "Prijatých %d bajtov z OWM.", total_read);

                        if (total_read > 0) {
                            int status_code =
                                esp_http_client_get_status_code(client);
                            ESP_LOGI(TAG,
                                     "HTTP Status: %d. Surová odpoveď API:\n%s",
                                     status_code, buffer);

                            cJSON* root = cJSON_Parse(buffer);
                            if (root) {
                                cJSON* main_obj =
                                    cJSON_GetObjectItem(root, "main");
                                cJSON* weather_arr =
                                    cJSON_GetObjectItem(root, "weather");

                                if (main_obj) {
                                    float temp =
                                        cJSON_GetObjectItem(main_obj, "temp")
                                            ->valuedouble;
                                    int hum = cJSON_GetObjectItem(main_obj,
                                                                  "humidity")
                                                  ->valueint;
                                    int press = cJSON_GetObjectItem(main_obj,
                                                                    "pressure")
                                                    ->valueint;

                                    int weather_id =
                                        800;  // Predvolené Clear Sky
                                    if (cJSON_IsArray(weather_arr)) {
                                        cJSON* w_item =
                                            cJSON_GetArrayItem(weather_arr, 0);
                                        if (w_item) {
                                            cJSON* id_item =
                                                cJSON_GetObjectItem(w_item,
                                                                    "id");
                                            if (cJSON_IsNumber(id_item))
                                                weather_id = id_item->valueint;
                                        }
                                    }

                                    ESP_LOGI(TAG,
                                             "OWM Úspech: %.1f°C, %d%%, %dhPa, "
                                             "IconID: %d",
                                             temp, hum, press, weather_id);

                                    cJSON* name_item =
                                        cJSON_GetObjectItem(root, "name");
                                    if (cJSON_IsString(name_item) &&
                                        name_item->valuestring != NULL) {
                                        strncpy(s_w_city,
                                                name_item->valuestring,
                                                sizeof(s_w_city) - 1);
                                        s_w_city[sizeof(s_w_city) - 1] = '\0';
                                        remove_diacritics(
                                            s_w_city);  // Zanshin: Odstránenie
                                                        // diakritiky (napr.
                                                        // Žilina -> Zilina)
                                    }

                                    storage_log_weather_data((unsigned long)now,
                                                             s_w_city, temp,
                                                             hum, press);

                                    // Uloženie do RAM pre zobrazenie v GUI
                                    // Tasku
                                    s_w_temp = temp;
                                    s_w_hum = hum;
                                    s_w_press = press;
                                    s_w_id = weather_id;
                                    s_w_valid = true;
                                } else {
                                    ESP_LOGE(TAG,
                                             "Chyba: JSON neobsahuje 'main'. "
                                             "API zrejme vrátilo chybu.");
                                    cJSON* msg =
                                        cJSON_GetObjectItem(root, "message");
                                    if (msg && cJSON_IsString(msg)) {
                                        ESP_LOGE(TAG, "OWM Dôvod chyby: %s",
                                                 msg->valuestring);
                                    }
                                }
                                cJSON_Delete(root);
                            } else {
                                ESP_LOGE(TAG,
                                         "Chyba parsovania JSON odpovede! JSON "
                                         "bol pravdepodobne ustrihnutý.");
                            }
                        }

                        // --- ZANSHIN: Sťahovanie kvality ovzdušia (AQI) ---
                        // Znovupoužívame už alokovaný 3KB buffer, aby sme
                        // nezaťažovali Heap!
                        if (cfg->lat != 0.0f || cfg->lon != 0.0f) {
                            snprintf(url, sizeof(url),
                                     "http://api.openweathermap.org/data/2.5/"
                                     "air_pollution?lat=%.4f&lon=%.4f&appid="
                                     "%s",
                                     cfg->lat, cfg->lon, cfg->weather_api_key);

                            esp_http_client_config_t aqi_config = {};
                            aqi_config.url = url;
                            aqi_config.method = HTTP_METHOD_GET;
                            aqi_config.timeout_ms = 5000;

                            esp_http_client_handle_t aqi_client =
                                esp_http_client_init(&aqi_config);
                            if (esp_http_client_open(aqi_client, 0) == ESP_OK) {
                                esp_http_client_fetch_headers(aqi_client);
                                int aqi_read = 0, r_len;
                                while ((r_len = esp_http_client_read(
                                            aqi_client, buffer + aqi_read,
                                            buf_size - aqi_read - 1)) > 0) {
                                    aqi_read += r_len;
                                    if (aqi_read >= buf_size - 1) break;
                                }
                                buffer[aqi_read] = '\0';

                                if (aqi_read > 0) {
                                    cJSON* aqi_root = cJSON_Parse(buffer);
                                    if (aqi_root) {
                                        cJSON* list = cJSON_GetObjectItem(
                                            aqi_root, "list");
                                        if (cJSON_IsArray(list)) {
                                            cJSON* item =
                                                cJSON_GetArrayItem(list, 0);
                                            if (item) {
                                                cJSON* main_obj =
                                                    cJSON_GetObjectItem(item,
                                                                        "main");
                                                if (main_obj) {
                                                    cJSON* aqi_val =
                                                        cJSON_GetObjectItem(
                                                            main_obj, "aqi");
                                                    if (cJSON_IsNumber(aqi_val))
                                                        s_w_aqi =
                                                            aqi_val->valueint;
                                                }
                                                cJSON* comps =
                                                    cJSON_GetObjectItem(
                                                        item, "components");
                                                if (comps) {
                                                    cJSON* pm25_val =
                                                        cJSON_GetObjectItem(
                                                            comps, "pm2_5");
                                                    if (cJSON_IsNumber(
                                                            pm25_val))
                                                        s_w_pm25 =
                                                            pm25_val
                                                                ->valuedouble;
                                                }
                                            }
                                        }
                                        cJSON_Delete(aqi_root);
                                    }
                                }
                            }
                            esp_http_client_cleanup(aqi_client);
                        }
                        // --------------------------------------------------

                        free(buffer);  // Uvoľnenie veľkého buffra z pamäte
                    } else {
                        ESP_LOGE(TAG, "Nedostatok voľnej RAM pre HTTP buffer!");
                    }
                } else {
                    ESP_LOGE(TAG, "Chyba pripojenia k OpenWeatherMap API");
                }
                esp_http_client_cleanup(client);
            }
        }

        // Slučka sa otočí raz za 5 sekúnd - zbytočne neblokuje procesor a
        // presne zachytí prechod minúty
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void weather_client_init(void) {
    weather_load_cache();  // Okamžité načítanie pre HMI displej
    // Štart Tasku - priorita 3 (Dátové prenosy sú pod displejom a nad senzormi)
    xTaskCreate(weather_fetch_task, "weather_task", 4096, NULL, 3, NULL);
}

void weather_client_load_last_known(void) { weather_load_cache(); }