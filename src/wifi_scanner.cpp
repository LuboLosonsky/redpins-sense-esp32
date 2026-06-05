#include "wifi_scanner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ble_server.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char* TAG = "WIFI_SCANNER";
static bool wifi_initialized = false;
static int wifi_retry_count = 0;
static bool s_is_connected = false;
static char s_ip_address[16] = "";

// --- Asynchrónny Event Handler pre Wi-Fi ---
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        if (wifi_retry_count < 3) {
            wifi_retry_count++;
            ESP_LOGW(TAG,
                     "Wi-Fi odpojené (Zlé heslo alebo signál). Pokus %d/3...",
                     wifi_retry_count);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Pripojenie k Wi-Fi zlyhalo. Zastavujem pokusy.");
            wifi_retry_count = 0;  // Reset pre ďalšie príkazy

            // Zanshin: Upozorníme Android Core o zlyhaní pripojenia
            char json[] = "{\"cmd\":\"wifi_failed\"}";
            uint8_t payload[32];
            payload[0] = 0xFD;
            memcpy(&payload[1], json, strlen(json));
            payload[strlen(json) + 1] = '\0';

            size_t offset = 0;
            size_t total_len = strlen(json) + 2;
            while (offset < total_len) {
                size_t chunk_size =
                    (total_len - offset > 20) ? 20 : (total_len - offset);
                ble_notify_datastream(&payload[offset], chunk_size);
                offset += chunk_size;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_retry_count = 0;  // Úspech, reset počítadla
        s_is_connected = true;
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        snprintf(s_ip_address, sizeof(s_ip_address), IPSTR,
                 IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "=== PRIPOJENÉ === IP Adresa: %s", s_ip_address);

        // Inicializácia SNTP (Reálny čas pre Delta Sync)
        if (!esp_sntp_enabled()) {
            ESP_LOGI(TAG, "Inicializujem SNTP klienta (CET/CEST timezone)...");

            // Aplikácia POSIX pravidla pre Strednú Európu (Zimný/Letný čas)
            setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
            tzset();

            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_setservername(1, "time.google.com");
            esp_sntp_init();
        }

        // Zero-copy JSON notifikácia pre Android Core
        char json[128];
        int json_len = snprintf(json, sizeof(json),
                                "{\"cmd\":\"wifi_connected\",\"ip\":\"%s\"}",
                                s_ip_address);

        // Flow formát pre Android: 0xFD + [JSON] + 0x00
        uint8_t payload[130];
        payload[0] = 0xFD;
        memcpy(&payload[1], json, json_len);
        payload[json_len + 1] = '\0';

        size_t offset = 0;
        size_t total_len = json_len + 2;
        while (offset < total_len) {
            size_t chunk_size =
                (total_len - offset > 20) ? 20 : (total_len - offset);
            ble_notify_datastream(&payload[offset], chunk_size);
            offset += chunk_size;
            vTaskDelay(pdMS_TO_TICKS(20));  // Flow control
        }
    }
}

static void ensure_wifi_init() {
    if (!wifi_initialized) {
        ESP_LOGI(TAG, "Inicializujem WiFi stack...");
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);

        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_initialized = true;
    }
}

void wifi_scanner_connect(const char* ssid, const char* password) {
    ensure_wifi_init();

    // Uloženie credentials do NVS pamäte podľa štruktúry z Focusu
    nvs_handle_t nvs_handle;
    if (nvs_open("wifi_cfg", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_set_str(nvs_handle, "ssid", ssid);
        nvs_set_str(nvs_handle, "pass", password);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        ESP_LOGI(TAG, "NVS: Prístupové údaje k '%s' bezpečne uložené.", ssid);
    }

    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password,
            sizeof(wifi_config.sta.password) - 1);

    // Pre moderné routre zaručíme kompatibilitu s WPA2/WPA3 mixed módmi
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    // Najprv sa pre istotu odpojíme (ak sme už boli niekde zavesení)
    esp_wifi_disconnect();
    wifi_retry_count = 0;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "Pripájam k Wi-Fi '%s'...", ssid);
    ESP_ERROR_CHECK(esp_wifi_connect());
}

void wifi_scanner_auto_connect() {
    ensure_wifi_init();

    nvs_handle_t nvs_handle;
    if (nvs_open("wifi_cfg", NVS_READONLY, &nvs_handle) == ESP_OK) {
        char ssid[33] = {0};
        char pass[65] = {0};
        size_t ssid_len = sizeof(ssid);
        size_t pass_len = sizeof(pass);

        if (nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len) == ESP_OK &&
            nvs_get_str(nvs_handle, "pass", pass, &pass_len) == ESP_OK) {
            ESP_LOGI(
                TAG,
                "NVS: Nájdené uložené údaje, automaticky pripájam k '%s'...",
                ssid);

            wifi_config_t wifi_config = {};
            strncpy((char*)wifi_config.sta.ssid, ssid,
                    sizeof(wifi_config.sta.ssid) - 1);
            strncpy((char*)wifi_config.sta.password, pass,
                    sizeof(wifi_config.sta.password) - 1);
            wifi_config.sta.pmf_cfg.capable = true;
            wifi_config.sta.pmf_cfg.required = false;

            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
            ESP_ERROR_CHECK(esp_wifi_connect());
        }
        nvs_close(nvs_handle);
    }
}

void wifi_scanner_scan_and_stream() {
    ensure_wifi_init();

    ESP_LOGI(TAG, "Spúšťam blokujúce skenovanie WiFi (Wu Wei)...");
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Zlyhalo skenovanie: %s", esp_err_to_name(err));
        uint8_t eof[2] = {0xFD, '\0'};
        ble_notify_datastream(eof, 2);
        return;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Skenovanie dokončené. Nájdených AP: %d", ap_count);

    // Zanshin: Obmedzíme na max 10 sietí, aby sme zabránili vyčerpaniu RAM
    // (prevencia Out Of Memory)
    if (ap_count > 10) ap_count = 10;

    wifi_ap_record_t ap_info[10];
    esp_wifi_scan_get_ap_records(&ap_count, ap_info);

    // Zero-copy prístup: Generovanie a chunkovanie za letu
    uint8_t chunk[20];
    chunk[0] = 0xFD;  // Hlavička JSON prúdu
    int chunk_offset = 1;

    auto flush_chunk = [&]() {
        if (chunk_offset > 1) {
            ble_notify_datastream(chunk, chunk_offset);
            chunk_offset = 1;  // Reset pre ďalší payload
            // Zanshin: Flow control, 20ms pauza pre Android a NimBLE queue
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    };

    auto append_to_stream = [&](const char* str) {
        while (*str) {
            chunk[chunk_offset++] = *str++;
            if (chunk_offset == 20) flush_chunk();
        }
    };

    append_to_stream("[");
    for (int i = 0; i < ap_count; i++) {
        char ap_json[128];
        snprintf(ap_json, sizeof(ap_json), "{\"ssid\":\"%.32s\",\"rssi\":%d}%s",
                 (char*)ap_info[i].ssid, ap_info[i].rssi,
                 (i < ap_count - 1) ? "," : "");
        append_to_stream(ap_json);
    }
    append_to_stream("]");
    flush_chunk();  // Vyprázdnenie zvyšku bufferu

    // Ukončovací paket pre Android
    uint8_t eof[2] = {0xFD, '\0'};
    ble_notify_datastream(eof, 2);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "WiFi scan úspešne odoslaný Androidu.");
}

bool wifi_scanner_is_connected() { return s_is_connected; }

int wifi_scanner_get_rssi() {
    if (!s_is_connected) return -100;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return -100;
}

void wifi_scanner_get_ssid(char* outBuffer, size_t maxLength) {
    if (s_is_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(outBuffer, maxLength, "%s", ap.ssid);
            return;
        }
    }
    outBuffer[0] = '\0';
}

void wifi_scanner_get_ip(char* outBuffer, size_t maxLength) {
    if (s_is_connected) {
        snprintf(outBuffer, maxLength, "%s", s_ip_address);
    } else {
        outBuffer[0] = '\0';
    }
}