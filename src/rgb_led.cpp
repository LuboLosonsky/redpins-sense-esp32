#include "rgb_led.h"

#include "esp_log.h"
#include "led_strip.h"

static const char* TAG = "RGB_LED";

#define RGB_LED_GPIO 8

// Jas drzany nizko zamerne - ide o ambientny indikator, nie osvetlenie.
// BALANCED je tlmenejsia (konzistentne s "eco" filozofiou rezimu), aj ked
// samotna LED pri tejto sile prakticky nema merateľny vplyv na vydrz.
#define RGB_PERFORMANCE_R 0
#define RGB_PERFORMANCE_G 0
#define RGB_PERFORMANCE_B 40

#define RGB_BALANCED_R 20
#define RGB_BALANCED_G 10
#define RGB_BALANCED_B 0

static led_strip_handle_t s_led_strip = NULL;
static PowerMode s_current_mode = MODE_PERFORMANCE;
static uint8_t s_brightness_percent = 100;

static uint8_t scale(uint8_t base_val) {
    return (uint8_t)(((uint16_t)base_val * s_brightness_percent) / 100);
}

// Prekresli LED podla s_current_mode + s_brightness_percent - spolocny
// vykonavaci bod pre rgb_led_set_mode_indicator aj _set_brightness_percent.
static void apply_current_color(void) {
    if (!s_led_strip) return;

    if (s_current_mode == MODE_PERFORMANCE) {
        led_strip_set_pixel(s_led_strip, 0, scale(RGB_PERFORMANCE_R),
                            scale(RGB_PERFORMANCE_G), scale(RGB_PERFORMANCE_B));
    } else if (s_current_mode == MODE_BALANCED) {
        led_strip_set_pixel(s_led_strip, 0, scale(RGB_BALANCED_R),
                            scale(RGB_BALANCED_G), scale(RGB_BALANCED_B));
    } else {
        led_strip_set_pixel(s_led_strip, 0, 0, 0, 0);
    }
    led_strip_refresh(s_led_strip);
}

void rgb_led_init(void) {
    if (s_led_strip) return;  // uz inicializovane

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = RGB_LED_GPIO;
    strip_config.max_leds = 1;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.resolution_hz = 10 * 1000 * 1000;  // 10MHz
    rmt_config.flags.with_dma = false;

    esp_err_t err =
        led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device zlyhalo: %s",
                 esp_err_to_name(err));
        s_led_strip = NULL;
        return;
    }

    led_strip_clear(s_led_strip);
    ESP_LOGI(TAG, "RGB LED inicializovana (GPIO%d)", RGB_LED_GPIO);
}

void rgb_led_set_mode_indicator(PowerMode mode) {
    s_current_mode = mode;
    apply_current_color();
}

void rgb_led_set_brightness_percent(uint8_t percent) {
    if (percent > 100) percent = 100;
    s_brightness_percent = percent;
    apply_current_color();
}

uint8_t rgb_led_get_brightness_percent(void) { return s_brightness_percent; }

void rgb_led_off(void) {
    if (!s_led_strip) return;
    led_strip_clear(s_led_strip);
}
