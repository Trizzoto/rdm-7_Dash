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

static temperature_sensor_handle_t s_temp_sensor = NULL;

/* ── Fuel sender calibration ─────────────────────────────────────────── */

static fuel_cal_config_t s_fuel_cal = {
    .empty_v    = 0.5f,
    .full_v     = 3.0f,
    .full_value = 100.0f,
    .enabled    = false,
};

static float s_last_fuel_voltage = 0.0f;

/* Simple FPS counter — incremented by the flush callback, read and
 * reset every timer period (~500 ms) to derive frames per second. */
static uint32_t s_frame_count = 0;
static int64_t  s_fps_last_us = 0;
static float    s_fps_value   = 0.0f;

void signal_internal_count_frame(void) { s_frame_count++; }

/* ── Timer callback (runs on LVGL task) ──────────────────────────────── */

static void _internal_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Signal names MUST match what the ECU presets generate:
     * preset label → replace [^A-Za-z0-9_] with '_' → toUpperCase() */

    /* FPS — computed from flush callback frame count over the timer period */
    int64_t now_us = esp_timer_get_time();
    if (s_fps_last_us > 0) {
        int64_t dt_us = now_us - s_fps_last_us;
        if (dt_us > 0) {
            s_fps_value = (float)s_frame_count * 1000000.0f / (float)dt_us;
        }
    }
    s_fps_last_us = now_us;
    s_frame_count = 0;
    signal_inject_test_value("FPS", s_fps_value);

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

    /* Fuel sender ADC voltage (every tick).
     * When calibration is enabled, inject the calibrated value instead
     * of the raw voltage so a single FUEL_SENDER_V signal is all the
     * user needs. Raw voltage is still available via the fuel cal API. */
    s_last_fuel_voltage = fuel_sender_read_voltage();
    if (s_fuel_cal.enabled) {
        float range = s_fuel_cal.full_v - s_fuel_cal.empty_v;
        float level = 0.0f;
        if (range > 0.001f || range < -0.001f) {
            level = (s_last_fuel_voltage - s_fuel_cal.empty_v) / range
                    * s_fuel_cal.full_value;
        }
        if (level < 0.0f) level = 0.0f;
        if (level > s_fuel_cal.full_value) level = s_fuel_cal.full_value;
        signal_inject_test_value("FUEL_SENDER_V", level);
    } else {
        signal_inject_test_value("FUEL_SENDER_V", s_last_fuel_voltage);
    }

    /* WiFi RSSI (every tick — must be < 2 s timeout) */
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        signal_inject_test_value("WIFI_RSSI", (float)ap_info.rssi);
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

    s_internal_timer = lv_timer_create(_internal_timer_cb, 500, NULL);
    ESP_LOGI(TAG, "internal signal timer started (500 ms)");

    /* Hide the built-in LVGL perf monitor label (we expose FPS as a signal
     * instead).  The label is the first child of lv_layer_sys(). */
    lv_obj_t *sys = lv_layer_sys();
    if (sys && lv_obj_get_child_cnt(sys) > 0) {
        lv_obj_t *perf_lbl = lv_obj_get_child(sys, 0);
        if (perf_lbl) lv_obj_add_flag(perf_lbl, LV_OBJ_FLAG_HIDDEN);
    }
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

void signal_internal_set_fuel_cal(float empty_v, float full_v,
                                  float full_value, bool enabled)
{
    s_fuel_cal.empty_v    = empty_v;
    s_fuel_cal.full_v     = full_v;
    s_fuel_cal.full_value = full_value;
    s_fuel_cal.enabled    = enabled;
    ESP_LOGI(TAG, "fuel cal: empty=%.3f full=%.3f val=%.1f en=%d",
             empty_v, full_v, full_value, (int)enabled);
}

void signal_internal_get_fuel_cal(fuel_cal_config_t *out)
{
    if (out) *out = s_fuel_cal;
}

float signal_internal_get_fuel_voltage(void)
{
    return s_last_fuel_voltage;
}
