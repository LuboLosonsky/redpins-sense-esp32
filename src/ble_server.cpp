// Zanshin: Ignorujeme varovania o chýbajúcich inicializátoroch štruktúr (NimBLE
// ich má veľa nepotrebných)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"

#include "ble_server.h"

#include <stdio.h>

#include "app_config.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sensor_core.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "storage.h"
#include "wifi_scanner.h"

static const char* TAG = "BLE_SERVER";
static char device_name[24] = "RP-S-0000";  // Dynamicky z MAC adresy

static uint8_t own_addr_type;
static uint16_t conn_handle =
    BLE_HS_CONN_HANDLE_NONE;  // Uchováva identifikátor aktívneho spojenia

static uint16_t telemetry_handle;   // Interný handle pre NOTIFY A101
static uint16_t datastream_handle;  // Interný handle pre NOTIFY A104

// --- RCP v2.1 UUID Definície (Reverse byte order pre natívny BLE stack) ---
// Služba (Sense): fd651000-b3d1-4e52-8b25-45c63b72a100
static const ble_uuid128_t svc_uuid =
    BLE_UUID128_INIT(0x00, 0xa1, 0x72, 0x3b, 0xc6, 0x45, 0x25, 0x8b, 0x52, 0x4e,
                     0xd1, 0xb3, 0x00, 0x10, 0x65, 0xfd);

// A101 Telemetry (READ, NOTIFY)
static const ble_uuid128_t char_telemetry_uuid =
    BLE_UUID128_INIT(0x01, 0xa1, 0x72, 0x3b, 0xc6, 0x45, 0x25, 0x8b, 0x52, 0x4e,
                     0xd1, 0xb3, 0x00, 0x10, 0x65, 0xfd);

// A102 Device Info (READ)
static const ble_uuid128_t char_devinfo_uuid =
    BLE_UUID128_INIT(0x02, 0xa1, 0x72, 0x3b, 0xc6, 0x45, 0x25, 0x8b, 0x52, 0x4e,
                     0xd1, 0xb3, 0x00, 0x10, 0x65, 0xfd);

// A103 Command (WRITE)
static const ble_uuid128_t char_command_uuid =
    BLE_UUID128_INIT(0x03, 0xa1, 0x72, 0x3b, 0xc6, 0x45, 0x25, 0x8b, 0x52, 0x4e,
                     0xd1, 0xb3, 0x00, 0x10, 0x65, 0xfd);

// A104 Data Stream (NOTIFY)
static const ble_uuid128_t char_datastream_uuid =
    BLE_UUID128_INIT(0x04, 0xa1, 0x72, 0x3b, 0xc6, 0x45, 0x25, 0x8b, 0x52, 0x4e,
                     0xd1, 0xb3, 0x00, 0x10, 0x65, 0xfd);

// --- BLE Access Callbacks ---

// Callback pre A101 Telemetry (READ)
static int ble_svc_telemetry_access(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt* ctxt,
                                    void* arg) {
    ESP_LOGI(TAG,
             "BLE Request: Zariadenie vyžaduje čítanie Telemetrie (A101) - "
             "Skúšam DHT11");

    float t = 0.0, h = 0.0;
    sensor_core_get_latest(&t, &h);
    uint16_t p = (t != 0.0 || h != 0.0) ? 1013 : 0;

    // RCP v2.1: Binárny payload pre A101 (Little Endian: 4B Temp, 4B Hum, 2B
    // Pres)
    uint8_t payload[10];
    memcpy(&payload[0], &t, 4);
    memcpy(&payload[4], &h, 4);
    memcpy(&payload[8], &p, 2);

    int rc = os_mbuf_append(ctxt->om, payload, sizeof(payload));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// Callback pre A102 Device Info (READ)
static int ble_svc_devinfo_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt* ctxt,
                                  void* arg) {
    ESP_LOGI(TAG, "BLE Request: Zariadenie %d vyžaduje Device Info (A102)",
             conn_handle);

    // Dynamické metadáta podľa špecifikácie RCP v2.1
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t uptime_s = esp_timer_get_time() / 1000000ULL;
    bool wifi_connected = wifi_scanner_is_connected();

    char ip[16] = "";
    if (wifi_connected) {
        wifi_scanner_get_ip(ip, sizeof(ip));
    }

    app_config_t* cfg = app_config_get();

    char dev_info_json[256];
    snprintf(dev_info_json, sizeof(dev_info_json),
             "{\"hw\":\"C6-V1\",\"bat\":100,\"free_heap\":%lu,\"uptime_s\":%lu,"
             "\"wifi_connected\":%s,\"ip\":\"%s\",\"alias\":\"%s\",\"lat\":%."
             "6f,\"lon\":%.6f}",
             free_heap, uptime_s, wifi_connected ? "true" : "false", ip,
             cfg->alias, cfg->lat, cfg->lon);

    int rc = os_mbuf_append(ctxt->om, dev_info_json, strlen(dev_info_json));

    ESP_LOGI(TAG, "BLE Sending Response: %s (%d bytes)", dev_info_json,
             strlen(dev_info_json));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// Wrapper pre asynchrónne spustenie skenovania mimo NimBLE tasku
static void wifi_scan_task(void* param) {
    wifi_scanner_scan_and_stream();
    vTaskDelete(NULL);
}

// Wrapper pre asynchrónne čítanie z LittleFS
static void sync_task(void* param) {
    uint32_t since_ts = *(uint32_t*)param;
    free(param);  // Uvoľníme parameter
    storage_sync_sensors(since_ts);
    vTaskDelete(NULL);
}

// Wrapper pre asynchrónne čítanie počasia z LittleFS
static void sync_weather_task(void* param) {
    uint32_t since_ts = *(uint32_t*)param;
    free(param);  // Uvoľníme parameter
    storage_sync_weather(since_ts);
    vTaskDelete(NULL);
}

// Asynchrónne generovanie a streamovanie SYS_INFO (0x04) po 20-bajtových
// chunkoch
static void sys_info_task(void* param) {
    uint32_t free_heap = esp_get_free_heap_size();
    uint32_t uptime_s = esp_timer_get_time() / 1000000ULL;
    bool wifi_connected = wifi_scanner_is_connected();

    char ip[16] = "";
    if (wifi_connected) {
        wifi_scanner_get_ip(ip, sizeof(ip));
    }

    app_config_t* cfg = app_config_get();

    char json[256];
    int json_len = snprintf(
        json, sizeof(json),
        "{\"hw\":\"C6-V1\",\"bat\":100,\"free_heap\":%lu,\"uptime_s\":%lu,"
        "\"wifi_connected\":%s,\"ip\":\"%s\",\"alias\":\"%s\",\"lat\":%.6f,"
        "\"lon\":%.6f}",
        free_heap, uptime_s, wifi_connected ? "true" : "false", ip, cfg->alias,
        cfg->lat, cfg->lon);

    size_t offset = 0;
    while (offset < json_len) {
        size_t copy_size = (json_len - offset > 19) ? 19 : (json_len - offset);
        uint8_t buffer[20];
        buffer[0] = 0xFD;  // Hlavička JSON streamu pre Android
        memcpy(&buffer[1], json + offset, copy_size);
        ble_notify_datastream(buffer, copy_size + 1);
        offset += copy_size;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint8_t eot[2] = {0xFD, '\0'};
    ble_notify_datastream(eot, 2);

    vTaskDelete(NULL);
}

// Streamovanie uložených sietí (A104) pre Android UI
static void wifi_list_saved_task(void* param) {
    char ssid[33] = {0};
    nvs_handle_t nvs_handle;
    if (nvs_open("wifi_cfg", NVS_READONLY, &nvs_handle) == ESP_OK) {
        size_t ssid_len = sizeof(ssid);
        nvs_get_str(nvs_handle, "ssid", ssid, &ssid_len);
        nvs_close(nvs_handle);
    }

    char json[128];
    if (strlen(ssid) > 0) {
        snprintf(json, sizeof(json), "[{\"ssid\":\"%s\"}]", ssid);
    } else {
        snprintf(json, sizeof(json), "[]");
    }

    int json_len = strlen(json);
    size_t offset = 0;
    while (offset < json_len) {
        size_t copy_size = (json_len - offset > 19) ? 19 : (json_len - offset);
        uint8_t buffer[20];
        buffer[0] = 0xFD;
        memcpy(&buffer[1], json + offset, copy_size);
        ble_notify_datastream(buffer, copy_size + 1);
        offset += copy_size;
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint8_t eot[2] = {0xFD, '\0'};
    ble_notify_datastream(eot, 2);

    vTaskDelete(NULL);
}

// Callback pre A103 Command (WRITE)
static int ble_svc_command_write(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt* ctxt, void* arg) {
    uint16_t total_len = OS_MBUF_PKTLEN(ctxt->om);
    ESP_LOGI(TAG, "BLE Command: Prijatý WRITE (A103), dĺžka: %d bajtov",
             total_len);

    // Profi debugging: Vypísanie prijatých dát v HEX formáte bez alokácie na
    // Heape
    uint8_t buf[128];  // Zväčšené pre WIFI_CONNECT (SSID 32B + PWD 64B)
    uint16_t len = total_len > sizeof(buf) ? sizeof(buf) : total_len;
    os_mbuf_copydata(ctxt->om, 0, len, buf);

    ESP_LOG_BUFFER_HEX("BLE_CMD_PAYLOAD", buf, len);

    if (len > 0) {
        uint8_t cmd = buf[0];
        switch (cmd) {
            case 0x21: {  // SENSOR_FORCE
                ESP_LOGI(TAG,
                         "Príkaz: SENSOR_FORCE (0x21). Odosielam telemetriu na "
                         "vyžiadanie.");

                float t = 0.0, h = 0.0;
                sensor_core_get_latest(&t, &h);
                uint16_t p = (t != 0.0 || h != 0.0) ? 1013 : 0;

                ble_notify_telemetry(t, h, p);
                break;
            }
            case 0x01:  // REBOOT
                ESP_LOGI(TAG, "Príkaz: REBOOT (0x01). Reštartujem systém.");
                esp_restart();
                break;
            case 0x02:  // FACTORY_RESET
                ESP_LOGI(TAG, "Príkaz: FACTORY_RESET (0x02). Vymazávam údaje.");
                nvs_flash_erase();
                remove("/data/config.json");
                remove("/data/sensor.csv");
                remove("/data/weather.csv");
                esp_restart();
                break;
            case 0x03:  // DEEP_SLEEP
                ESP_LOGI(TAG, "Príkaz: DEEP_SLEEP (0x03).");
                esp_deep_sleep_start();
                break;
            case 0x04:  // SYS_INFO
                ESP_LOGI(
                    TAG,
                    "Príkaz: SYS_INFO (0x04). Streamujem metadáta na A104.");
                xTaskCreate(sys_info_task, "sys_info", 4096, NULL, 2, NULL);
                break;
            case 0x11:  // WIFI_SCAN
                ESP_LOGI(TAG,
                         "Príkaz: WIFI_SCAN (0x11). Spúšťam natívny sken na "
                         "pozadí.");
                xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 2, NULL);
                break;
            case 0x12: {  // WIFI_CONNECT
                ESP_LOGI(
                    TAG,
                    "Príkaz: WIFI_CONNECT (0x12). Parsujem binárny payload.");
                if (len >= 3) {
                    uint8_t ssid_len = buf[1];
                    if (2 + ssid_len < len) {
                        char ssid[33] = {0};  // +1 pre '\0' terminátor
                        memcpy(ssid, &buf[2], ssid_len);

                        // Zanshin: Orezanie bielych znakov a enterov z konca
                        // SSID
                        for (int i = strlen(ssid) - 1;
                             i >= 0 && (ssid[i] == '\n' || ssid[i] == '\r' ||
                                        ssid[i] == ' ');
                             i--) {
                            ssid[i] = '\0';
                        }

                        uint8_t pwd_len = buf[2 + ssid_len];
                        char pwd[65] = {0};  // +1 pre '\0' terminátor
                        if (2 + ssid_len + 1 + pwd_len <= len) {
                            memcpy(pwd, &buf[2 + ssid_len + 1], pwd_len);

                            // Zanshin: Orezanie bielych znakov a enterov z
                            // konca hesla
                            for (int i = strlen(pwd) - 1;
                                 i >= 0 && (pwd[i] == '\n' || pwd[i] == '\r' ||
                                            pwd[i] == ' ');
                                 i--) {
                                pwd[i] = '\0';
                            }

                            ESP_LOGI(TAG,
                                     "WIFI_CONNECT -> SSID: '%s', PWD: '%s'",
                                     ssid, pwd);
                            wifi_scanner_connect(ssid, pwd);
                        } else {
                            ESP_LOGW(
                                TAG,
                                "WIFI_CONNECT -> Odmietnuté: Neúplný payload "
                                "hesla (očakávané >= %d, prijaté %d)",
                                2 + ssid_len + 1 + pwd_len, len);
                        }
                    } else {
                        ESP_LOGW(TAG,
                                 "WIFI_CONNECT -> Odmietnuté: Neúplný payload "
                                 "SSID (očakávané >= %d, prijaté %d)",
                                 2 + ssid_len, len);
                    }
                }
                break;
            }
            case 0x13:  // WIFI_DISCONNECT
                ESP_LOGI(TAG, "Príkaz: WIFI_DISCONNECT (0x13)");
                esp_wifi_disconnect();
                xTaskCreate(sys_info_task, "sys_info", 4096, NULL, 2, NULL);
                break;
            case 0x14:  // WIFI_LIST_SAVED
                ESP_LOGI(TAG, "Príkaz: WIFI_LIST_SAVED (0x14)");
                xTaskCreate(wifi_list_saved_task, "wifi_saved", 4096, NULL, 2,
                            NULL);
                break;
            case 0x23: {  // SENSOR_SYNC (Delta Sync)
                uint32_t since_ts = 0;
                if (len >= 5) {
                    // Little Endian konverzia nezávislá na architektúre
                    since_ts = buf[1] | (buf[2] << 8) | (buf[3] << 16) |
                               (buf[4] << 24);
                }
                ESP_LOGI(TAG, "Príkaz: SENSOR_SYNC (0x23). Timestamp: %lu",
                         since_ts);

                uint32_t* ts_param = (uint32_t*)malloc(sizeof(uint32_t));
                *ts_param = since_ts;
                xTaskCreate(sync_task, "sync_task", 4096, ts_param, 2, NULL);
                break;
            }
            case 0x24: {  // WEATHER_SYNC (Delta Sync)
                uint32_t since_ts = 0;
                if (len >= 5) {
                    // Little Endian konverzia nezávislá na architektúre
                    since_ts = buf[1] | (buf[2] << 8) | (buf[3] << 16) |
                               (buf[4] << 24);
                }
                ESP_LOGI(TAG, "Príkaz: WEATHER_SYNC (0x24). Timestamp: %lu",
                         since_ts);

                uint32_t* ts_param = (uint32_t*)malloc(sizeof(uint32_t));
                *ts_param = since_ts;
                xTaskCreate(sync_weather_task, "sync_w_task", 4096, ts_param, 2,
                            NULL);
                break;
            }
            case 0x25:  // LOG_CLEAR
                ESP_LOGI(TAG, "Príkaz: LOG_CLEAR (0x25)");
                remove("/data/sensor.csv");
                remove("/data/weather.csv");
                break;
            case 0x26: {  // SET_CALIBRATION
                ESP_LOGI(TAG, "Príkaz: SET_CALIBRATION (0x26).");
                if (len >= 9) {
                    float dht_off, bmp_off;
                    memcpy(&dht_off, &buf[1], 4);
                    memcpy(&bmp_off, &buf[5], 4);
                    ESP_LOGI(TAG, "Nové offsety: DHT=%.2f, BMP=%.2f", dht_off,
                             bmp_off);

                    app_config_t* cfg = app_config_get();
                    cfg->dht_temp_offset = dht_off;
                    cfg->bmp_temp_offset = bmp_off;
                    app_config_save();
                } else {
                    ESP_LOGW(TAG,
                             "SET_CALIBRATION -> Odmietnuté: Neúplný payload "
                             "(očakávané 9, prijaté %d)",
                             len);
                }
                break;
            }
            case 0x31: {  // SET_COORDS
                ESP_LOGI(TAG, "Príkaz: SET_COORDS (0x31).");
                if (len >= 9) {
                    float lat, lon;
                    memcpy(&lat, &buf[1], 4);
                    memcpy(&lon, &buf[5], 4);
                    ESP_LOGI(TAG, "Nové GPS: Lat=%.6f, Lon=%.6f", lat, lon);

                    app_config_t* cfg = app_config_get();
                    cfg->lat = lat;
                    cfg->lon = lon;
                    app_config_save();
                    xTaskCreate(sys_info_task, "sys_info", 4096, NULL, 2, NULL);
                } else {
                    ESP_LOGW(TAG,
                             "SET_COORDS -> Odmietnuté: Neúplný payload "
                             "(očakávané 9, prijaté %d)",
                             len);
                }
                break;
            }
            case 0x32: {  // SET_ALIAS
                ESP_LOGI(
                    TAG,
                    "Príkaz: SET_ALIAS (0x32). Parsujem string z payloadu.");
                if (len >= 2) {
                    uint8_t alias_len = buf[1];
                    if (2 + alias_len <= len) {
                        app_config_t* cfg = app_config_get();
                        size_t max_len = sizeof(cfg->alias) - 1;
                        size_t copy_len =
                            alias_len < max_len ? alias_len : max_len;

                        memcpy(cfg->alias, &buf[2], copy_len);
                        cfg->alias[copy_len] = '\0';

                        ESP_LOGI(TAG, "Nový Alias nastavený na: '%s'",
                                 cfg->alias);
                        app_config_save();  // Uloží zmenu na disk (LittleFS)
                        xTaskCreate(sys_info_task, "sys_info", 4096, NULL, 2,
                                    NULL);
                    } else {
                        ESP_LOGW(TAG,
                                 "SET_ALIAS -> Odmietnuté: Neúplný payload "
                                 "(očakávané >= %d, prijaté %d)",
                                 2 + alias_len, len);
                    }
                } else {
                    ESP_LOGW(TAG,
                             "SET_ALIAS -> Odmietnuté: Chýba dĺžka aliasu");
                }
                break;
            }
            default:
                ESP_LOGW(TAG, "Neznámy alebo nezabudovaný príkaz: 0x%02X", cmd);
                break;
        }
    }
    return 0;
}

// Callback pre A104 Data Stream (Iba NOTIFY - NimBLE vyžaduje non-NULL pointer)
static int ble_svc_datastream_access(uint16_t conn_handle, uint16_t attr_handle,
                                     struct ble_gatt_access_ctxt* ctxt,
                                     void* arg) {
    return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
}

// --- Zanshin: Statická GATT Tabuľka (Žiadna fragmentácia v RAM) ---
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &char_telemetry_uuid.u,
                    .access_cb = ble_svc_telemetry_access,  // Opravené: Musí
                                                            // mať callback
                    .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &telemetry_handle,
                },
                {
                    .uuid = &char_devinfo_uuid.u,
                    .access_cb = ble_svc_devinfo_access,
                    .flags = BLE_GATT_CHR_F_READ,
                },
                {
                    .uuid = &char_command_uuid.u,
                    .access_cb = ble_svc_command_write,
                    .flags = BLE_GATT_CHR_F_WRITE,
                },
                {
                    .uuid = &char_datastream_uuid.u,
                    .access_cb = ble_svc_datastream_access,  // Opravené: NimBLE
                                                             // netoleruje NULL
                    .flags = BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &datastream_handle,
                },
                {}  // Koniec poľa charakteristík
            },
    },
    {}  // Koniec poľa služieb
};

static void ble_app_advertise();

static int ble_gap_event(struct ble_gap_event* event, void* arg) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                ESP_LOGE(TAG, "BLE Connection failed! Status: %d",
                         event->connect.status);
                ble_app_advertise();
            } else {
                ESP_LOGI(TAG, "BLE Connected! Conn Handle: %d",
                         event->connect.conn_handle);
                conn_handle = event->connect.conn_handle;

                // Zanshin: Vynútenie 20ms-40ms intervalu pre Focus/Core
                // kompatibilitu. Bez tohto Android nestihne poslať CCCD
                // Subscribe paket skôr, než WiFi zablokuje rádio.
                struct ble_gap_upd_params params = {.itvl_min = 16,  // 20ms
                                                    .itvl_max = 32,  // 40ms
                                                    .latency = 0,
                                                    .supervision_timeout = 400,
                                                    .min_ce_len = 0,
                                                    .max_ce_len = 0};
                ble_gap_update_params(conn_handle, &params);
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(TAG, "BLE Disconnected! Conn Handle: %d, Dôvod: %d",
                     event->disconnect.conn.conn_handle,
                     event->disconnect.reason);
            conn_handle = BLE_HS_CONN_HANDLE_NONE;  // Uvoľníme spojenie
            ble_app_advertise();  // Automatická obnova viditeľnosti
            break;
    }
    return 0;
}

static void ble_app_advertise() {
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // Paket 1: Adv Data (3 + 18 = 21 bajtov) -> Iba UUID
    fields.uuids128 = (ble_uuid128_t*)&svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    // Pridanie preferovaného intervalu pripojenia (rovnako ako v starom kóde)
    static const uint8_t slave_itvl_range_val[4] = {
        0x06, 0x00, 0x12, 0x00};  // min 7.5ms, max 22.5ms
    fields.slave_itvl_range = slave_itvl_range_val;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) ESP_LOGE(TAG, "Chyba nastavenia ADV fields: %d", rc);

    // Paket 2: Scan Response (11 bajtov) -> Názov zariadenia
    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (uint8_t*)device_name;
    rsp_fields.name_len = strlen(device_name);
    rsp_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) ESP_LOGE(TAG, "Chyba nastavenia RSP fields: %d", rc);

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                      ble_gap_event, NULL);
    ESP_LOGI(TAG, "Advertising spustený ako '%s'", device_name);
}

static void ble_app_on_sync() {
    ble_hs_id_infer_auto(0, &own_addr_type);
    ble_app_advertise();
}

static void ble_host_task(void* param) {
    ESP_LOGI(TAG, "NimBLE Host Task spustený.");
    nimble_port_run();  // Blokujúca funkcia pre NimBLE stack
    nimble_port_freertos_deinit();
}

void ble_notify_telemetry(float t, float h, uint16_t p) {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || telemetry_handle == 0)
        return;  // Nikto nepočúva

    ESP_LOGI(TAG, "📤 BLE TX (A101) -> Telemetria: %.1f°C, %.1f%%, %dhPa", t, h,
             p);

    // RCP v2.1: Binárny payload pre A101 (Little Endian: 4B Temp, 4B Hum, 2B
    // Pres)
    uint8_t payload[10];
    memcpy(&payload[0], &t, 4);
    memcpy(&payload[4], &h, 4);
    memcpy(&payload[8], &p, 2);

    // Zero-copy: Alokujeme mbuf z poolu sieťového stacku (nefragmentuje
    // FreeRTOS heap)
    struct os_mbuf* om = ble_hs_mbuf_from_flat(payload, sizeof(payload));
    if (om) {
        int rc = ble_gatts_notify_custom(conn_handle, telemetry_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "Chyba telemetrie (rc=%d). Android nepočúva (CCCD).",
                     rc);
        }
    }
}

void ble_notify_datastream(const uint8_t* data, size_t length) {
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE || datastream_handle == 0) {
        ESP_LOGW(TAG, "Notifikácia zahodená: Nie je spojenie alebo handle.");
        return;
    }

    if (data[0] == 0xFD) {
        char tmp[24] = {0};
        size_t text_len = (length - 1 < 23) ? (length - 1) : 23;
        memcpy(tmp, &data[1], text_len);
        ESP_LOGI(TAG, "📤 BLE TX (A104) JSON Stream: %s", tmp);
    } else if (data[0] == 0xFE) {
        char tmp[24] = {0};
        size_t text_len = (length - 1 < 23) ? (length - 1) : 23;
        memcpy(tmp, &data[1], text_len);
        ESP_LOGI(TAG, "📤 BLE TX (A104) CSV Stream: %s", tmp);
    } else {
        ESP_LOGI(TAG, "📤 BLE TX (A104) Binárny/EOT paket, dĺžka: %d", length);
        ESP_LOG_BUFFER_HEX("BLE_TX_HEX", data, length);
    }

    struct os_mbuf* om = ble_hs_mbuf_from_flat(data, length);
    if (om) {
        int rc = ble_gatts_notify_custom(conn_handle, datastream_handle, om);
        if (rc != 0) {
            // Ak vypíše chybu, zvyčajne je to chyba 528 (0x0210 -
            // BLE_HS_ENOTSUB)
            ESP_LOGW(TAG,
                     "Android odmietol DATA_STREAM paket (rc=%d). Sú zapnuté "
                     "notifikácie?",
                     rc);
        }
    } else {
        ESP_LOGE(TAG, "Nedostatok RAM pre vytvorenie mbuf notifikácie!");
    }
}

void ble_server_init() {
    // Dynamické generovanie mena podľa RCP v2.1 (rovnako ako v starom Arduino
    // kóde)
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(device_name, sizeof(device_name), "RP-S-%02X%02X", mac[4], mac[5]);

    // Zanshin: Skrytie otravného spamu z NimBLE jadra, naše logy zostanú
    // zachované
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    // 1. Inicializácia portu NimBLE stacku MUSÍ byť úplne prvá
    nimble_port_init();

    // Výkon antény (+9dBm) z tvojho starého configu
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);

    // 2. Inicializácia základných služieb (Kritické predtým, než pridáme
    // vlastné)
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Nastavenie GAP Device Name, aby pri read requeste z Androidu sedelo meno
    ble_svc_gap_device_name_set(device_name);

    // Inicializácia služieb
    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    ESP_ERROR_CHECK(rc);
    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    ESP_ERROR_CHECK(rc);

    ble_hs_cfg.sync_cb = ble_app_on_sync;
    nimble_port_freertos_init(ble_host_task);
}

bool ble_server_is_connected() {
    return conn_handle != BLE_HS_CONN_HANDLE_NONE;
}