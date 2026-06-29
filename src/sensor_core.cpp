#include "sensor_core.h"

#include <stdint.h>
#include <time.h>

#include "app_config.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage.h"

static const char* TAG = "SENSOR_CORE";

#define I2C_PORT I2C_NUM_0
#define I2C_SDA_PIN GPIO_NUM_0
#define I2C_SCL_PIN GPIO_NUM_1
#define I2C_FREQ_HZ 100000
#define I2C_FALLBACK_FREQ_HZ 10000
#define ENABLE_BH1750 1
#define SENSOR_CORE_DEBUG_I2C 0
#define SENSOR_MEASURE_INTERVAL_MS 1000
// Zanshin: BME280 ma jemnejsie rozlisenie ako stary DHT11, jeho ADC sum
// (+-0.2..0.3 C / %RH medzi vzorkami) by inak kazdu sekundu rozhybal
// hodnotu a sposoboval zbytocny redraw displeja. EMA vyhladi sum priamo
// pri zdroji (displej, CSV log aj BLE telemetria dostanu uz cistu hodnotu).
#define SENSOR_EMA_ALPHA 0.1f

#define BME280_ADDR_1 0x76
#define BME280_ADDR_2 0x77
#define BH1750_ADDR_1 0x23
#define BH1750_ADDR_2 0x5C

#define BME280_REG_ID 0xD0
#define BME280_CHIP_ID 0x60
#define BME280_REG_RESET 0xE0
#define BME280_RESET_CMD 0xB6
#define BME280_REG_CTRL_HUM 0xF2
#define BME280_REG_STATUS 0xF3
#define BME280_REG_CTRL_MEAS 0xF4
#define BME280_REG_CONFIG 0xF5
#define BME280_REG_PRESS_MSB 0xF7

#define BH1750_CMD_CONT_HI_RES 0x10
#define BH1750_CMD_ONE_TIME_HI_RES 0x20
#define BH1750_MEAS_TIME_MS 180

static uint8_t s_bme_addr = 0;
static uint8_t s_bh_addr = 0;
static gpio_num_t s_i2c_sda = I2C_SDA_PIN;
static gpio_num_t s_i2c_scl = I2C_SCL_PIN;
static uint32_t s_i2c_freq_hz = I2C_FREQ_HZ;
static bool s_i2c_driver_installed = false;
static bool s_bme_use_bitbang = false;
static bool s_i2c_use_bitbang_bus = false;
static bool s_weather_sensor_ok = false;
static uint32_t s_bh_invalid_samples = 0;

#if SENSOR_CORE_DEBUG_I2C
#define I2C_DBGI(...) ESP_LOGI(TAG, __VA_ARGS__)
#define I2C_DBGW(...) ESP_LOGW(TAG, __VA_ARGS__)
#else
#define I2C_DBGI(...)                   \
    do {                                \
        if (0) {                        \
            ESP_LOGI(TAG, __VA_ARGS__); \
        }                               \
    } while (0)
#define I2C_DBGW(...)                   \
    do {                                \
        if (0) {                        \
            ESP_LOGW(TAG, __VA_ARGS__); \
        }                               \
    } while (0)
#endif

static float s_last_t = 0.0f;
static float s_last_h = 0.0f;
static float s_last_p = 1013.0f;
static float s_last_lux = 0.0f;

static int32_t s_t_fine = 0;

static esp_err_t i2c_read_reg(uint8_t dev_addr, uint8_t reg, uint8_t* data,
                              size_t len);

static esp_err_t i2c_read_device_bytes(uint8_t dev_addr, uint8_t* data,
                                       size_t len);

typedef struct {
    uint16_t dig_T1;
    int16_t dig_T2;
    int16_t dig_T3;
    uint16_t dig_P1;
    int16_t dig_P2;
    int16_t dig_P3;
    int16_t dig_P4;
    int16_t dig_P5;
    int16_t dig_P6;
    int16_t dig_P7;
    int16_t dig_P8;
    int16_t dig_P9;
    uint8_t dig_H1;
    int16_t dig_H2;
    uint8_t dig_H3;
    int16_t dig_H4;
    int16_t dig_H5;
    int8_t dig_H6;
} bme280_calib_data_t;

static bme280_calib_data_t s_calib = {};

static void diagnose_gpio_wiggle(gpio_num_t sda, gpio_num_t scl,
                                 const char* label) {
#if !SENSOR_CORE_DEBUG_I2C
    (void)sda;
    (void)scl;
    (void)label;
    return;
#else
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    gpio_set_pull_mode(sda, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(scl, GPIO_PULLUP_ONLY);
    gpio_set_direction(sda, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction(scl, GPIO_MODE_INPUT_OUTPUT_OD);

    // Uvolnene = HIGH cez pull-up.
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    int hh_sda = gpio_get_level(sda);
    int hh_scl = gpio_get_level(scl);

    // SDA LOW, SCL HIGH.
    gpio_set_level(sda, 0);
    gpio_set_level(scl, 1);
    vTaskDelay(pdMS_TO_TICKS(2));
    int lh_sda = gpio_get_level(sda);
    int lh_scl = gpio_get_level(scl);

    // SDA HIGH, SCL LOW.
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    int hl_sda = gpio_get_level(sda);
    int hl_scl = gpio_get_level(scl);

    // Obe LOW.
    gpio_set_level(sda, 0);
    gpio_set_level(scl, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    int ll_sda = gpio_get_level(sda);
    int ll_scl = gpio_get_level(scl);

    int debug_sink =
        hh_sda + hh_scl + lh_sda + lh_scl + hl_sda + hl_scl + ll_sda + ll_scl;
    (void)debug_sink;

    auto measure_rise_us_local = [&](gpio_num_t pin) {
        gpio_set_level(pin, 0);
        esp_rom_delay_us(50);
        gpio_set_level(pin, 1);  // open-drain release

        const uint32_t max_wait_us = 500;
        for (uint32_t us = 0; us < max_wait_us; ++us) {
            if (gpio_get_level(pin) == 1) return us;
            esp_rom_delay_us(1);
        }
        return max_wait_us;
    };

    // Vratenie do uvolneneho stavu.
    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);

    uint32_t rise_sda_us = measure_rise_us_local(sda);
    uint32_t rise_scl_us = measure_rise_us_local(scl);

    gpio_set_direction(sda, GPIO_MODE_INPUT);
    gpio_set_direction(scl, GPIO_MODE_INPUT);

    I2C_DBGI("GPIO wiggle (%s): HH[%d,%d] LH[%d,%d] HL[%d,%d] LL[%d,%d]", label,
             hh_sda, hh_scl, lh_sda, lh_scl, hl_sda, hl_scl, ll_sda, ll_scl);
    I2C_DBGI("GPIO rise (%s): SDA=%luus SCL=%luus", label,
             (unsigned long)rise_sda_us, (unsigned long)rise_scl_us);

    if (lh_scl == 0 || hl_sda == 0) {
        I2C_DBGW("GPIO wiggle (%s): mozne prepojenie/skrat medzi SDA a SCL",
                 label);
    }
    if (rise_sda_us >= 200 || rise_scl_us >= 200) {
        I2C_DBGW(
            "GPIO rise (%s): velmi slaby/chybajuci pull-up (odporucam "
            "externy 4.7k)",
            label);
    }
#endif
}

static void diagnose_line_levels(gpio_num_t sda, gpio_num_t scl,
                                 const char* stage) {
    gpio_set_direction(sda, GPIO_MODE_INPUT);
    gpio_set_direction(scl, GPIO_MODE_INPUT);
    gpio_set_pull_mode(sda, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(scl, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(2));

    int sda_lvl = gpio_get_level(sda);
    int scl_lvl = gpio_get_level(scl);
    I2C_DBGI("I2C line state (%s): SDA=%d SCL=%d", stage, sda_lvl, scl_lvl);
    if (sda_lvl == 0 || scl_lvl == 0) {
        I2C_DBGW("I2C line low (%s): SDA=%d SCL=%d (mozne skraty/pin conflict)",
                 stage, sda_lvl, scl_lvl);
    }
}

static void i2c_force_recover_bus(gpio_num_t sda, gpio_num_t scl) {
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    gpio_set_pull_mode(sda, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(scl, GPIO_PULLUP_ONLY);
    gpio_set_direction(sda, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction(scl, GPIO_MODE_INPUT_OUTPUT_OD);

    gpio_set_level(sda, 1);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(10);

    // 9 clocks uvolni stuck slave, potom explicitny STOP.
    for (int i = 0; i < 9; ++i) {
        gpio_set_level(scl, 0);
        esp_rom_delay_us(5);
        gpio_set_level(scl, 1);
        esp_rom_delay_us(5);
    }

    gpio_set_level(sda, 0);
    esp_rom_delay_us(5);
    gpio_set_level(scl, 1);
    esp_rom_delay_us(5);
    gpio_set_level(sda, 1);
    esp_rom_delay_us(5);

    gpio_set_direction(sda, GPIO_MODE_INPUT);
    gpio_set_direction(scl, GPIO_MODE_INPUT);
}

static bool i2c_setup_bus(gpio_num_t sda, gpio_num_t scl, uint32_t freq_hz,
                          const char* label) {
    if (s_i2c_driver_installed) {
        i2c_driver_delete(I2C_PORT);
        s_i2c_driver_installed = false;
    }

    if (SENSOR_CORE_DEBUG_I2C) {
        diagnose_gpio_wiggle(sda, scl, label ? label : "pre-i2c-setup");
    }
    i2c_force_recover_bus(sda, scl);

    if (SENSOR_CORE_DEBUG_I2C) {
        diagnose_line_levels(sda, scl, "pre-init");
    }

    i2c_config_t i2c_cfg = {};
    i2c_cfg.mode = I2C_MODE_MASTER;
    i2c_cfg.sda_io_num = sda;
    i2c_cfg.scl_io_num = scl;
    i2c_cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_cfg.master.clk_speed = freq_hz;

    if (i2c_param_config(I2C_PORT, &i2c_cfg) != ESP_OK) return false;
    if (i2c_driver_install(I2C_PORT, i2c_cfg.mode, 0, 0, 0) != ESP_OK)
        return false;

    s_i2c_driver_installed = true;

    s_i2c_sda = sda;
    s_i2c_scl = scl;
    s_i2c_freq_hz = freq_hz;
    ESP_LOGI(TAG, "I2C init: SDA=%d, SCL=%d, FREQ=%lu", s_i2c_sda, s_i2c_scl,
             (unsigned long)s_i2c_freq_hz);
    if (SENSOR_CORE_DEBUG_I2C) {
        diagnose_line_levels(sda, scl, "post-init");
    }
    return true;
}

static void sensor_boot_selftest() {
    ESP_LOGI(TAG, "SELFTEST: I2C map BME=0x%02X BH=0x%02X", s_bme_addr,
             s_bh_addr);

    if (s_bme_addr != 0) {
        uint8_t chip_id = 0;
        if (i2c_read_reg(s_bme_addr, BME280_REG_ID, &chip_id, 1) == ESP_OK) {
            ESP_LOGI(TAG, "SELFTEST: BME280 chip_id=0x%02X", chip_id);
        } else {
            ESP_LOGW(TAG, "SELFTEST: BME280 chip_id read failed");
        }
    }

    float t = 0.0f, h = 0.0f, p = 0.0f, lux = 0.0f;
    bool bme_ok = false;
    bool bh_ok = false;

    for (int i = 0; i < 3 && !bme_ok; ++i) {
        bme_ok = sensor_core_read_bme280(&t, &h, &p);
        if (!bme_ok) vTaskDelay(pdMS_TO_TICKS(40));
    }
    for (int i = 0; i < 3 && !bh_ok; ++i) {
#if ENABLE_BH1750
        bh_ok = sensor_core_read_bh1750(&lux);
#else
        bh_ok = false;
#endif
        if (!bh_ok) vTaskDelay(pdMS_TO_TICKS(40));
    }

    if (bme_ok) {
        s_last_t = t;
        s_last_h = h;
        s_last_p = p;
    }
    if (bh_ok) {
        s_last_lux = lux;
    }

    ESP_LOGI(TAG,
             "SELFTEST: BME=%s BH=%s | T=%.1fC H=%.1f%% P=%.1fhPa Lux=%.1f",
             bme_ok ? "OK" : "FAIL",
#if ENABLE_BH1750
             bh_ok ? "OK" : "FAIL",
#else
             "DISABLED",
#endif
             s_last_t, s_last_h, s_last_p, s_last_lux);
}

static esp_err_t i2c_write_bytes(uint8_t dev_addr, const uint8_t* data,
                                 size_t len) {
    if (s_i2c_use_bitbang_bus) {
        auto bb_delay = []() { esp_rom_delay_us(3); };
        auto set_sda = [&](int lvl) {
            gpio_set_level(s_i2c_sda, lvl ? 1 : 0);
            bb_delay();
        };
        auto set_scl = [&](int lvl) {
            gpio_set_level(s_i2c_scl, lvl ? 1 : 0);
            bb_delay();
        };
        auto write_byte = [&](uint8_t b) {
            for (int i = 7; i >= 0; --i) {
                set_sda((b >> i) & 0x01);
                set_scl(1);
                set_scl(0);
            }
            set_sda(1);
            set_scl(1);
            bool ack = (gpio_get_level(s_i2c_sda) == 0);
            set_scl(0);
            return ack;
        };

        if (!data || len == 0) return ESP_ERR_INVALID_ARG;

        gpio_set_direction(s_i2c_sda, GPIO_MODE_INPUT_OUTPUT_OD);
        gpio_set_direction(s_i2c_scl, GPIO_MODE_INPUT_OUTPUT_OD);

        set_sda(1);
        set_scl(1);
        set_sda(0);
        set_scl(0);

        bool ok = write_byte((uint8_t)((dev_addr << 1) | 0x00));
        for (size_t i = 0; i < len && ok; ++i) {
            ok = write_byte(data[i]);
        }

        set_sda(0);
        set_scl(1);
        set_sda(1);
        gpio_set_direction(s_i2c_sda, GPIO_MODE_INPUT);
        gpio_set_direction(s_i2c_scl, GPIO_MODE_INPUT);
        return ok ? ESP_OK : ESP_FAIL;
    }

    return i2c_master_write_to_device(I2C_PORT, dev_addr, data, len,
                                      pdMS_TO_TICKS(100));
}

static esp_err_t i2c_write_reg_u8(uint8_t dev_addr, uint8_t reg, uint8_t val) {
    uint8_t payload[2] = {reg, val};
    return i2c_write_bytes(dev_addr, payload, sizeof(payload));
}

static esp_err_t i2c_read_reg(uint8_t dev_addr, uint8_t reg, uint8_t* data,
                              size_t len) {
    if (s_i2c_use_bitbang_bus) {
        auto bb_delay = []() { esp_rom_delay_us(3); };
        auto set_sda = [&](int lvl) {
            gpio_set_level(s_i2c_sda, lvl ? 1 : 0);
            bb_delay();
        };
        auto set_scl = [&](int lvl) {
            gpio_set_level(s_i2c_scl, lvl ? 1 : 0);
            bb_delay();
        };
        auto write_byte = [&](uint8_t b) {
            for (int i = 7; i >= 0; --i) {
                set_sda((b >> i) & 0x01);
                set_scl(1);
                set_scl(0);
            }
            set_sda(1);
            set_scl(1);
            bool ack = (gpio_get_level(s_i2c_sda) == 0);
            set_scl(0);
            return ack;
        };
        auto read_byte = [&](bool master_ack) {
            uint8_t b = 0;
            set_sda(1);
            for (int i = 7; i >= 0; --i) {
                set_scl(1);
                if (gpio_get_level(s_i2c_sda)) b |= (uint8_t)(1U << i);
                set_scl(0);
            }
            set_sda(master_ack ? 0 : 1);
            set_scl(1);
            set_scl(0);
            set_sda(1);
            return b;
        };

        if (!data || len == 0) {
            gpio_set_direction(s_i2c_sda, GPIO_MODE_INPUT);
            gpio_set_direction(s_i2c_scl, GPIO_MODE_INPUT);
            return ESP_ERR_INVALID_ARG;
        }

        gpio_set_direction(s_i2c_sda, GPIO_MODE_INPUT_OUTPUT_OD);
        gpio_set_direction(s_i2c_scl, GPIO_MODE_INPUT_OUTPUT_OD);

        // START + write register address
        set_sda(1);
        set_scl(1);
        set_sda(0);
        set_scl(0);

        bool ok =
            write_byte((uint8_t)((dev_addr << 1) | 0x00)) && write_byte(reg);

        // Repeated START + read
        set_sda(1);
        set_scl(1);
        set_sda(0);
        set_scl(0);

        ok = ok && write_byte((uint8_t)((dev_addr << 1) | 0x01));
        if (ok) {
            for (size_t i = 0; i < len; ++i) {
                data[i] = read_byte((i + 1) < len);
            }
        }

        // STOP
        set_sda(0);
        set_scl(1);
        set_sda(1);
        gpio_set_direction(s_i2c_sda, GPIO_MODE_INPUT);
        gpio_set_direction(s_i2c_scl, GPIO_MODE_INPUT);
        return ok ? ESP_OK : ESP_FAIL;
    }

    return i2c_master_write_read_device(I2C_PORT, dev_addr, &reg, 1, data, len,
                                        pdMS_TO_TICKS(100));
}

static esp_err_t i2c_read_device_bytes(uint8_t dev_addr, uint8_t* data,
                                       size_t len) {
    if (s_i2c_use_bitbang_bus) {
        auto bb_delay = []() { esp_rom_delay_us(3); };
        auto set_sda = [&](int lvl) {
            gpio_set_level(s_i2c_sda, lvl ? 1 : 0);
            bb_delay();
        };
        auto set_scl = [&](int lvl) {
            gpio_set_level(s_i2c_scl, lvl ? 1 : 0);
            bb_delay();
        };
        auto write_byte = [&](uint8_t b) {
            for (int i = 7; i >= 0; --i) {
                set_sda((b >> i) & 0x01);
                set_scl(1);
                set_scl(0);
            }
            set_sda(1);
            set_scl(1);
            bool ack = (gpio_get_level(s_i2c_sda) == 0);
            set_scl(0);
            return ack;
        };
        auto read_byte = [&](bool master_ack) {
            uint8_t b = 0;
            set_sda(1);
            for (int i = 7; i >= 0; --i) {
                set_scl(1);
                if (gpio_get_level(s_i2c_sda)) b |= (uint8_t)(1U << i);
                set_scl(0);
            }
            set_sda(master_ack ? 0 : 1);
            set_scl(1);
            set_scl(0);
            set_sda(1);
            return b;
        };

        if (!data || len == 0) return ESP_ERR_INVALID_ARG;

        gpio_set_direction(s_i2c_sda, GPIO_MODE_INPUT_OUTPUT_OD);
        gpio_set_direction(s_i2c_scl, GPIO_MODE_INPUT_OUTPUT_OD);

        set_sda(1);
        set_scl(1);
        set_sda(0);
        set_scl(0);

        bool ok = write_byte((uint8_t)((dev_addr << 1) | 0x01));
        if (ok) {
            for (size_t i = 0; i < len; ++i) {
                data[i] = read_byte((i + 1) < len);
            }
        }

        set_sda(0);
        set_scl(1);
        set_sda(1);
        gpio_set_direction(s_i2c_sda, GPIO_MODE_INPUT);
        gpio_set_direction(s_i2c_scl, GPIO_MODE_INPUT);
        return ok ? ESP_OK : ESP_FAIL;
    }

    return i2c_master_read_from_device(I2C_PORT, dev_addr, data, len,
                                       pdMS_TO_TICKS(100));
}

static bool probe_device_ex_timeout(uint8_t addr, esp_err_t* out_err,
                                    TickType_t timeout_ticks) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_PORT, cmd, timeout_ticks);
    i2c_cmd_link_delete(cmd);
    if (out_err) *out_err = err;
    return err == ESP_OK;
}

static bool probe_device_ex(uint8_t addr, esp_err_t* out_err) {
    return probe_device_ex_timeout(addr, out_err, pdMS_TO_TICKS(100));
}

static bool bitbang_probe_addr(gpio_num_t sda, gpio_num_t scl, uint8_t addr) {
    gpio_reset_pin(sda);
    gpio_reset_pin(scl);
    gpio_set_pull_mode(sda, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(scl, GPIO_PULLUP_ONLY);
    gpio_set_direction(sda, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction(scl, GPIO_MODE_INPUT_OUTPUT_OD);

    auto bb_delay = []() { esp_rom_delay_us(3); };
    auto set_sda = [&](int lvl) {
        gpio_set_level(sda, lvl ? 1 : 0);
        bb_delay();
    };
    auto set_scl = [&](int lvl) {
        gpio_set_level(scl, lvl ? 1 : 0);
        bb_delay();
    };

    // Idle high.
    set_sda(1);
    set_scl(1);

    // START.
    set_sda(0);
    set_scl(0);

    uint8_t byte = (uint8_t)((addr << 1) | 0x00);
    for (int i = 7; i >= 0; --i) {
        set_sda((byte >> i) & 0x01);
        set_scl(1);
        set_scl(0);
    }

    // ACK bit: release SDA and sample while SCL high.
    set_sda(1);
    set_scl(1);
    int ack = (gpio_get_level(sda) == 0) ? 1 : 0;
    set_scl(0);

    // STOP.
    set_sda(0);
    set_scl(1);
    set_sda(1);

    gpio_set_direction(sda, GPIO_MODE_INPUT);
    gpio_set_direction(scl, GPIO_MODE_INPUT);
    return ack == 1;
}

static bool i2c_reinstall_current_bus() {
    if (s_i2c_driver_installed) {
        i2c_driver_delete(I2C_PORT);
        s_i2c_driver_installed = false;
    }

    i2c_config_t i2c_cfg = {};
    i2c_cfg.mode = I2C_MODE_MASTER;
    i2c_cfg.sda_io_num = s_i2c_sda;
    i2c_cfg.scl_io_num = s_i2c_scl;
    i2c_cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_cfg.master.clk_speed = s_i2c_freq_hz;

    if (i2c_param_config(I2C_PORT, &i2c_cfg) != ESP_OK) return false;
    if (i2c_driver_install(I2C_PORT, i2c_cfg.mode, 0, 0, 0) != ESP_OK)
        return false;

    s_i2c_driver_installed = true;
    if (SENSOR_CORE_DEBUG_I2C) {
        diagnose_line_levels(s_i2c_sda, s_i2c_scl, "post-bitbang-reinit");
    }
    return true;
}

static uint8_t log_bitbang_bme_probe() {
    // Run only after normal I2C probing fails to separate driver-vs-physical
    // ACK.
    if (s_i2c_driver_installed) {
        i2c_driver_delete(I2C_PORT);
        s_i2c_driver_installed = false;
    }

    i2c_force_recover_bus(s_i2c_sda, s_i2c_scl);
    bool ack76 = bitbang_probe_addr(s_i2c_sda, s_i2c_scl, BME280_ADDR_1);
    bool ack77 = bitbang_probe_addr(s_i2c_sda, s_i2c_scl, BME280_ADDR_2);
    int line_sda = gpio_get_level(s_i2c_sda);
    int line_scl = gpio_get_level(s_i2c_scl);

    ESP_LOGW(TAG, "BITBANG probe BME: 0x76=%s 0x77=%s (lines SDA=%d SCL=%d)",
             ack76 ? "ACK" : "NOACK", ack77 ? "ACK" : "NOACK", line_sda,
             line_scl);

    if (!i2c_reinstall_current_bus()) {
        ESP_LOGE(TAG, "I2C reinstall after bitbang failed");
        return 0;
    }

    if (ack76) return BME280_ADDR_1;
    if (ack77) return BME280_ADDR_2;
    return 0;
}

static void log_i2c_scan_overview() {
    if (!SENSOR_CORE_DEBUG_I2C) return;

    bool any = false;
    int ack_cnt = 0;
    int nack_cnt = 0;
    int timeout_cnt = 0;
    int other_cnt = 0;
    int timeout_diag_budget = 4;
    const TickType_t scan_probe_timeout = pdMS_TO_TICKS(20);

    for (int addr = 0x08; addr <= 0x77; ++addr) {
        esp_err_t err = ESP_FAIL;
        if (probe_device_ex_timeout((uint8_t)addr, &err, scan_probe_timeout)) {
            ESP_LOGI(TAG, "I2C scan: found device at 0x%02X", addr);
            any = true;
            ack_cnt++;
        } else if (err == ESP_ERR_TIMEOUT) {
            timeout_cnt++;

            if (timeout_diag_budget > 0) {
                int pre_sda = gpio_get_level(s_i2c_sda);
                int pre_scl = gpio_get_level(s_i2c_scl);
                ESP_LOGW(TAG,
                         "I2C timeout @0x%02X: line sample A SDA=%d "
                         "SCL=%d",
                         addr, pre_sda, pre_scl);

                // Keep diagnostics non-invasive while the I2C driver is active.
                esp_rom_delay_us(100);

                int post_sda = gpio_get_level(s_i2c_sda);
                int post_scl = gpio_get_level(s_i2c_scl);
                ESP_LOGW(TAG,
                         "I2C timeout @0x%02X: line sample B SDA=%d "
                         "SCL=%d",
                         addr, post_sda, post_scl);

                timeout_diag_budget--;
            }
        } else if (err == ESP_FAIL) {
            nack_cnt++;
        } else {
            other_cnt++;
        }

        // Long timeout-only scans can starve IDLE on C6 and trigger task_wdt.
        if ((addr & 0x07) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    int line_sda = gpio_get_level(s_i2c_sda);
    int line_scl = gpio_get_level(s_i2c_scl);
    ESP_LOGI(TAG,
             "I2C scan stats: ack=%d nack=%d timeout=%d other=%d "
             "pins[SDA=%d SCL=%d] lines[SDA=%d SCL=%d] FREQ=%lu",
             ack_cnt, nack_cnt, timeout_cnt, other_cnt, s_i2c_sda, s_i2c_scl,
             line_sda, line_scl, (unsigned long)s_i2c_freq_hz);

    if (!any) {
        ESP_LOGW(TAG,
                 "I2C scan: no devices found on pins[SDA=%d SCL=%d] "
                 "lines[SDA=%d SCL=%d] FREQ=%lu",
                 s_i2c_sda, s_i2c_scl, line_sda, line_scl,
                 (unsigned long)s_i2c_freq_hz);
    }
}

static void detect_sensors_on_current_bus() {
    s_bme_addr = 0;
    s_bh_addr = 0;
    s_bme_use_bitbang = false;
    s_i2c_use_bitbang_bus = false;

    esp_err_t err_bme1 = ESP_FAIL;
    esp_err_t err_bme2 = ESP_FAIL;
    const char* bh23_status = "DISABLED";
    const char* bh5c_status = "DISABLED";

#if ENABLE_BH1750
    esp_err_t err_bh1 = ESP_FAIL;
    esp_err_t err_bh2 = ESP_FAIL;
#endif

    if (s_i2c_use_bitbang_bus) {
        if (bitbang_probe_addr(s_i2c_sda, s_i2c_scl, BME280_ADDR_1)) {
            s_bme_addr = BME280_ADDR_1;
            err_bme1 = ESP_OK;
        } else if (bitbang_probe_addr(s_i2c_sda, s_i2c_scl, BME280_ADDR_2)) {
            s_bme_addr = BME280_ADDR_2;
            err_bme2 = ESP_OK;
        }
    } else if (probe_device_ex(BME280_ADDR_1, &err_bme1)) {
        s_bme_addr = BME280_ADDR_1;
    } else if (probe_device_ex(BME280_ADDR_2, &err_bme2)) {
        s_bme_addr = BME280_ADDR_2;
    }

#if ENABLE_BH1750
    if (s_i2c_use_bitbang_bus) {
        if (bitbang_probe_addr(s_i2c_sda, s_i2c_scl, BH1750_ADDR_1)) {
            s_bh_addr = BH1750_ADDR_1;
            err_bh1 = ESP_OK;
        } else if (bitbang_probe_addr(s_i2c_sda, s_i2c_scl, BH1750_ADDR_2)) {
            s_bh_addr = BH1750_ADDR_2;
            err_bh2 = ESP_OK;
        }
    } else if (probe_device_ex(BH1750_ADDR_1, &err_bh1)) {
        s_bh_addr = BH1750_ADDR_1;
    } else if (probe_device_ex(BH1750_ADDR_2, &err_bh2)) {
        s_bh_addr = BH1750_ADDR_2;
    }

    bh23_status = esp_err_to_name(err_bh1);
    bh5c_status = esp_err_to_name(err_bh2);
#endif

    ESP_LOGI(TAG, "I2C probe results: BME[0x76=%s,0x77=%s] BH[0x23=%s,0x5C=%s]",
             esp_err_to_name(err_bme1), esp_err_to_name(err_bme2), bh23_status,
             bh5c_status);
}

static bool bme280_read_calibration() {
    uint8_t buf1[26] = {0};
    uint8_t buf2[7] = {0};

    if (i2c_read_reg(s_bme_addr, 0x88, buf1, 24) != ESP_OK) return false;
    if (i2c_read_reg(s_bme_addr, 0xA1, &buf1[24], 1) != ESP_OK) return false;
    if (i2c_read_reg(s_bme_addr, 0xE1, buf2, 7) != ESP_OK) return false;

    s_calib.dig_T1 = (uint16_t)((buf1[1] << 8) | buf1[0]);
    s_calib.dig_T2 = (int16_t)((buf1[3] << 8) | buf1[2]);
    s_calib.dig_T3 = (int16_t)((buf1[5] << 8) | buf1[4]);

    s_calib.dig_P1 = (uint16_t)((buf1[7] << 8) | buf1[6]);
    s_calib.dig_P2 = (int16_t)((buf1[9] << 8) | buf1[8]);
    s_calib.dig_P3 = (int16_t)((buf1[11] << 8) | buf1[10]);
    s_calib.dig_P4 = (int16_t)((buf1[13] << 8) | buf1[12]);
    s_calib.dig_P5 = (int16_t)((buf1[15] << 8) | buf1[14]);
    s_calib.dig_P6 = (int16_t)((buf1[17] << 8) | buf1[16]);
    s_calib.dig_P7 = (int16_t)((buf1[19] << 8) | buf1[18]);
    s_calib.dig_P8 = (int16_t)((buf1[21] << 8) | buf1[20]);
    s_calib.dig_P9 = (int16_t)((buf1[23] << 8) | buf1[22]);

    s_calib.dig_H1 = buf1[24];
    s_calib.dig_H2 = (int16_t)((buf2[1] << 8) | buf2[0]);
    s_calib.dig_H3 = buf2[2];
    s_calib.dig_H4 = (int16_t)((buf2[3] << 4) | (buf2[4] & 0x0F));
    s_calib.dig_H5 = (int16_t)((buf2[5] << 4) | (buf2[4] >> 4));
    s_calib.dig_H6 = (int8_t)buf2[6];

    return true;
}

static int32_t bme280_compensate_temp(int32_t adc_T) {
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)s_calib.dig_T1 << 1))) *
                    ((int32_t)s_calib.dig_T2)) >>
                   11;

    int32_t var2 = (((((adc_T >> 4) - ((int32_t)s_calib.dig_T1)) *
                      ((adc_T >> 4) - ((int32_t)s_calib.dig_T1))) >>
                     12) *
                    ((int32_t)s_calib.dig_T3)) >>
                   14;

    s_t_fine = var1 + var2;
    return (s_t_fine * 5 + 128) >> 8;
}

static uint32_t bme280_compensate_press(int32_t adc_P) {
    int64_t var1 = ((int64_t)s_t_fine) - 128000;
    int64_t var2 = var1 * var1 * (int64_t)s_calib.dig_P6;
    var2 = var2 + ((var1 * (int64_t)s_calib.dig_P5) << 17);
    var2 = var2 + (((int64_t)s_calib.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)s_calib.dig_P3) >> 8) +
           ((var1 * (int64_t)s_calib.dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1) * ((int64_t)s_calib.dig_P1)) >> 33;

    if (var1 == 0) return 0;

    int64_t p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)s_calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)s_calib.dig_P8) * p) >> 19;

    p = ((p + var1 + var2) >> 8) + (((int64_t)s_calib.dig_P7) << 4);
    return (uint32_t)p;
}

static uint32_t bme280_compensate_hum(int32_t adc_H) {
    int32_t v_x1_u32r = s_t_fine - 76800;

    v_x1_u32r =
        (((((adc_H << 14) - (((int32_t)s_calib.dig_H4) << 20) -
            (((int32_t)s_calib.dig_H5) * v_x1_u32r)) +
           16384) >>
          15) *
         (((((((v_x1_u32r * ((int32_t)s_calib.dig_H6)) >> 10) *
              (((v_x1_u32r * ((int32_t)s_calib.dig_H3)) >> 11) + 32768)) >>
             10) +
            2097152) *
               ((int32_t)s_calib.dig_H2) +
           8192) >>
          14));

    v_x1_u32r = v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) *
                              ((int32_t)s_calib.dig_H1)) >>
                             4);

    if (v_x1_u32r < 0) v_x1_u32r = 0;
    if (v_x1_u32r > 419430400) v_x1_u32r = 419430400;
    return (uint32_t)(v_x1_u32r >> 12);
}

bool sensor_core_read_bme280(float* temperature, float* humidity,
                             float* pressure_hpa) {
    if (s_bme_addr == 0) return false;

    uint8_t raw[8] = {0};
    if (i2c_read_reg(s_bme_addr, BME280_REG_PRESS_MSB, raw, sizeof(raw)) !=
        ESP_OK) {
        return false;
    }

    int32_t adc_P = ((int32_t)raw[0] << 12) | ((int32_t)raw[1] << 4) |
                    ((int32_t)raw[2] >> 4);
    int32_t adc_T = ((int32_t)raw[3] << 12) | ((int32_t)raw[4] << 4) |
                    ((int32_t)raw[5] >> 4);
    int32_t adc_H = ((int32_t)raw[6] << 8) | raw[7];

    if (adc_T == 0x80000 || adc_P == 0x80000 || adc_H == 0x8000) return false;

    int32_t t_x100 = bme280_compensate_temp(adc_T);
    uint32_t p_q24_8 = bme280_compensate_press(adc_P);
    uint32_t h_q22_10 = bme280_compensate_hum(adc_H);

    float t = t_x100 / 100.0f;
    float p = ((float)p_q24_8 / 256.0f) / 100.0f;
    float h = h_q22_10 / 1024.0f;

    app_config_t* cfg = app_config_get();
    t += cfg->dht_temp_offset;
    p += cfg->bmp_temp_offset;

    if (h < 0.0f) h = 0.0f;
    if (h > 100.0f) h = 100.0f;
    if (p < 300.0f || p > 1200.0f) return false;

    *temperature = t;
    *humidity = h;
    *pressure_hpa = p;
    return true;
}

bool sensor_core_read_bh1750(float* lux) {
#if !ENABLE_BH1750
    (void)lux;
    return false;
#else
    if (s_bh_addr == 0) return false;

    uint8_t raw[2] = {0};
    if (i2c_read_device_bytes(s_bh_addr, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }

    uint16_t level = ((uint16_t)raw[0] << 8) | raw[1];
    if (level == 0xFFFF) {
        // Bus-high read pattern; treat as invalid sample.
        s_bh_invalid_samples++;
        if ((s_bh_invalid_samples % 10U) == 1U) {
            ESP_LOGW(TAG, "BH1750 invalid raw=0xFFFF (sample #%lu), ignoring",
                     (unsigned long)s_bh_invalid_samples);
        }
        return false;
    }

    s_bh_invalid_samples = 0;

    *lux = (float)level / 1.2f;
    if (*lux < 0.0f || *lux > 200000.0f) return false;
    return true;
#endif
}

static bool sensor_core_read_bh1750_with_recovery(float* lux) {
#if !ENABLE_BH1750
    (void)lux;
    return false;
#else
    if (s_bh_addr == 0) {
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        // Force fresh measurement each cycle to avoid stale values.
        uint8_t cmd = BH1750_CMD_ONE_TIME_HI_RES;
        if (i2c_write_bytes(s_bh_addr, &cmd, 1) != ESP_OK) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(BH1750_MEAS_TIME_MS));
        if (sensor_core_read_bh1750(lux)) {
            return true;
        }
    }

    return false;
#endif
}

void sensor_core_init() {
    s_bme_use_bitbang = false;
    s_i2c_use_bitbang_bus = false;
    s_weather_sensor_ok = false;

    ESP_LOGI(TAG,
             "Initializing Clean I2C Bus on GPIO 0 (SDA) and GPIO 1 (SCL)...");
    if (!i2c_setup_bus(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FREQ_HZ,
                       "Fixed Bus 0/1")) {
        ESP_LOGE(TAG, "I2C master bus setup failed on pins 0/1");
        return;
    }

    detect_sensors_on_current_bus();

    if (s_bme_addr == 0) {
        ESP_LOGW(
            TAG,
            "BME280 not found. Running clear master diagnostics bus scan:");
        log_i2c_scan_overview();

        uint8_t bb_bme_addr = log_bitbang_bme_probe();
        if (bb_bme_addr != 0 && s_bme_addr == 0) {
            ESP_LOGW(TAG,
                     "Bitbang saw BME ACK at 0x%02X. Retrying I2C at %lu Hz.",
                     bb_bme_addr, (unsigned long)I2C_FALLBACK_FREQ_HZ);

            if (i2c_setup_bus(I2C_SDA_PIN, I2C_SCL_PIN, I2C_FALLBACK_FREQ_HZ,
                              "Fallback Bus 0/1 @10k")) {
                detect_sensors_on_current_bus();
            } else {
                ESP_LOGE(TAG, "Fallback I2C bus setup failed at 10 kHz");
            }

            if (s_bme_addr == 0) {
                if (s_i2c_driver_installed) {
                    i2c_driver_delete(I2C_PORT);
                    s_i2c_driver_installed = false;
                }

                s_bme_addr = bb_bme_addr;
                s_bme_use_bitbang = true;
                s_i2c_use_bitbang_bus = true;

                uint8_t bb_chip_id = 0;
                if (i2c_read_reg(s_bme_addr, BME280_REG_ID, &bb_chip_id, 1) ==
                        ESP_OK &&
                    bb_chip_id == BME280_CHIP_ID) {
                    ESP_LOGW(TAG,
                             "BME280 software-I2C fallback enabled on 0x%02X",
                             s_bme_addr);

                    // Prepare bitbang lines once; runtime reads then avoid
                    // gpio_reset_pin spam in logs.
                    gpio_reset_pin(s_i2c_sda);
                    gpio_reset_pin(s_i2c_scl);
                    gpio_set_pull_mode(s_i2c_sda, GPIO_PULLUP_ONLY);
                    gpio_set_pull_mode(s_i2c_scl, GPIO_PULLUP_ONLY);
                } else {
                    ESP_LOGE(TAG,
                             "Software-I2C fallback chip-id check failed "
                             "(id=0x%02X)",
                             bb_chip_id);
                    s_bme_addr = 0;
                    s_bme_use_bitbang = false;
                    s_i2c_use_bitbang_bus = false;
                }
            }
        }
    }

    if (s_bme_addr != 0) {
        uint8_t chip_id = 0;
        if (i2c_read_reg(s_bme_addr, BME280_REG_ID, &chip_id, 1) == ESP_OK &&
            chip_id == BME280_CHIP_ID) {
            i2c_write_reg_u8(s_bme_addr, BME280_REG_RESET, BME280_RESET_CMD);
            vTaskDelay(pdMS_TO_TICKS(5));
            i2c_write_reg_u8(s_bme_addr, BME280_REG_CTRL_HUM, 0x01);
            i2c_write_reg_u8(s_bme_addr, BME280_REG_CTRL_MEAS, 0x27);
            i2c_write_reg_u8(s_bme_addr, BME280_REG_CONFIG, 0xA0);

            if (bme280_read_calibration()) {
                ESP_LOGI(TAG, "BME280 detegovany na adrese 0x%02X", s_bme_addr);
                s_weather_sensor_ok = true;
            } else {
                ESP_LOGE(TAG, "BME280 kalibracia zlyhala");
                s_bme_addr = 0;
                s_weather_sensor_ok = false;
            }
        } else {
            ESP_LOGE(TAG, "BME280 chip id mismatch (0x%02X)", chip_id);
            s_bme_addr = 0;
            s_weather_sensor_ok = false;
        }
    } else {
        ESP_LOGW(TAG, "BME280 nebol najdeny (0x76/0x77)");
        s_weather_sensor_ok = false;
    }

    if (s_bh_addr != 0) {
        uint8_t cmd = BH1750_CMD_CONT_HI_RES;
        if (i2c_write_bytes(s_bh_addr, &cmd, 1) == ESP_OK) {
            ESP_LOGI(TAG, "BH1750 detegovany na adrese 0x%02X", s_bh_addr);
            vTaskDelay(pdMS_TO_TICKS(BH1750_MEAS_TIME_MS));
        } else {
            ESP_LOGE(TAG, "BH1750 init zlyhal");
            s_bh_addr = 0;
        }
    } else {
#if ENABLE_BH1750
        ESP_LOGW(TAG, "BH1750 nebol najdeny (0x23/0x5C)");
#else
        ESP_LOGI(TAG, "BH1750 je vypnuty pre BME-only diagnostiku");
#endif
    }

    sensor_boot_selftest();
}

static void sensor_task(void* arg) {
    ESP_LOGI(TAG, "Sensor Task spusteny (zive meranie: %lums, logovanie: 10m)",
             (unsigned long)SENSOR_MEASURE_INTERVAL_MS);
    int last_log_min = -1;
    bool sync_warned = false;

    while (1) {
        float t = 0.0f, h = 0.0f, p = 0.0f;
        float lux = 0.0f;

        if (sensor_core_read_bme280(&t, &h, &p)) {
            s_last_t += SENSOR_EMA_ALPHA * (t - s_last_t);
            s_last_h += SENSOR_EMA_ALPHA * (h - s_last_h);
            s_last_p = p;
        }

        if (sensor_core_read_bh1750_with_recovery(&lux)) {
            s_last_lux = lux;
        }

        time_t now;
        time(&now);

        if (now > 1600000000) {
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);

            if (timeinfo.tm_min % 10 == 0 && timeinfo.tm_min != last_log_min) {
                last_log_min = timeinfo.tm_min;
                storage_log_sensor_data((uint32_t)now, s_last_t, s_last_h,
                                        s_last_p);
                ESP_LOGI(TAG,
                         "Ukladam senzory: T=%.1fC H=%.1f%% P=%.1fhPa Lux=%.1f",
                         s_last_t, s_last_h, s_last_p, s_last_lux);
            }
        } else if (!sync_warned) {
            ESP_LOGW(
                TAG,
                "Cas nie je synchronizovany, planner logovania caka na NTP");
            sync_warned = true;
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_MEASURE_INTERVAL_MS));
    }
}

void sensor_core_start_task() {
    xTaskCreate(sensor_task, "sensor_task", 4096, NULL, 5, NULL);
}

void sensor_core_get_latest(float* t, float* h) {
    *t = s_last_t;
    *h = s_last_h;
}

void sensor_core_get_latest_full(float* t, float* h, float* p_hpa, float* lux) {
    if (t) *t = s_last_t;
    if (h) *h = s_last_h;
    if (p_hpa) *p_hpa = s_last_p;
    if (lux) *lux = s_last_lux;
}

bool sensor_core_weather_sensor_ok() { return s_weather_sensor_ok; }
