#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ble_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "STORAGE";

// --- Zanshin: Stream Collision Guard pre RCP V2.3 ---
volatile bool g_storage_abort_stream = false;

void storage_abort_stream() { g_storage_abort_stream = true; }

// --- Zanshin: O(1) RAM cache pre barometrický trend ---
#define TREND_MAX_SAMPLES 18  // 3 hodiny (pri 10-minútovom intervale)
static int s_trend_buffer[TREND_MAX_SAMPLES];
static int s_trend_idx = 0;
static int s_trend_count = 0;

// Pocet zaznamov v CSV (bez hlavicky) - pocitane raz pri starte, potom
// inkrementalne aktualizovane pri kazdom zapise/rotacii. Zobrazuje sa na
// obrazovke SYSTEM, aby bolo vidiet ci lokalne aj weather logovanie realne
// bezi.
static int s_sensor_record_count = 0;
static int s_weather_record_count = 0;

static int count_csv_records(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;

    char line_buf[64];
    int count = 0;
    bool skipped_header = false;
    while (fgets(line_buf, sizeof(line_buf), f)) {
        if (!skipped_header) {
            skipped_header = true;
            continue;
        }
        if (line_buf[0] != '\0' && line_buf[0] != '\n') count++;
    }
    fclose(f);
    return count;
}

static void prefill_trend_buffer() {
    FILE* f = fopen("/data/sensor.csv", "r");
    if (!f) return;

    char line_buf[64];
    while (fgets(line_buf, sizeof(line_buf), f)) {
        uint32_t ts = strtoul(line_buf, NULL, 10);
        if (ts == 0) continue;  // Preskoč hlavičku

        // Rýchly posun v štruktúre: timestamp;temp;hum;press
        char* p1 = strchr(line_buf, ';');
        if (p1) {
            char* p2 = strchr(p1 + 1, ';');
            if (p2) {
                char* p3 = strchr(p2 + 1, ';');
                if (p3) {
                    // Zápis do kruhového buffra
                    s_trend_buffer[s_trend_idx] = atoi(p3 + 1);
                    s_trend_idx = (s_trend_idx + 1) % TREND_MAX_SAMPLES;
                    if (s_trend_count < TREND_MAX_SAMPLES) s_trend_count++;
                }
            }
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "Trend buffer naplnený z histórie. Vzorky: %d",
             s_trend_count);
}

esp_err_t storage_init() {
    ESP_LOGI(TAG, "Inicializujem LittleFS...");

    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/data";
    conf.partition_label = "storage";
    conf.format_if_mount_failed = true;
    conf.dont_mount = false;

    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Zlyhalo pripojenie LittleFS. Error: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(conf.partition_label, &total, &used);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Nemožno získať info o partícii (%s)",
                 esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "LittleFS pripojené. Veľkosť: %d B, Využité: %d B", total,
                 used);

        // Zanshin: Ak súbor neexistuje, vygenerujeme si dummy históriu na
        // testovanie.
        FILE* f = fopen("/data/sensor.csv", "r");
        if (!f) {
            ESP_LOGI(TAG,
                     "Súbor neexistuje, generujem testovací sensor.csv s "
                     "hlavičkou...");
            f = fopen("/data/sensor.csv", "w");
            if (f) {
                fprintf(f, "timestamp;temp;hum;press\n");  // Povinná hlavička
                fprintf(f, "1714500000;22.5;45.2;1013\n");
                fprintf(f, "1714503600;22.7;44.8;1012\n");
                fprintf(f, "1714507200;23.1;44.1;1011\n");
                fprintf(f, "1714510800;23.5;43.9;1010\n");
                fclose(f);
            }
        } else {
            fclose(f);
        }

        // Zanshin: Ak neexistuje počasie, vygenerujeme dummy históriu pre
        // parser v Androide
        // Auto-migrácia: Skontrolujeme hlavičku pre RCP v2.3
        FILE* fw = fopen("/data/weather.csv", "r");
        bool recreate_weather = true;
        if (fw) {
            char header[64];
            if (fgets(header, sizeof(header), fw)) {
                if (strncmp(header, "timestamp;city", 14) == 0)
                    recreate_weather = false;
            }
            fclose(fw);
        }
        if (recreate_weather) {
            ESP_LOGI(TAG,
                     "Reštrukturalizácia weather.csv (RCP V2.3) pre zladenie s "
                     "Androidom...");
            fw = fopen("/data/weather.csv", "w");
            if (fw) {
                fprintf(fw, "timestamp;city;temp;hum;press\n");
                fprintf(fw, "1714500000;Zilina;15.5;60;1013\n");
                fclose(fw);
            }
        }

        s_sensor_record_count = count_csv_records("/data/sensor.csv");
        s_weather_record_count = count_csv_records("/data/weather.csv");
        ESP_LOGI(TAG, "Zaznamy pri starte: sensor=%d weather=%d",
                 s_sensor_record_count, s_weather_record_count);
    }

    prefill_trend_buffer();

    return ESP_OK;
}

void storage_sync_sensors(uint32_t since_timestamp) {
    ESP_LOGI(TAG, "Spúšťam Delta Sync od timestampu: %lu", since_timestamp);
    g_storage_abort_stream = false;

    // NTP Guard (Ak je epoch time menší ako september 2020)
    time_t now_ts;
    time(&now_ts);
    if (now_ts < 1600000000) {
        ESP_LOGW(TAG, "NTP nesynchronizované. Blokujem dump SENS (RCP v2.3).");
        uint8_t env[5] = {0xA3, 0, 0, 0, 0};
        ble_notify_datastream(env, 5);
        return;
    }

    FILE* f = fopen("/data/sensor.csv", "r");
    if (!f) {
        ESP_LOGE(TAG, "Súbor sensor.csv neexistuje!");
        uint8_t env[5] = {0xA3, 0, 0, 0, 0};
        ble_notify_datastream(env, 5);
        return;
    }

    long target_offset = 0;
    char line_buf[64];

    // 1. Preskočíme lokálnu hlavičku, aby sme mohli vložiť vlastnú (Dynamic
    // Header)
    if (fgets(line_buf, sizeof(line_buf), f)) {
        target_offset = ftell(f);
        // 2. Hľadáme prvý záznam novší ako since_timestamp
        while (fgets(line_buf, sizeof(line_buf), f)) {
            uint32_t ts = strtoul(line_buf, NULL, 10);
            if (ts > since_timestamp) break;
            target_offset = ftell(f);
        }
    }

    // Predvýpočet veľkosti pre obálku (Binary Framing)
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, target_offset, SEEK_SET);

    const char* dyn_header = "timestamp;temp;hum;press\n";
    uint32_t data_size =
        (file_size > target_offset) ? (file_size - target_offset) : 0;
    uint32_t payload_size = strlen(dyn_header) + data_size;

    // Odoslanie RCP V2.3 Obálky
    uint8_t env[5];
    env[0] = 0xA3;  // Mode SENSORS
    env[1] = (payload_size >> 0) & 0xFF;
    env[2] = (payload_size >> 8) & 0xFF;
    env[3] = (payload_size >> 16) & 0xFF;
    env[4] = (payload_size >> 24) & 0xFF;
    ble_notify_datastream(env, 5);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Odoslanie Dynamic Headeru
    size_t h_len = strlen(dyn_header);
    size_t h_sent = 0;
    while (h_sent < h_len && !g_storage_abort_stream) {
        size_t to_send = (h_len - h_sent > 20) ? 20 : (h_len - h_sent);
        ble_notify_datastream((uint8_t*)dyn_header + h_sent, to_send);
        h_sent += to_send;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Zero-Copy stream surových dát
    uint8_t chunk[20];
    while (!g_storage_abort_stream) {
        size_t bytes_read = fread(chunk, 1, 20, f);
        if (bytes_read > 0) {
            ble_notify_datastream(chunk, bytes_read);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (bytes_read < 20) break;
    }

    fclose(f);
    ESP_LOGI(TAG, "Delta Sync ukončený.");
}

void storage_sync_weather(uint32_t since_timestamp) {
    ESP_LOGI(TAG, "Spúšťam Delta Sync (Počasie) od timestampu: %lu",
             since_timestamp);
    g_storage_abort_stream = false;

    time_t now_ts;
    time(&now_ts);
    if (now_ts < 1600000000) {
        ESP_LOGW(TAG,
                 "NTP nesynchronizované. Blokujem dump WEATHER (RCP v2.3).");
        uint8_t env[5] = {0xA4, 0, 0, 0, 0};
        ble_notify_datastream(env, 5);
        return;
    }

    FILE* f = fopen("/data/weather.csv", "r");
    if (!f) {
        ESP_LOGE(TAG, "Súbor weather.csv neexistuje!");
        uint8_t env[5] = {0xA4, 0, 0, 0, 0};
        ble_notify_datastream(env, 5);
        return;
    }

    long target_offset = 0;
    char line_buf[64];

    if (fgets(line_buf, sizeof(line_buf), f)) {
        target_offset = ftell(f);
        while (fgets(line_buf, sizeof(line_buf), f)) {
            uint32_t ts = strtoul(line_buf, NULL, 10);
            if (ts > since_timestamp) break;
            target_offset = ftell(f);
        }
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, target_offset, SEEK_SET);

    const char* dyn_header = "timestamp;city;temp;hum;press\n";
    uint32_t data_size =
        (file_size > target_offset) ? (file_size - target_offset) : 0;
    uint32_t payload_size = strlen(dyn_header) + data_size;

    uint8_t env[5];
    env[0] = 0xA4;  // Mode WEATHER
    env[1] = (payload_size >> 0) & 0xFF;
    env[2] = (payload_size >> 8) & 0xFF;
    env[3] = (payload_size >> 16) & 0xFF;
    env[4] = (payload_size >> 24) & 0xFF;
    ble_notify_datastream(env, 5);
    vTaskDelay(pdMS_TO_TICKS(20));

    size_t h_len = strlen(dyn_header);
    size_t h_sent = 0;
    while (h_sent < h_len && !g_storage_abort_stream) {
        size_t to_send = (h_len - h_sent > 20) ? 20 : (h_len - h_sent);
        ble_notify_datastream((uint8_t*)dyn_header + h_sent, to_send);
        h_sent += to_send;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint8_t chunk[20];
    while (!g_storage_abort_stream) {
        size_t bytes_read = fread(chunk, 1, 20, f);
        if (bytes_read > 0) {
            ble_notify_datastream(chunk, bytes_read);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (bytes_read < 20) break;
    }

    fclose(f);
    ESP_LOGI(TAG, "Delta Sync (Počasie) ukončený.");
}

// --- ZANSHIN: Inteligentná rotácia logov pri zaplnení disku ---
static void storage_rotate_log_if_needed(const char* filename) {
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);

    // Spustíme rotáciu, ak je voľné miesto < 10%
    if (total > 0 && (total - used) < (total * 0.1f)) {
        ESP_LOGW(TAG, "Málo miesta na disku! Spúšťam rotáciu pre %s", filename);

        char temp_filename[32];
        snprintf(temp_filename, sizeof(temp_filename), "%s.tmp", filename);

        FILE* f_in = fopen(filename, "r");
        FILE* f_out = fopen(temp_filename, "w");

        if (!f_in || !f_out) {
            if (f_in) fclose(f_in);
            if (f_out) fclose(f_out);
            ESP_LOGE(TAG, "Chyba otvorenia súborov pre rotáciu!");
            return;
        }

        // 1. Spočítame riadky a preskočíme hlavičku
        char line_buf[128];
        int total_lines = 0;
        fgets(line_buf, sizeof(line_buf), f_in);  // Hlavička
        fprintf(f_out, "%s", line_buf);  // Zapíšeme hlavičku do nového súboru
        while (fgets(line_buf, sizeof(line_buf), f_in)) {
            total_lines++;
        }

        // 2. Vypočítame, koľko najstarších riadkov zahodíme (20%)
        int lines_to_skip = total_lines / 5;
        ESP_LOGI(TAG, "Celkovo riadkov: %d, zahadzujem: %d", total_lines,
                 lines_to_skip);

        // 3. Znova prejdeme súbor a skopírujeme len relevantné dáta
        fseek(f_in, 0, SEEK_SET);
        fgets(line_buf, sizeof(line_buf), f_in);  // Znova preskočíme hlavičku

        int current_line = 0;
        while (fgets(line_buf, sizeof(line_buf), f_in)) {
            current_line++;
            if (current_line > lines_to_skip) {
                fprintf(f_out, "%s", line_buf);
            }
        }

        fclose(f_in);
        fclose(f_out);

        // 4. Bezpečná "atomic" operácia (Zanshin) - ochrana pred výpadkom prúdu
        char old_filename[40];
        snprintf(old_filename, sizeof(old_filename), "%s.old", filename);

        // A: Pre istotu zmažeme .old, ak tam zostal po predchádzajúcom tvrdom
        // reštarte
        remove(old_filename);

        // B: Pôvodný súbor premenujeme na .old (stále ho máme, ak by teraz
        // vypadla elektrina)
        rename(filename, old_filename);

        // C: Nový, vyčistený .tmp premenujeme na ostrý názov
        rename(temp_filename, filename);

        // D: Sme v bezpečí, ostrý súbor existuje. Zmažeme zálohu.
        remove(old_filename);

        // Korekcia pocitadla zaznamov (rotacia zahodila lines_to_skip
        // najstarsich riadkov) - podla nazvu suboru vieme, ktore pocitadlo
        // upravit bez opatovneho prehladavania celeho suboru.
        int new_count = total_lines - lines_to_skip;
        if (strcmp(filename, "/data/sensor.csv") == 0) {
            s_sensor_record_count = new_count;
        } else if (strcmp(filename, "/data/weather.csv") == 0) {
            s_weather_record_count = new_count;
        }

        ESP_LOGI(TAG, "Rotácia logu %s úspešne dokončená (Fail-Safe flow).",
                 filename);
    }
}

void storage_log_sensor_data(uint32_t timestamp, float t, float h, float p) {
    storage_rotate_log_if_needed("/data/sensor.csv");
    FILE* f = fopen("/data/sensor.csv", "a");
    if (f) {
        // Formátovanie striktne podľa CsvStructure.md (tlak ako int)
        fprintf(f, "%lu;%.1f;%.1f;%d\n", timestamp, t, h, (int)p);
        fclose(f);
        s_sensor_record_count++;
    }

    // Aktualizácia trend buffra v RAM
    s_trend_buffer[s_trend_idx] = (int)p;
    s_trend_idx = (s_trend_idx + 1) % TREND_MAX_SAMPLES;
    if (s_trend_count < TREND_MAX_SAMPLES) s_trend_count++;
}

void storage_log_weather_data(uint32_t timestamp, const char* city, float t,
                              int h, int p) {
    storage_rotate_log_if_needed("/data/weather.csv");
    FILE* f = fopen("/data/weather.csv", "a");
    if (f) {
        fprintf(f, "%lu;%s;%.1f;%d;%d\n", timestamp, city ? city : "", t, h, p);
        fclose(f);
        s_weather_record_count++;
    }
}

int storage_get_sensor_record_count(void) { return s_sensor_record_count; }

int storage_get_weather_record_count(void) { return s_weather_record_count; }

int storage_get_temperature_history(uint32_t since_timestamp, float* temp_array,
                                    int max_count) {
    FILE* f = fopen("/data/sensor.csv", "r");
    if (!f) return 0;

    char line_buf[64];
    int count = 0;

    while (fgets(line_buf, sizeof(line_buf), f)) {
        uint32_t ts = strtoul(line_buf, NULL, 10);
        if (ts == 0)
            continue;  // Preskočí iba hlavičku, vždy načítame čo najviac dát
                       // pre graf
        if (ts < since_timestamp)
            continue;  // Filter podľa zvoleného rozsahu dní

        char* p1 = strchr(line_buf, ';');
        if (p1) {
            float temp = atof(p1 + 1);
            if (count < max_count) {
                temp_array[count++] = temp;
            } else {
                // Shift posun doľava, ak pretečie limit (efektívne správanie
                // kruhového buffra)
                memmove(temp_array, temp_array + 1,
                        (max_count - 1) * sizeof(float));
                temp_array[max_count - 1] = temp;
            }
        }
    }
    fclose(f);
    return count;
}

int storage_get_weather_history(uint32_t since_timestamp, float* temp_array,
                                int max_count) {
    FILE* f = fopen("/data/weather.csv", "r");
    if (!f) return 0;

    char line_buf[64];
    int count = 0;

    while (fgets(line_buf, sizeof(line_buf), f)) {
        uint32_t ts = strtoul(line_buf, NULL, 10);
        if (ts == 0)
            continue;  // Preskočí iba hlavičku, vždy načítame čo najviac dát
                       // pre graf
        if (ts < since_timestamp) continue;

        char* p1 = strchr(line_buf, ';');
        if (p1) {
            char* p2 = strchr(p1 + 1, ';');  // Preskočíme mesto
            if (p2) {
                float temp = atof(p2 + 1);
                if (count < max_count) {
                    temp_array[count++] = temp;
                } else {
                    memmove(temp_array, temp_array + 1,
                            (max_count - 1) * sizeof(float));
                    temp_array[max_count - 1] = temp;
                }
            }
        }
    }
    fclose(f);
    return count;
}

void storage_get_fs_info(size_t* total, size_t* used) {
    // Natívne ESP-IDF API vráti aktuálny stav partície "storage"
    esp_littlefs_info("storage", total, used);
}

int storage_get_pressure_trend() {
    if (s_trend_count < TREND_MAX_SAMPLES) {
        return -2;  // Nedostatok dát pre výpočet 3-hodinového trendu
    }
    // Najnovší záznam je na (idx - 1), najstarší na (idx) v plnom buffri
    int current_p = s_trend_buffer[(s_trend_idx + TREND_MAX_SAMPLES - 1) %
                                   TREND_MAX_SAMPLES];
    int old_p = s_trend_buffer[s_trend_idx];

    int diff = current_p - old_p;
    if (diff >= 2) return 1;    // Stúpa
    if (diff <= -2) return -1;  // Klesá
    return 0;                   // Stabilný
}