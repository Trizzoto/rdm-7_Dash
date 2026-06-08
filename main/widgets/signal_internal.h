#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A single (voltage, value) reference point on the fuel sender curve. */
typedef struct {
    float voltage;     /**< ADC voltage at this reference point */
    float value;       /**< Tank reading at this voltage (e.g. % or litres) */
} fuel_cal_point_t;

#define FUEL_CAL_MAX_POINTS 8

/** Fuel sender calibration parameters (stored in layout JSON).
 *
 * Multipoint mode (point_count >= 2): the curve is piecewise-linear between
 * adjacent points. The driver sorts `points` ascending by voltage on apply,
 * so callers can pass them in any order. Below points[0].voltage the output
 * clamps to points[0].value; above points[N-1].voltage it clamps to
 * points[N-1].value.
 *
 * Legacy 2-point mode (point_count == 0): the curve is a single straight
 * line from (empty_v -> 0) to (full_v -> full_value), used by old layouts
 * that pre-date the points[] field. The empty_v/full_v/full_value fields
 * stay as a backwards-compat view even when point_count >= 2 — set to
 * mirror points[0] and points[N-1] so anything reading them gets a sane
 * 2-point summary. */
typedef struct {
    /* Legacy 2-point view (kept in sync with the endpoints of the
     * multipoint curve when point_count >= 2). */
    float empty_v;     /**< ADC voltage at empty tank */
    float full_v;      /**< ADC voltage at full tank */
    float full_value;  /**< Calibrated value at full (e.g. 100 for %, or liters) */
    bool  enabled;     /**< Whether calibration is active */

    /* Multipoint curve. point_count == 0 means use the 2-point view above. */
    uint8_t          point_count;
    fuel_cal_point_t points[FUEL_CAL_MAX_POINTS];
} fuel_cal_config_t;

/**
 * Start the internal signal injection timer (LVGL timer, 500 ms).
 * Must be called from the LVGL task after signal_registry_init().
 */
void signal_internal_start(void);

/**
 * Stop and delete the timer.
 */
void signal_internal_stop(void);

/** Set fuel sender calibration (takes effect immediately).
 *
 * Two-point legacy entry point — clears the multipoint curve and falls back
 * to a straight line {empty_v -> 0, full_v -> full_value}. New code should
 * prefer signal_internal_set_fuel_cal_points(). */
void signal_internal_set_fuel_cal(float empty_v, float full_v,
                                  float full_value, bool enabled);

/** Set a multipoint fuel sender calibration (takes effect immediately).
 *
 * `count` must be 2..FUEL_CAL_MAX_POINTS. Points need not be pre-sorted —
 * the driver sorts ascending by voltage internally. Passing count < 2
 * is treated as "clear multipoint, leave legacy 2-point view untouched". */
void signal_internal_set_fuel_cal_points(const fuel_cal_point_t *points,
                                         uint8_t count, bool enabled);

/** Read back current fuel calibration config. */
void signal_internal_get_fuel_cal(fuel_cal_config_t *out);

/** Return the last raw ADC voltage reading from the fuel sender. */
float signal_internal_get_fuel_voltage(void);

/** Increment the FPS frame counter. Call from the display flush callback. */
void signal_internal_count_frame(void);

/* ── Calculated-gear configuration ───────────────────────────────────────
 * Live-editable from the web UI. set() persists to NVS + updates the
 * in-RAM cache so the timer sees the new config on the next tick.
 * gear_cal_config_t is defined in storage/config_store.h — pulled in by
 * anyone who uses these APIs. */
typedef struct gear_cal_config gear_cal_config_t_fwd;  /* placeholder — real typedef in config_store.h */
void signal_internal_set_gear_cal(const void *cfg);
void signal_internal_get_gear_cal(void *out);

/* ── Odometer (kilometres) ───────────────────────────────────────────────
 *
 * The ODOMETER signal accumulates kilometres from whichever signal name
 * lives in gear_cal_config_t::speed_signal (defaults to "VEHICLE_SPEED").
 * Integration runs every signal-internal tick (500 ms). Persistence to
 * NVS is hybrid — every 1 km of unsaved distance OR every 5 minutes,
 * whichever comes first.
 *
 * signal_internal_set_odometer_km() is the manual-entry path: lets the
 * user match the vehicle's factory odometer when first installing the
 * dash. Writes to NVS immediately. */
void  signal_internal_set_odometer_km(float km);
float signal_internal_get_odometer_km(void);

#ifdef __cplusplus
}
#endif
