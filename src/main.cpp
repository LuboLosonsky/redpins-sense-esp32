#include <stdio.h>

#include "app_config.h"
#include "ble_server.h"
#include "display_hal.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sensor_core.h"
#include "storage.h"
#include "weather_client.h"
#include "wifi_scanner.h"

static const char* TAG = "REDPINS_CORE";

#define LCD_BLK_PIN (gpio_num_t) PIN_NUM_BCKL  // Prepojené na display_hal.h

extern esp_lcd_panel_handle_t panel_handle;  // Natiahnutie handlu z display_hal

extern "C" void gui_task(void* arg);

extern "C" void app_main(void) {
    // Zanshin: Počkáme 3 sekundy, kým Windows pripojí COM port
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "=====================================================");
    ESP_LOGI(TAG, "========== REDPINS SENSE INIT ZACINA ==============");
    ESP_LOGI(TAG, "=====================================================");

    // GPIO8 je vyhradeny pre I2C SDA, preto ho nepouzivame ako vystup pre LED.

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

    // 2. Mount LittleFS pre perzistenciu (config.json, CSV logy)
    if (storage_init() != ESP_OK) {
        ESP_LOGE(TAG,
                 "Kritická chyba: Storage sa nepodarilo spustiť. Systém môže "
                 "byť nestabilný.");
        // Zámerne nezastavujeme beh, BLE stack sa musí spustiť pre diagnostiku
    }

    // Načítanie aplikačnej konfigurácie (alias, GPS, kalibrácia, UI) z
    // config.json
    app_config_init();

    // 3. Hardvérová abstrakcia (SPI pre LCD)
    display_hal_init();

    // 4. Inicializácia senzorov (BME280/BH1750 diagnostika na I2C)
    sensor_core_init();

    // Zanshin: Odstránili sme synchrónne testovacie kreslenie v main(),
    // pretože preťažovalo asynchrónnu DMA frontu. Vykresľovanie od
    // tohto momentu obsluhuje výhradne gui_task s dodržaním časovania.

    // 5. Inicializácia BLE (NimBLE stack) - Expozícia RCP v2.1
    ble_server_init();

    // 6. Registrácia FreeRTOS taskov
    sensor_core_start_task();
    weather_client_init();

    xTaskCreate(gui_task, "gui_task", 4096, NULL, 2, NULL);

    // 7. Pokus o automatické pripojenie na známu Wi-Fi sieť z config.json
    // Poznámka: Pôvodné volanie wifi_scanner_auto_connect() by malo byť
    // nahradené logikou, ktorá číta g_app_config.wifi_ssid a
    // g_app_config.wifi_password.
    wifi_scanner_auto_connect();  // <- Refaktorovať na použitie app_config

    // 8. Hlavná slučka (Wu Wei - žiadna práca navyše, uvoľnenie prostriedkov)
    while (true) {
        // Systém prechádza do riadenia cez FreeRTOS eventy a prerušenia
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}