#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fuel sender calibration parameters (stored in layout JSON). */
typedef struct {
    float empty_v;     /**< ADC voltage at empty tank */
    float full_v;      /**< ADC voltage at full tank */
    float full_value;  /**< Calibrated value at full (e.g. 100 for %, or liters) */
    bool  enabled;     /**< Whether calibration is active */
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

/** Set fuel sender calibration (takes effect immediately). */
void signal_internal_set_fuel_cal(float empty_v, float full_v,
                                  float full_value, bool enabled);

/** Read back current fuel calibration config. */
void signal_internal_get_fuel_cal(fuel_cal_config_t *out);

/** Return the last raw ADC voltage reading from the fuel sender. */
float signal_internal_get_fuel_voltage(void);

/** Increment the FPS frame counter. Call from the display flush callback. */
void signal_internal_count_frame(void);

#ifdef __cplusplus
}
#endif
