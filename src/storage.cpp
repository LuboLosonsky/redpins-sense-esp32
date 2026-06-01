#include "storage.h"

#include <stdio.h>
#include <stdlib.h>

#include "ble_server.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "STORAGE";

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
    }

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

void storage_log_sensor_data(uint32_t timestamp, float t, float h, float p) {
    FILE* f = fopen("/data/sensor.csv", "a");
    if (f) {
        // Formátovanie striktne podľa CsvStructure.md (tlak ako int)
        fprintf(f, "%lu;%.1f;%.1f;%d\n", timestamp, t, h, (int)p);
        fclose(f);
    }
}