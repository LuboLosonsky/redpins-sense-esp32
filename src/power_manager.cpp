#include "power_manager.h"

#include "display_hal.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "gui_state.h"  // BTN_OK_PIN/BTN_ESC_PIN/BTN_UP_PIN/BTN_DOWN_PIN
#include "nvs.h"
#include "sensor_core.h"
#include "wifi_scanner.h"

static const char* TAG = "PWR_MGR";

// Predvoleny jas mimo MODE_BALANCED - zodpoveda povodnej hodnote nastavenej
// v display_hal_init() (duty 80/255 ~ 31%), aby prechod PERFORMANCE po
// eco rezime vyzeral ako "normalny" stav zariadenia.
#define POWER_PERFORMANCE_BACKLIGHT_PERCENT 31
#define POWER_BALANCED_BACKLIGHT_PERCENT 5

// Docasne vykonnostne okno po aktivite (stlacenie tlacidla / LDR tien) v
// MODE_BALANCED.
#define POWER_BALANCED_ACTIVITY_WINDOW_MS 60000
// Okno pred (opatovnym) zaspatim v MODE_LONG_LIFE - kratsie nez Balanced
// okno, aby sa dalo rychlejsie testovat/interagovat po fast-wake.
#define POWER_LONG_LIFE_ARM_WINDOW_MS 30000

// MODE_LONG_LIFE pouziva casovac namiesto ext1 GPIO-wake: interny weak
// pull-up (~45k na ESP32-C6) sa ukazal nespolahlivy proti sumu na
// vonkajsich tlacidlovych vedeniach (opakovane falosne prebudenia na
// roznych pinoch bez akehokolvek stlacenia). Zariadenie sa preto prebudi
// pravidelne samo, potichu (bez zapnutia displeja) skontroluje tlacidla
// digitalne, a ak nic nie je stlacene, ihned zaspi znova.
#define POWER_LONG_LIFE_POLL_INTERVAL_MS 500

// Diferencialna detekcia tiena na LDR (MODE_BALANCED)
#define POWER_LDR_CHECK_INTERVAL_MS 200
#define POWER_LDR_DROP_RATIO 0.5f  // pokles o viac ako 50 % (bolo 35 %)
// Pod touto hranicou (lux) sa pokles neyhodnocuje - v takmer tme su male
// absolutne zmeny (sum senzora) percentualne obrovske a spusobovali by
// falosne poplachy.
#define POWER_LDR_MIN_PREV_LUX 8.0f
// Pokles musi byt potvrdeny na 2 po sebe iducich vzorkach (400ms), aby
// jednorazovy sumovy vypadok senzora nevyvolal zbytocne prebudenie.
#define POWER_LDR_CONFIRM_SAMPLES 2
// Po kazdej zmene podsvietenia (eco<->performance) senzor BH1750 chvilu
// "dobieha" na novu uroven jasu displeja - a kedze je fyzicky blizko
// displeja, samotna zmena podsvietenia mu vie zamiesat namerany lux.
// Pocas tohto okna po zmene sa pokles neyhodnocuje, len sa tichoo prebuduje
// baseline (s_prev_lux), aby nedoslo k tomu, ze si zariadenie "prebudi
// samo seba" tym, ze stmavne displej.
#define POWER_LDR_SETTLE_MS 1000

// Docasny diagnosticky log kazdej LDR vzorky (prev/cur lux, streak,
// aktualny backlight%) - pomaha overit/vylucit feedback loop medzi
// podsvietenim a senzorom. Bezny provoz loguje len prechody (viz
// "LDR tien potvrdeny"/"LDR navrat do eco" nizsie), preto default 0.
#define PWR_MGR_DEBUG_LDR 0

static PowerMode s_mode = MODE_PERFORMANCE;
static bool s_fast_wake = false;

static bool s_temp_perf_active = false;
static uint32_t s_activity_deadline_ms = 0;
static uint32_t s_last_ldr_check_ms = 0;
static uint32_t s_ldr_settle_until_ms = 0;
static float s_prev_lux = -1.0f;
static int s_ldr_drop_streak = 0;

// Tlacidla kontrolovane pri kazdom periodickom MODE_LONG_LIFE wake.
static const gpio_num_t s_button_pins[] = {
    (gpio_num_t)BTN_OK_PIN, (gpio_num_t)BTN_ESC_PIN, (gpio_num_t)BTN_UP_PIN,
    (gpio_num_t)BTN_DOWN_PIN};
#define POWER_BUTTON_PIN_COUNT \
    (sizeof(s_button_pins) / sizeof(s_button_pins[0]))

// Digitalne prečíta vsetky tlacidla (bez zavislosti na RTC/ext1 - obycajny
// GPIO input s pull-up). Pouzite pri periodickom Long Life wake, PRED
// akymkolvek displej/senzor/BLE initom.
static bool power_manager_any_button_pressed(void) {
    bool pressed = false;
    for (size_t i = 0; i < POWER_BUTTON_PIN_COUNT; i++) {
        gpio_set_direction(s_button_pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(s_button_pins[i], GPIO_PULLUP_ONLY);
        if (gpio_get_level(s_button_pins[i]) == 0) pressed = true;
    }
    return pressed;
}

// Vyzbroji "settle" okno a zrusi rozbehnutu baseline po zmene podsvietenia -
// viz komentar pri POWER_LDR_SETTLE_MS.
static void power_manager_arm_ldr_settle(void) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_ldr_settle_until_ms = now_ms + POWER_LDR_SETTLE_MS;
    s_prev_lux = -1.0f;
    s_ldr_drop_streak = 0;
}

static void power_manager_apply_eco_display(void) {
    display_hal_set_backlight_percent(POWER_BALANCED_BACKLIGHT_PERCENT);
    display_hal_set_eco_framerate(true);
    power_manager_arm_ldr_settle();
}

static void power_manager_apply_performance_display(void) {
    display_hal_set_backlight_percent(POWER_PERFORMANCE_BACKLIGHT_PERCENT);
    display_hal_set_eco_framerate(false);
    power_manager_arm_ldr_settle();
}

static void power_manager_apply_display_for_mode(PowerMode mode) {
    if (mode == MODE_BALANCED) {
        power_manager_apply_eco_display();
    } else {
        power_manager_apply_performance_display();
    }
}

// MODE_LONG_LIFE: cisty deep sleep, prebudenie na casovac (nie ext1 - viz
// POWER_LONG_LIFE_POLL_INTERVAL_MS). Displej ide do spanku (bezpecny no-op
// ak este nebol tento boot inicializovany - viz power_manager_handle_
// long_life_wake). Funkcia sa nikdy nevrati.
static void enter_long_life_sleep(void) {
    ESP_LOGI(TAG,
             "Vstupujem do MODE_LONG_LIFE deep sleep (timer wake, %dms)...",
             POWER_LONG_LIFE_POLL_INTERVAL_MS);

    display_hal_set_backlight_percent(0);
    display_hal_enter_display_sleep();

    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup((uint64_t)POWER_LONG_LIFE_POLL_INTERVAL_MS *
                                   1000ULL);

    // Zanshin fix: potlacime auto-reconnect PRED esp_wifi_stop() - inak by
    // disconnect event z tohto stopu vyvolal esp_wifi_connect() z ineho
    // tasku sucasne s prebiehajucim vypinanim (viz wifi_scanner.cpp).
    wifi_scanner_prepare_shutdown();
    esp_err_t wifi_stop_err = esp_wifi_stop();  // Best-effort, aj ked WiFi
                                                 // nikdy nebezalo
    ESP_LOGI(TAG, "esp_wifi_stop()=%d, uspavam CPU (esp_deep_sleep_start)...",
             (int)wifi_stop_err);

    esp_deep_sleep_start();  // Nikdy sa nevrati
}

void power_manager_init(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    nvs_handle_t h;
    if (nvs_open("power_cfg", NVS_READONLY, &h) == ESP_OK) {
        uint8_t stored = MODE_PERFORMANCE;
        if (nvs_get_u8(h, "power_mode", &stored) == ESP_OK) {
            s_mode = (PowerMode)stored;
        }
        nvs_close(h);
    }

    s_fast_wake =
        (cause != ESP_SLEEP_WAKEUP_UNDEFINED) && (s_mode == MODE_LONG_LIFE);

    ESP_LOGI(TAG, "Nacitany power_mode=%d, wakeup_cause=%d, fast_wake=%d",
             (int)s_mode, (int)cause, (int)s_fast_wake);
}

bool power_manager_is_fast_wake(void) { return s_fast_wake; }

bool power_manager_check_button_wake(void) {
    if (!s_fast_wake || s_mode != MODE_LONG_LIFE) return true;

    if (power_manager_any_button_pressed()) {
        ESP_LOGI(TAG,
                 "Long Life periodicky wake: tlacidlo drzane, pokracujem do "
                 "plneho boot flow");
        return true;
    }

    return false;  // Rutinny periodicky wake bez tlacidla - volajuci
                    // (main.cpp) rozhodne o udrzbe dat pred navratom do spanku
}

void power_manager_on_display_ready(void) {
    power_manager_apply_display_for_mode(s_mode);

    // MODE_LONG_LIFE: aj pri fast-wake, aj pri studenom starte s ulozenym
    // Long Life rezimom, potrebujeme vyzbrojit okno predtym, nez sa znova
    // zacne uvazovat o spanku (inak by prve tick() vyhodnotenie s
    // deadline=0 uspalo zariadenie takmer okamzite).
    if (s_mode == MODE_LONG_LIFE) {
        power_manager_notify_activity();
    }
}

PowerMode power_manager_get_mode(void) { return s_mode; }

void power_manager_set_mode(PowerMode mode) {
    s_mode = mode;
    s_temp_perf_active = false;
    s_prev_lux = -1.0f;
    s_ldr_drop_streak = 0;

    nvs_handle_t h;
    if (nvs_open("power_cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "power_mode", (uint8_t)mode);
        nvs_commit(h);
        nvs_close(h);
        ESP_LOGI(TAG, "power_mode ulozeny do NVS: %d", (int)mode);
    } else {
        ESP_LOGE(TAG, "Chyba pri otvarani NVS namespace 'power_cfg'");
    }

    power_manager_apply_display_for_mode(mode);

    // MODE_LONG_LIFE: neuspavame okamzite z menu (prekvapilo by pouzivatela
    // uprostred navigacie) - vyzbrojime rovnake 60s okno ako pri prebudeni.
    if (mode == MODE_LONG_LIFE) {
        power_manager_notify_activity();
    }
}

void power_manager_notify_activity(void) {
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t window_ms = (s_mode == MODE_LONG_LIFE)
                             ? POWER_LONG_LIFE_ARM_WINDOW_MS
                             : POWER_BALANCED_ACTIVITY_WINDOW_MS;
    s_activity_deadline_ms = now_ms + window_ms;

    if (s_mode == MODE_BALANCED && !s_temp_perf_active) {
        s_temp_perf_active = true;
        power_manager_apply_performance_display();
    }
}

void power_manager_tick(void) {
    if (s_mode == MODE_PERFORMANCE) return;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    if (s_mode == MODE_LONG_LIFE) {
        if (now_ms >= s_activity_deadline_ms) {
            enter_long_life_sleep();  // Nikdy sa nevrati
        }
        return;
    }

    if (s_mode == MODE_BALANCED) {
        if (now_ms - s_last_ldr_check_ms >= POWER_LDR_CHECK_INTERVAL_MS) {
            s_last_ldr_check_ms = now_ms;

            float t = 0.0f, hum = 0.0f, p = 0.0f, lux = 0.0f;
            sensor_core_get_latest_full(&t, &hum, &p, &lux);

            if (lux >= 0.0f) {
                bool settling = now_ms < s_ldr_settle_until_ms;
                bool drop_now = !settling &&
                                s_prev_lux >= POWER_LDR_MIN_PREV_LUX &&
                                lux < s_prev_lux * POWER_LDR_DROP_RATIO;

#if PWR_MGR_DEBUG_LDR
                ESP_LOGI(TAG,
                         "LDR check: prev=%.1f cur=%.1f streak=%d "
                         "settling=%d bl=%u%%",
                         s_prev_lux, lux, s_ldr_drop_streak, (int)settling,
                         display_hal_get_backlight_percent());
#endif

                if (drop_now) {
                    s_ldr_drop_streak++;
                    if (s_ldr_drop_streak >= POWER_LDR_CONFIRM_SAMPLES) {
                        ESP_LOGI(TAG,
                                 "LDR tien potvrdeny (%.1f -> %.1f lx) - "
                                 "spustam docasny performance rezim",
                                 s_prev_lux, lux);
                        power_manager_notify_activity();
                        s_ldr_drop_streak = 0;
                    }
                } else if (!settling) {
                    s_ldr_drop_streak = 0;
                }
                s_prev_lux = lux;
            }
        }

        if (s_temp_perf_active && now_ms >= s_activity_deadline_ms) {
            s_temp_perf_active = false;
            // Spolocne pre LDR aj tlacidlo - obe vyvolavaju rovnaky docasny
            // stav cez power_manager_notify_activity(), nerozlisujeme zdroj.
            ESP_LOGI(TAG, "Docasny performance rezim vyprsal, navrat do eco");
            power_manager_apply_eco_display();
        }
    }
}

void power_manager_force_long_life_sleep(void) {
    enter_long_life_sleep();  // Nikdy sa nevrati
}
