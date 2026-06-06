#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ble_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "STORAGE";

// --- Zanshin: O(1) RAM cache pre barometrický trend ---
#define TREND_MAX_SAMPLES 18  // 3 hodiny (pri 10-minútovom intervale)
static int s_trend_buffer[TREND_MAX_SAMPLES];
static int s_trend_idx = 0;
static int s_trend_count = 0;

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
        FILE* fw = fopen("/data/weather.csv", "r");
        if (!fw) {
            ESP_LOGI(TAG,
                     "Súbor neexistuje, generujem testovací weather.csv s "
                     "hlavičkou...");
            fw = fopen("/data/weather.csv", "w");
            if (fw) {
                fprintf(fw, "timestamp;temp;hum;press;id\n");
                fprintf(fw, "1714500000;15.5;60;1013;800\n");
                fprintf(fw, "1714503600;16.2;58;1012;801\n");
                fclose(fw);
            }
        } else {
            fclose(fw);
        }
    }

    prefill_trend_buffer();

    return ESP_OK;
}

void storage_sync_sensors(uint32_t since_timestamp) {
    ESP_LOGI(TAG, "Spúšťam Delta Sync od timestampu: %lu", since_timestamp);

    FILE* f = fopen("/data/sensor.csv", "r");
    if (!f) {
        ESP_LOGE(TAG, "Súbor sensor.csv neexistuje!");
        uint8_t eof = 0xFF;
        ble_notify_datastream(&eof, 1);
        return;
    }

    long target_offset = 0;
    char line_buf[64];

    // Rýchly sekvenčný scan (O(n) na disku, O(1) v RAM)
    while (fgets(line_buf, sizeof(line_buf), f)) {
        uint32_t ts = strtoul(line_buf, NULL, 10);
        if (ts == 0 && target_offset == 0)
            continue;  // Zachová hlavičku pri prvej synchronizácii
        if (ts > since_timestamp) break;  // Našli sme prvý novší záznam
        target_offset = ftell(f);  // Inak si zapamätáme koniec tohto riadku
    }

    fseek(f, target_offset, SEEK_SET);
    ESP_LOGI(TAG, "Našiel som offset %ld. Začínam zero-copy stream.",
             target_offset);

    uint8_t chunk[20];
    chunk[0] = 0xFE;  // Hlavička pre CSV stream (RCP v1.3/2.1)

    while (true) {
        size_t bytes_read = fread(&chunk[1], 1, 19, f);
        if (bytes_read > 0) {
            ble_notify_datastream(chunk, bytes_read + 1);
            vTaskDelay(pdMS_TO_TICKS(20));  // Flow control okno
        }
        if (bytes_read < 19) break;  // End of File
    }

    fclose(f);

    uint8_t eof = 0xFF;  // Koniec prenosu
    ble_notify_datastream(&eof, 1);
    ESP_LOGI(TAG, "Delta Sync ukončený.");
}

void storage_sync_weather(uint32_t since_timestamp) {
    ESP_LOGI(TAG, "Spúšťam Delta Sync (Počasie) od timestampu: %lu",
             since_timestamp);

    FILE* f = fopen("/data/weather.csv", "r");
    if (!f) {
        ESP_LOGE(TAG, "Súbor weather.csv neexistuje!");
        uint8_t eof = 0xFF;
        ble_notify_datastream(&eof, 1);
        return;
    }

    long target_offset = 0;
    char line_buf[64];

    while (fgets(line_buf, sizeof(line_buf), f)) {
        uint32_t ts = strtoul(line_buf, NULL, 10);
        if (ts == 0 && target_offset == 0)
            continue;  // Zachová hlavičku pri prvej synchronizácii
        if (ts > since_timestamp) break;
        target_offset = ftell(f);
    }

    fseek(f, target_offset, SEEK_SET);
    ESP_LOGI(TAG, "Našiel som offset %ld. Začínam zero-copy stream počasia.",
             target_offset);

    uint8_t chunk[20];
    chunk[0] = 0xFE;  // Hlavička pre CSV stream

    while (true) {
        size_t bytes_read = fread(&chunk[1], 1, 19, f);
        if (bytes_read > 0) {
            ble_notify_datastream(chunk, bytes_read + 1);
            vTaskDelay(pdMS_TO_TICKS(20));  // Flow control okno
        }
        if (bytes_read < 19) break;  // End of File
    }

    fclose(f);

    uint8_t eof = 0xFF;  // Koniec prenosu
    ble_notify_datastream(&eof, 1);
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
    }

    // Aktualizácia trend buffra v RAM
    s_trend_buffer[s_trend_idx] = (int)p;
    s_trend_idx = (s_trend_idx + 1) % TREND_MAX_SAMPLES;
    if (s_trend_count < TREND_MAX_SAMPLES) s_trend_count++;
}

void storage_log_weather_data(uint32_t timestamp, float t, int h, int p,
                              int id) {
    storage_rotate_log_if_needed("/data/weather.csv");
    FILE* f = fopen("/data/weather.csv", "a");
    if (f) {
        fprintf(f, "%lu;%.1f;%d;%d;%d\n", timestamp, t, h, p, id);
        fclose(f);
    }
}

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
            float temp = atof(p1 + 1);
            if (count < max_count) {
                temp_array[count++] = temp;
            } else {
                memmove(temp_array, temp_array + 1,
                        (max_count - 1) * sizeof(float));
                temp_array[max_count - 1] = temp;
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