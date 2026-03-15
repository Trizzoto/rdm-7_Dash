/**
 * signal_internal.c — Expose internal ESP32-S3 metrics and GPIO inputs
 *                     as signals in the RDM-7 Dash signal system.
 *
 * Uses an LVGL timer (runs on LVGL task) to periodically read internal
 * metrics and inject them into any matching registered signals.  Only
 * signals that exist in the current layout are updated.
 */

#include "signal_internal.h"
#include "signal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "lvgl.h"
#include "io/wire_inputs.h"

static const char *TAG = "sig_internal";

/* Fuel sender voltage reader defined in main.c */
extern float fuel_sender_read_voltage(void);

static lv_timer_t *s_internal_timer = NULL;
static uint32_t    s_slow_counter   = 0;

static temperature_sensor_handle_t s_temp_sensor = NULL;

/* ── Timer callback (runs on LVGL task) ──────────────────────────────── */

static void _internal_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_slow_counter++;

    /* Signal names MUST match what the ECU presets generate:
     * preset label → replace [^A-Za-z0-9_] with '_' → toUpperCase() */

    /* CPU % (every tick) */
    signal_inject_test_value("CPU_PERCENT",
                             (float)(100 - lv_timer_get_idle()));

    /* Free internal heap in KB (every tick) */
    signal_inject_test_value("FREE_HEAP_KB",
                             (float)(esp_get_free_heap_size() / 1024));

    /* Free PSRAM in KB (every tick) */
    signal_inject_test_value("FREE_PSRAM_KB",
                             (float)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    /* Uptime in seconds (every tick) */
    signal_inject_test_value("UPTIME_S",
                             (float)(esp_timer_get_time() / 1000000ULL));

    /* Chip temperature (every tick) */
    if (s_temp_sensor) {
        float temp = 0;
        if (temperature_sensor_get_celsius(s_temp_sensor, &temp) == ESP_OK) {
            signal_inject_test_value("CHIP_TEMP", temp);
        }
    }

    /* GPIO indicator inputs (every tick, only if GPIOs are configured) */
    if (WIRE_INPUT_LEFT_GPIO >= 0) {
        signal_inject_test_value("INDICATOR_LEFT",
                                 (float)gpio_get_level(WIRE_INPUT_LEFT_GPIO));
    }
    if (WIRE_INPUT_RIGHT_GPIO >= 0) {
        signal_inject_test_value("INDICATOR_RIGHT",
                                 (float)gpio_get_level(WIRE_INPUT_RIGHT_GPIO));
    }

    /* Fuel sender ADC voltage (every tick) */
    signal_inject_test_value("FUEL_SENDER_V", fuel_sender_read_voltage());

    /* Slow items — every 10 ticks (5 seconds at 500 ms interval) */
    if (s_slow_counter % 10 == 0) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            signal_inject_test_value("WIFI_RSSI", (float)ap_info.rssi);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void signal_internal_start(void)
{
    if (s_internal_timer) {
        ESP_LOGW(TAG, "already started");
        return;
    }

    /* Initialise the on-chip temperature sensor (once) */
    if (!s_temp_sensor) {
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        esp_err_t err = temperature_sensor_install(&cfg, &s_temp_sensor);
        if (err == ESP_OK) {
            temperature_sensor_enable(s_temp_sensor);
            ESP_LOGI(TAG, "temperature sensor initialised");
        } else {
            ESP_LOGW(TAG, "temperature sensor init failed: %s",
                     esp_err_to_name(err));
            s_temp_sensor = NULL;
        }
    }

    s_slow_counter = 0;
    s_internal_timer = lv_timer_create(_internal_timer_cb, 500, NULL);
    ESP_LOGI(TAG, "internal signal timer started (500 ms)");
}

void signal_internal_stop(void)
{
    if (s_internal_timer) {
        lv_timer_del(s_internal_timer);
        s_internal_timer = NULL;
        ESP_LOGI(TAG, "internal signal timer stopped");
    }

    if (s_temp_sensor) {
        temperature_sensor_disable(s_temp_sensor);
        temperature_sensor_uninstall(s_temp_sensor);
        s_temp_sensor = NULL;
        ESP_LOGI(TAG, "temperature sensor released");
    }
}
