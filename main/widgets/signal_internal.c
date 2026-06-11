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
#include "data/channel_manager.h"
#include "storage/config_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "driver/gpio.h"
#include "driver/temperature_sensor.h"
#include "lvgl.h"
#include "io/wire_inputs.h"
#include <math.h>

static const char *TAG = "sig_internal";

/* Fuel sender voltage reader defined in main.c */
extern float fuel_sender_read_voltage(void);

static lv_timer_t *s_internal_timer = NULL;

static temperature_sensor_handle_t s_temp_sensor = NULL;

/* ── Fuel sender calibration ─────────────────────────────────────────── */

static fuel_cal_config_t s_fuel_cal = {
    .empty_v     = 0.5f,
    .full_v      = 3.0f,
    .full_value  = 100.0f,
    .enabled     = false,
    .point_count = 0,
};

/* Piecewise-linear interpolation against the active calibration.
 *
 *   - When point_count >= 2, walks the (sorted) points and finds the
 *     segment that brackets v. Clamps outside.
 *   - Otherwise falls back to the legacy 2-point line.
 *
 * Output is clamped to [0, max_point_value] so a noisy sender that briefly
 * reads beyond either end can't push a tank reading negative or beyond the
 * top-of-scale. */
static float _fuel_apply_cal(float v)
{
    if (s_fuel_cal.point_count >= 2) {
        const fuel_cal_point_t *p = s_fuel_cal.points;
        uint8_t n = s_fuel_cal.point_count;
        if (v <= p[0].voltage)   return p[0].value;
        if (v >= p[n-1].voltage) return p[n-1].value;
        for (uint8_t i = 0; i + 1 < n; i++) {
            float v0 = p[i].voltage, v1 = p[i+1].voltage;
            if (v >= v0 && v <= v1) {
                float dv = v1 - v0;
                if (dv <= 0.0001f) return p[i].value;
                float t = (v - v0) / dv;
                return p[i].value + t * (p[i+1].value - p[i].value);
            }
        }
        return p[n-1].value;
    }
    /* Legacy 2-point path. */
    float range = s_fuel_cal.full_v - s_fuel_cal.empty_v;
    if (range > -0.001f && range < 0.001f) return 0.0f;
    float level = (v - s_fuel_cal.empty_v) / range * s_fuel_cal.full_value;
    if (level < 0.0f) level = 0.0f;
    if (level > s_fuel_cal.full_value) level = s_fuel_cal.full_value;
    return level;
}

static float s_last_fuel_voltage = 0.0f;

/* ── Calculated gear config (loaded from NVS on start, refreshed via API) ── */
static gear_cal_config_t s_gear_cal = {0};

/* ── Odometer state ────────────────────────────────────────────────────────
 *
 * s_odometer_km holds the live running total — published each tick as the
 * "ODOMETER" signal. s_odo_unsaved_km is the accumulator that decides when
 * to flush to NVS; reset to zero on every successful save.
 *
 * Save trigger:  ≥ 1 km unsaved  OR  ≥ 5 min since the last save with any
 * non-zero unsaved distance. Worst-case loss on unexpected power-off is
 * ~1 km at highway speed and ~0.4 km at very low speeds. */
#define ODOMETER_SAVE_THRESHOLD_KM   1.0f
#define ODOMETER_SAVE_PERIOD_US      ((int64_t)300000000)  /* 5 min */
#define INTERNAL_TIMER_PERIOD_S      0.5f                   /* mirrors lv_timer_create() */

/* Upper sanity cap on integrated speed. No production passenger car does
 * 400+ km/h, so any sample above this is provably sensor garbage — most
 * commonly an OEM-defined "no data" sentinel (Falcon FG's 0x207 bytes 4-5
 * read 0xFFFF when stationary, which the preset's scale of 1/128 turns
 * into 511.99 km/h) but also raw-bit decode errors with a wrong
 * scale/offset. Without this cap a sentinel-broadcasting frame quietly
 * adds ~8 km/min to the odometer while parked. Logged-once warning so the
 * user can see something's wrong with the speed signal without spamming. */
#define ODOMETER_MAX_VALID_KMH       400.0f

static float   s_odometer_km        = 0.0f;
static float   s_odo_unsaved_km     = 0.0f;
static int64_t s_odo_last_save_us   = 0;
static bool    s_odo_speed_warned   = false;
static bool    s_odo_speed_capped_warned = false;

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

    /* GPIO indicator inputs (every tick, only if wire-input mode is on so
     * the GPIOs are actually claimed — getters return -1 otherwise). */
    int left_gpio  = wire_inputs_get_left_gpio();
    int right_gpio = wire_inputs_get_right_gpio();
    if (left_gpio >= 0) {
        signal_inject_test_value("INDICATOR_LEFT",
                                 (float)gpio_get_level(left_gpio));
    }
    if (right_gpio >= 0) {
        signal_inject_test_value("INDICATOR_RIGHT",
                                 (float)gpio_get_level(right_gpio));
    }

    /* Fuel sender ADC voltage (every tick).
     * When calibration is enabled, inject the calibrated value instead
     * of the raw voltage so a single FUEL_SENDER_V signal is all the
     * user needs. Raw voltage is still available via the fuel cal API. */
    s_last_fuel_voltage = fuel_sender_read_voltage();
    if (s_fuel_cal.enabled) {
        signal_inject_test_value("FUEL_SENDER_V",
                                 _fuel_apply_cal(s_last_fuel_voltage));
    } else {
        signal_inject_test_value("FUEL_SENDER_V", s_last_fuel_voltage);
    }

    /* WiFi RSSI (every tick — must be < 2 s timeout) */
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        signal_inject_test_value("WIFI_RSSI", (float)ap_info.rssi);
    }

    /* ── CALCULATED_GEAR ──────────────────────────────────────────────
     * Back-compute the currently engaged gear from live RPM + speed by
     * comparing the observed overall ratio to the user-configured
     * gearbox + final-drive ratios.
     *
     *   wheel_rps    = (speed_kmh * 1000 / 3600) / wheel_circumference_m
     *   overall      = (rpm / 60) / wheel_rps          // engine revs per wheel rev
     *   gearbox      = overall / final_drive
     *   best_gear    = argmin_i |gearbox - ratios[i]|
     *
     * Emits 0 for N / stationary (speed below 5 km/h OR rpm below 500).
     * When disabled (user hasn't configured), no signal is emitted — the
     * widget that tries to read CALCULATED_GEAR simply sees no value.
     *
     * If the user-configured rpm/speed signal names don't resolve (most
     * common cause: the signal was renamed in the layout but the gear
     * config wasn't updated), log a warning ONCE rather than computing
     * silently every tick. The warned-state flag resets when the config
     * is updated via the API or the next time the lookup succeeds. */
    static bool s_gear_warned_missing = false;
    if (s_gear_cal.enabled && s_gear_cal.ratio_count > 1 &&
        s_gear_cal.wheel_circumference_m > 0.01f &&
        s_gear_cal.final_drive > 0.01f) {
        int16_t rpm_idx   = signal_find_by_name(s_gear_cal.rpm_signal);
        int16_t speed_idx = signal_find_by_name(s_gear_cal.speed_signal);
        signal_t *rpm_sig   = (rpm_idx   >= 0) ? signal_get_by_index((uint16_t)rpm_idx)   : NULL;
        signal_t *speed_sig = (speed_idx >= 0) ? signal_get_by_index((uint16_t)speed_idx) : NULL;
        if (!rpm_sig || !speed_sig) {
            if (!s_gear_warned_missing) {
                ESP_LOGW(TAG, "CALCULATED_GEAR enabled but signals not resolvable "
                              "(rpm='%s'%s, speed='%s'%s) — gear will not update",
                         s_gear_cal.rpm_signal,   rpm_sig   ? "" : " [MISSING]",
                         s_gear_cal.speed_signal, speed_sig ? "" : " [MISSING]");
                s_gear_warned_missing = true;
            }
        } else {
            s_gear_warned_missing = false;
            float rpm   = rpm_sig->current_value;
            float speed = speed_sig->current_value;
            {
                float gear = 0.0f;
                if (speed < 5.0f || rpm < 500.0f) {
                    gear = 0.0f;  /* stationary / idle → Neutral */
                } else {
                    float wheel_rps = (speed * 1000.0f / 3600.0f)
                                      / s_gear_cal.wheel_circumference_m;
                    if (wheel_rps > 0.01f) {
                        float overall  = (rpm / 60.0f) / wheel_rps;
                        float gearbox  = overall / s_gear_cal.final_drive;
                        /* Find closest configured ratio, skipping index 0 (N). */
                        int best_i = 0;
                        float best_err = 1e9f;
                        for (int i = 1; i < s_gear_cal.ratio_count; i++) {
                            float err = fabsf(gearbox - s_gear_cal.ratios[i]);
                            if (err < best_err) { best_err = err; best_i = i; }
                        }
                        gear = (float)best_i;
                    }
                }
                signal_inject_test_value("CALCULATED_GEAR", gear);
            }
        }
    }

    /* ── ODOMETER ────────────────────────────────────────────────────
     * Integrate the configured vehicle-speed signal each tick:
     *
     *   km_per_tick = speed_kmh × dt_seconds / 3600
     *
     * Source signal: gear_cal.speed_signal if set, else "VEHICLE_SPEED".
     * (Reusing gear_cal's speed_signal field means the user only sets the
     * speed source once and it powers both features.)
     *
     * Skipped silently when the signal isn't in the registry or is stale —
     * no point integrating zero, and a stale signal usually means the
     * engine is off.
     *
     * Persistence happens inside this block on the hybrid trigger
     * (≥ ODOMETER_SAVE_THRESHOLD_KM accumulated OR
     *  ≥ ODOMETER_SAVE_PERIOD_US since last save).
     *
     * The ODOMETER signal value is published every tick regardless so
     * widgets show a steady reading even before the next save fires. */
    {
        const char *speed_name = s_gear_cal.speed_signal[0]
                               ? s_gear_cal.speed_signal
                               : "VEHICLE_SPEED";
        int16_t   sidx = signal_find_by_name(speed_name);
        signal_t *sig  = (sidx >= 0) ? signal_get_by_index((uint16_t)sidx) : NULL;
        if (sig && !sig->is_stale) {
            s_odo_speed_warned = false;
            float speed = sig->current_value;
            /* Sanity cap: anything ≥ 400 km/h is treated as a sentinel /
             * decode error, not a real speed. See ODOMETER_MAX_VALID_KMH
             * comment. We warn once so the user knows their speed signal
             * is mis-decoded without spamming the log every 500 ms. */
            if (speed > 0.0f && speed < ODOMETER_MAX_VALID_KMH) {
                float km_per_tick = speed * INTERNAL_TIMER_PERIOD_S / 3600.0f;
                s_odometer_km    += km_per_tick;
                s_odo_unsaved_km += km_per_tick;
            } else if (speed >= ODOMETER_MAX_VALID_KMH && !s_odo_speed_capped_warned) {
                ESP_LOGW(TAG, "ODOMETER: '%s' = %.1f km/h exceeds %.0f cap; "
                              "treating as sentinel/decode error (will not "
                              "integrate). Fix the signal's bit position or "
                              "scale to silence this.",
                         speed_name, (double)speed,
                         (double)ODOMETER_MAX_VALID_KMH);
                s_odo_speed_capped_warned = true;
            }
        } else if (!s_odo_speed_warned) {
            ESP_LOGI(TAG, "ODOMETER: speed signal '%s' not in registry — "
                          "odometer will only update once it's registered",
                     speed_name);
            s_odo_speed_warned = true;
        }
        signal_inject_test_value("ODOMETER", s_odometer_km);

        /* Persist on threshold OR period. The period branch only fires if
         * there's actually unsaved distance — no point burning an NVS write
         * to re-save the same value when the car has been parked for hours. */
        int64_t now_us = esp_timer_get_time();
        bool save_due = false;
        if (s_odo_unsaved_km >= ODOMETER_SAVE_THRESHOLD_KM) save_due = true;
        if (!save_due &&
            s_odo_unsaved_km > 0.001f &&
            s_odo_last_save_us > 0 &&
            (now_us - s_odo_last_save_us) > ODOMETER_SAVE_PERIOD_US) {
            save_due = true;
        }
        if (save_due) {
            esp_err_t err = config_store_save_odometer_km(s_odometer_km);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "ODOMETER persisted: %.3f km (+%.3f km since last save)",
                         (double)s_odometer_km, (double)s_odo_unsaved_km);
                s_odo_unsaved_km   = 0.0f;
                s_odo_last_save_us = now_us;
            } else {
                ESP_LOGW(TAG, "ODOMETER NVS save failed: %s — will retry next tick",
                         esp_err_to_name(err));
            }
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

/* Internal-source signals — always present regardless of layout/ECU.
 * Pre-registering here means the dash-system canonical channels
 * (dash_fps, dash_cpu, …) can resolve their signal_index even on
 * layouts whose JSON doesn't enumerate them. Duplicate registrations
 * are silently no-op'd by signal_register, so re-runs after layout
 * reloads are safe. */
static void _register_internal_signals(void) {
    struct { const char *name; const char *unit; } DASH_SIGS[] = {
        {"FPS",            "fps"},
        {"CPU_PERCENT",    "%"},
        {"FREE_HEAP_KB",   "kB"},
        {"FREE_PSRAM_KB",  "kB"},
        {"UPTIME_S",       "s"},
        {"CHIP_TEMP",      "°C"},
        {"WIFI_RSSI",      "dBm"},
        /* Other internal signals (ODOMETER, FUEL_SENDER_V, etc) are
         * usually declared in the layout JSON, but registering them
         * here too means a layout that omits them still works. */
        {"ODOMETER",       "km"},
        {"FUEL_SENDER_V",  "V"},
        {"CALCULATED_GEAR","gear"},
        {"INDICATOR_LEFT", ""},
        {"INDICATOR_RIGHT",""},
    };
    for (size_t i = 0; i < sizeof(DASH_SIGS)/sizeof(DASH_SIGS[0]); i++) {
        signal_register_with_source(DASH_SIGS[i].name,
                                    /*can_id=*/0,
                                    /*bit_start=*/0,
                                    /*bit_length=*/0,
                                    /*scale=*/1.0f, /*offset=*/0.0f,
                                    /*is_signed=*/false, /*endian=*/1,
                                    DASH_SIGS[i].unit,
                                    SIGNAL_SOURCE_INTERNAL);
    }
}

void signal_internal_register_signals(void)
{
    _register_internal_signals();
}

void signal_internal_start(void)
{
    if (s_internal_timer) {
        ESP_LOGW(TAG, "already started");
        return;
    }

    /* Make sure dash-internal signals exist in the registry BEFORE the
     * inject timer fires (signal_inject_test_value silently no-ops on
     * unregistered names). Also re-resolve channel bindings — any
     * dash_* canonical channels activated at boot couldn't see these
     * signals when channel_manager_init ran, so wake them up now. */
    _register_internal_signals();
    channel_manager_resolve_signals();

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

    /* Load gear cal from NVS so CALCULATED_GEAR is available from tick 0
     * once the user has configured it. First boot: enabled=false, timer
     * skips the compute branch — no CPU overhead. */
    config_store_load_gear_cal(&s_gear_cal);

    /* Load saved odometer reading. First boot returns ESP_ERR_NOT_FOUND and
     * leaves s_odometer_km at 0 — the user can match the factory reading
     * via the Vehicle Settings card. last_save_us = 0 sentinel disables
     * the period-based save until we've actually saved at least once. */
    float saved_odo = 0.0f;
    if (config_store_load_odometer_km(&saved_odo) == ESP_OK) {
        s_odometer_km = saved_odo;
        ESP_LOGI(TAG, "ODOMETER restored from NVS: %.3f km",
                 (double)s_odometer_km);
    } else {
        ESP_LOGI(TAG, "ODOMETER: no saved value — starting at 0");
    }
    s_odo_unsaved_km   = 0.0f;
    s_odo_last_save_us = esp_timer_get_time();

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
    s_fuel_cal.empty_v     = empty_v;
    s_fuel_cal.full_v      = full_v;
    s_fuel_cal.full_value  = full_value;
    s_fuel_cal.enabled     = enabled;
    /* Switching back to the 2-point legacy view — drop any multipoint
     * curve that was previously installed so interpolation uses the line. */
    s_fuel_cal.point_count = 0;
    ESP_LOGI(TAG, "fuel cal: empty=%.3f full=%.3f val=%.1f en=%d",
             empty_v, full_v, full_value, (int)enabled);
}

void signal_internal_set_fuel_cal_points(const fuel_cal_point_t *points,
                                         uint8_t count, bool enabled)
{
    if (!points || count < 2) {
        /* Treat as "disable multipoint, keep legacy 2-point view". */
        s_fuel_cal.point_count = 0;
        s_fuel_cal.enabled = enabled;
        ESP_LOGI(TAG, "fuel cal: multipoint cleared, enabled=%d", (int)enabled);
        return;
    }
    if (count > FUEL_CAL_MAX_POINTS) count = FUEL_CAL_MAX_POINTS;

    /* Copy then sort ascending by voltage (insertion sort — tiny N). */
    fuel_cal_point_t tmp[FUEL_CAL_MAX_POINTS];
    for (uint8_t i = 0; i < count; i++) tmp[i] = points[i];
    for (uint8_t i = 1; i < count; i++) {
        fuel_cal_point_t k = tmp[i];
        int j = (int)i - 1;
        while (j >= 0 && tmp[j].voltage > k.voltage) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = k;
    }
    for (uint8_t i = 0; i < count; i++) s_fuel_cal.points[i] = tmp[i];
    s_fuel_cal.point_count = count;
    s_fuel_cal.enabled = enabled;

    /* Keep the 2-point legacy view in sync with the endpoints so any
     * downstream reader (status JSON, layout serializer) gets a sensible
     * summary even if it doesn't yet understand points[]. */
    s_fuel_cal.empty_v    = tmp[0].voltage;
    s_fuel_cal.full_v     = tmp[count - 1].voltage;
    s_fuel_cal.full_value = tmp[count - 1].value;

    ESP_LOGI(TAG, "fuel cal: %u points, enabled=%d (range %.3fV..%.3fV → %.1f)",
             (unsigned)count, (int)enabled,
             tmp[0].voltage, tmp[count - 1].voltage, tmp[count - 1].value);
}

void signal_internal_get_fuel_cal(fuel_cal_config_t *out)
{
    if (out) *out = s_fuel_cal;
}

float signal_internal_get_fuel_voltage(void)
{
    return s_last_fuel_voltage;
}

void signal_internal_set_gear_cal(const void *cfg)
{
    if (!cfg) return;
    const gear_cal_config_t *in = (const gear_cal_config_t *)cfg;
    s_gear_cal = *in;
    config_store_save_gear_cal(in);
    ESP_LOGI(TAG, "gear cal updated: wheel=%.3fm fd=%.3f n=%u en=%d",
             s_gear_cal.wheel_circumference_m, s_gear_cal.final_drive,
             (unsigned)s_gear_cal.ratio_count, (int)s_gear_cal.enabled);
}

void signal_internal_get_gear_cal(void *out)
{
    if (!out) return;
    *(gear_cal_config_t *)out = s_gear_cal;
}

/* ── Odometer public API ──────────────────────────────────────────────── */

void signal_internal_set_odometer_km(float km)
{
    if (!isfinite(km) || km < 0.0f) km = 0.0f;
    s_odometer_km      = km;
    s_odo_unsaved_km   = 0.0f;
    /* Commit straight to NVS so a manual entry survives an immediate
     * power-cycle (common pattern when fitting the dash: user types in
     * the existing odometer reading then walks away to check wiring). */
    esp_err_t err = config_store_save_odometer_km(km);
    s_odo_last_save_us = esp_timer_get_time();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ODOMETER manually set to %.3f km", (double)km);
    } else {
        ESP_LOGW(TAG, "ODOMETER manual set: NVS save failed: %s",
                 esp_err_to_name(err));
    }
    /* Publish immediately so any open Vehicle Settings card / widget sees
     * the new value without waiting for the next 500 ms tick. */
    signal_inject_test_value("ODOMETER", s_odometer_km);
}

float signal_internal_get_odometer_km(void)
{
    return s_odometer_km;
}
