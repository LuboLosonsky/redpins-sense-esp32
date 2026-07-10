#include <stdio.h>
#include <time.h>

#include "app_config.h"
#include "ble_server.h"
#include "display_hal.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "power_manager.h"
#include "sensor_core.h"
#include "storage.h"
#include "weather_client.h"
#include "wifi_scanner.h"

static const char* TAG = "REDPINS_CORE";

#define LCD_BLK_PIN (gpio_num_t) PIN_NUM_BCKL  // Prepojené na display_hal.h

extern esp_lcd_panel_handle_t panel_handle;  // Natiahnutie handlu z display_hal

extern "C" void gui_task(void* arg);

// --- MODE_LONG_LIFE: udrzba dat pocas periodickeho wake bez tlacidla ---
// RTC_DATA_ATTR prezije deep sleep (RTC domena ostava napajana), takze si
// pamatame posledny zaznam aj cez opakovane resety bez nutnosti pri kazdom
// 500ms wake citat CSV zo suborovehoy systemu.
RTC_DATA_ATTR static time_t s_rtc_last_sensor_log_ts = 0;
RTC_DATA_ATTR static time_t s_rtc_last_weather_fetch_ts = 0;

// Weather API fetch stoji WiFi asociaciu + HTTP request - jednu z
// energeticky najnarocnejsich operacii na ESP32. V Long Life ho preto
// robime len raz za hodinu (namiesto 20 min ako v Performance/Balanced),
// aby sme vyrazne nezhorsili uspornost rezimu.
#define LONG_LIFE_WEATHER_FETCH_INTERVAL_S (60 * 60)
#define LONG_LIFE_WIFI_CONNECT_TIMEOUT_MS 6000

// Volane len z vetvy "periodicky Long Life wake, ziadne tlacidlo drzane" -
// displej/BLE zostavaju vypnute, robime len to, co data vyzaduju.
static void long_life_maintenance_cycle(void) {
    time_t now;
    time(&now);
    if (now < 1600000000) return;  // Cas nikdy nebol NTP-synchronizovany

    bool sensor_due = (uint32_t)(now - s_rtc_last_sensor_log_ts) >=
                       (SENSOR_LOG_INTERVAL_MS / 1000);
    bool weather_due = (uint32_t)(now - s_rtc_last_weather_fetch_ts) >=
                        LONG_LIFE_WEATHER_FETCH_INTERVAL_S;

    if (!sensor_due && !weather_due) return;

    if (storage_init() != ESP_OK) return;
    app_config_init();

    if (sensor_due) {
        sensor_core_init();
        float t = 0.0f, h = 0.0f, p = 0.0f;
        if (sensor_core_read_bme280(&t, &h, &p)) {
            storage_log_sensor_data((uint32_t)now, t, h, p);
            s_rtc_last_sensor_log_ts = now;
            ESP_LOGI(TAG,
                     "Long Life udrzba: zaznam senzora ulozeny (%.1fC %.1f%% "
                     "%.0fhPa)",
                     t, h, p);
        }
    }

    if (weather_due) {
        ESP_LOGI(TAG, "Long Life udrzba: pokus o hodinovy weather fetch...");
        wifi_scanner_auto_connect();

        uint32_t waited_ms = 0;
        while (!wifi_scanner_is_connected() &&
               waited_ms < LONG_LIFE_WIFI_CONNECT_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(300));
            waited_ms += 300;
        }

        if (wifi_scanner_is_connected()) {
            weather_client_init();  // Prvy fetch v tasku prebehne takmer
                                     // okamzite (last_fetch_min == -1)
            vTaskDelay(pdMS_TO_TICKS(8000));  // Cas na dokoncenie HTTP requestu
            s_rtc_last_weather_fetch_ts = now;
        } else {
            ESP_LOGW(TAG,
                     "Long Life udrzba: WiFi sa nepripojilo, weather fetch "
                     "preskoceny");
        }

        esp_wifi_stop();
    }
}

extern "C" void app_main(void) {
    // 1. Inicializácia NVS (Kritické pre ukladanie kalibrácie, WiFi a
    // fungovanie BLE) - musi bezat pred power_manager_init().
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Nacitanie ulozeneho napajacieho profilu, urcenie fast-wake z deep
    // sleep (musi byt hned po NVS, pred ostatnou inicializaciou).
    power_manager_init();

    // MODE_LONG_LIFE: ak ide o rutinny periodicky wake na casovac a ziadne
    // tlacidlo nie je drzane, urobime len nevyhnutnu udrzbu dat (sensor
    // log/weather fetch podla intervalu) a vratime sa spat do spanku -
    // displej sa vobec nezapne. Funkcia nizsie sa v tom pripade nikdy
    // nevrati.
    if (!power_manager_check_button_wake()) {
        long_life_maintenance_cycle();
        power_manager_force_long_life_sleep();  // Nikdy sa nevrati
    }

    // Zanshin: Pri studenom starte pockame 3 sekundy, kym Windows pripoji
    // COM port. Pri fast-wake (tlacidlo skutocne drzane) na to necakame -
    // rychlost reakcie je tu dolezitejsia nez skore boot logy.
    if (!power_manager_is_fast_wake()) {
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    ESP_LOGI(TAG, "=====================================================");
    ESP_LOGI(TAG, "========== REDPINS SENSE INIT ZACINA ==============");
    ESP_LOGI(TAG, "=====================================================");

    // Zanshin oprava: predoslý komentár tu tvrdil, že GPIO8 je vyhradený
    // pre I2C SDA - nepravda, realne I2C piny su GPIO0/1 (sensor_core.cpp).
    // GPIO8 je volny a od teraz ho pouziva RGB LED (rgb_led.cpp).

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
    power_manager_on_display_ready();

    // 4. Inicializácia senzorov (BME280/BH1750 diagnostika na I2C)
    sensor_core_init();

    // Zanshin: Odstránili sme synchrónne testovacie kreslenie v main(),
    // pretože preťažovalo asynchrónnu DMA frontu. Vykresľovanie od
    // tohto momentu obsluhuje výhradne gui_task s dodržaním časovania.

    // 5. Inicializácia BLE (NimBLE stack) - Expozícia RCP v2.1
    // Fast-wake z MODE_LONG_LIFE preskakuje BLE/WiFi/weather - zariadenie sa
    // len kratko rozsvieti na lokalnu interakciu, bez radiovej rezie.
    if (!power_manager_is_fast_wake()) {
        ble_server_init();
    }

    // 6. Registrácia FreeRTOS taskov
    sensor_core_start_task();  // vzdy - GUI potrebuje zive lokalne data

    if (!power_manager_is_fast_wake()) {
        weather_client_init();
    } else {
        // Bez WiFi nema zmysel spustat weather_fetch_task, ale posledne
        // znama hodnota z disku (weather.csv) sa da ukazat bez radia.
        weather_client_load_last_known();
    }

    xTaskCreate(gui_task, "gui_task", 4096, NULL, 2, NULL);

    // 7. Pokus o automatické pripojenie na známu Wi-Fi sieť z config.json
    // Poznámka: Pôvodné volanie wifi_scanner_auto_connect() by malo byť
    // nahradené logikou, ktorá číta g_app_config.wifi_ssid a
    // g_app_config.wifi_password.
    if (!power_manager_is_fast_wake()) {
        wifi_scanner_auto_connect();  // <- Refaktorovať na použitie app_config
    }

    // 8. Hlavná slučka (Wu Wei - žiadna práca navyše, uvoľnenie prostriedkov)
    while (true) {
        // Systém prechádza do riadenia cez FreeRTOS eventy a prerušenia
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}