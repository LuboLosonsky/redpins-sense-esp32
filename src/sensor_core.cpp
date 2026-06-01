#include "sensor_core.h"

#include <time.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage.h"

static const char* TAG = "SENSOR_CORE";

// Zanshin: Bezpečný, voľný pin podľa fyzického layoutu dosky
#define DHT_PIN GPIO_NUM_18

static float s_last_t = 0.0;
static float s_last_h = 0.0;

void sensor_core_init() {
    ESP_LOGI(TAG, "Inicializácia DHT11 na pine %d", DHT_PIN);
    gpio_reset_pin(DHT_PIN);
    // Pripravíme pin s pull-up rezistorom, aby zbernica neplávala
    gpio_set_pull_mode(DHT_PIN, GPIO_PULLUP_ONLY);
}

bool sensor_core_read_dht11(float* temperature, float* humidity) {
    uint8_t data[5] = {0};

    // Štartovací signál pre DHT11
    gpio_set_direction(DHT_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(DHT_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));  // DHT11 potrebuje low aspoň 18ms
    gpio_set_level(DHT_PIN, 1);
    esp_rom_delay_us(30);  // Uvoľnenie na 20-40us
    gpio_set_direction(DHT_PIN, GPIO_MODE_INPUT);

    // Čakáme na odpoveď senzora
    int timeout = 0;
    while (gpio_get_level(DHT_PIN) == 1 && timeout++ < 100) esp_rom_delay_us(1);
    if (timeout >= 100) {
        ESP_LOGD(TAG, "Senzor neodpovedá (Timeout 1) - Kolízia s prerušením");
        return false;
    }

    timeout = 0;
    while (gpio_get_level(DHT_PIN) == 0 && timeout++ < 100) esp_rom_delay_us(1);
    if (timeout >= 100) {
        ESP_LOGD(TAG, "Senzor neodpovedá (Timeout 2) - Kolízia s prerušením");
        return false;
    }

    timeout = 0;
    while (gpio_get_level(DHT_PIN) == 1 && timeout++ < 100) esp_rom_delay_us(1);
    if (timeout >= 100) {
        ESP_LOGD(TAG, "Senzor neodpovedá (Timeout 3) - Kolízia s prerušením");
        return false;
    }

    // Čítanie 40 bitov dát
    for (int i = 0; i < 40; i++) {
        timeout = 0;
        while (gpio_get_level(DHT_PIN) == 0 && timeout++ < 100)
            esp_rom_delay_us(1);
        if (timeout >= 100) {
            ESP_LOGD(TAG, "Timeout pri čítaní bitu %d", i);
            return false;
        }

        uint32_t t = esp_timer_get_time();
        timeout = 0;
        while (gpio_get_level(DHT_PIN) == 1 && timeout++ < 100)
            esp_rom_delay_us(1);
        if (timeout >= 100) {
            ESP_LOGD(TAG, "Timeout pri čítaní bitu %d (High)", i);
            return false;
        }

        if ((esp_timer_get_time() - t) > 40)
            data[i / 8] |= (1 << (7 - (i % 8)));
    }

    // Kontrola parity (Checksum)
    if (data[4] == ((data[0] + data[1] + data[2] + data[3]) & 0xFF)) {
        *humidity = data[0] + (data[1] * 0.1f);
        *temperature = data[2] + (data[3] * 0.1f);

        // Aplikácia kalibračného ofsetu (Wu Wei - priamo na zdroji)
        *temperature += app_config_get()->dht_temp_offset;

        return true;
    }
    ESP_LOGE(TAG, "Chyba: Nesedí kontrolný súčet (Checksum)!");
    return false;
}

static void sensor_task(void* arg) {
    ESP_LOGI(TAG, "Sensor Task spustený (Živé meranie: 5s, Logovanie: 10m)");
    int log_counter = 0;

    while (1) {
        float t = 0, h = 0;
        if (sensor_core_read_dht11(&t, &h)) {
            s_last_t = t;
            s_last_h = h;
        }

        // Každých 10 minút (120 x 5 sekúnd) zapíšeme do LittleFS
        log_counter++;
        if (log_counter >= 120) {
            time_t now;
            time(&now);

            // UNIX Timestamp > 1600000000 znamená, že SNTP je už
            // zosynchronizované (po roku 2020)
            if (now > 1600000000) {
                storage_log_sensor_data((uint32_t)now, s_last_t, s_last_h,
                                        1013.0);
                ESP_LOGI(TAG, "Dáta uložené do internej pamäte. Timestamp: %lu",
                         (unsigned long)now);
            } else {
                ESP_LOGW(TAG,
                         "Čas ešte nie je synchronizovaný, preskakujem zápis "
                         "do logu (prevencia poškodenia Delta Sync).");
            }

            log_counter = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void sensor_core_start_task() {
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
}

void sensor_core_get_latest(float* t, float* h) {
    *t = s_last_t;
    *h = s_last_h;
}