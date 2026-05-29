#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char* TAG = "REDPINS_CORE";

extern "C" void app_main(void) {
    // 1. Inicializácia NVS (Kritické pre ukladanie kalibrácie, WiFi a
    // fungovanie BLE)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "NVS Inicializované. Redpins Sense C6 (RCP v2.1) bootuje.");

    // TODO: 2. Mount LittleFS pre perzistenciu (config.json, CSV logy)

    // TODO: 3. Hardvérová abstrakcia (SPI pre LCD, RMT pre RGB LED, I2C/SPI pre
    // senzory)

    // TODO: 4. Inicializácia BLE (NimBLE stack) - Expozícia A101-A104
    // charakteristík

    // 5. Registrácia FreeRTOS taskov
    /*
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
    xTaskCreate(gui_task, "gui_task", 4096, NULL, 2, NULL);
    */

    // 6. Hlavná slučka (Wu Wei - žiadna práca navyše, uvoľnenie prostriedkov)
    while (true) {
        // Systém prechádza do riadenia cez FreeRTOS eventy a prerušenia
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}