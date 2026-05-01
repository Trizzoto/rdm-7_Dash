/*
 * can_manager.h — TWAI peripheral lifecycle and CAN receive task.
 *
 * Initialises the hardware, manages the acceptance filter, and owns the
 * FreeRTOS task that receives frames and forwards them to the dispatch layer.
 */
#pragma once
#include "driver/twai.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Initialise the TWAI hardware only (no receive task).
 * Loads the saved bitrate from NVS, builds the acceptance filter from
 * the signal registry, installs the driver, and starts the peripheral.
 *
 * Call early in app_main — before the LVGL mutex exists.
 */
void can_init(void);

/**
 * Create the CAN receive task.
 * Call after the LVGL mutex has been created (late in app_main startup).
 */
void can_start_task(void);

/**
 * Stop the CAN receive task, uninstall the TWAI driver, rebuild the
 * acceptance filter from the signal registry, reinstall, restart, and
 * re-create the task.  Call after any change to signal CAN IDs.
 */
void reconfigure_can_filter(void);

/**
 * Compute a hardware acceptance filter from the signal registry.
 * Iterates all registered signals and builds a mask that passes
 * all their CAN IDs.  Writes result into *out_filter.
 */
void build_twai_filter_from_signals(twai_filter_config_t *out_filter);

/**
 * Temporarily adjust the CAN receive task's FreeRTOS priority.
 * Returns the previous priority so the caller can restore it.
 * Safe to call when no task is running (returns tskIDLE_PRIORITY).
 */
UBaseType_t can_task_get_priority(void);
void        can_task_set_priority(UBaseType_t priority);

/**
 * Change the CAN bus bitrate at runtime.
 * Stops the receive task, uninstalls the TWAI driver, applies the new
 * timing configuration, reinstalls, restarts, and re-creates the task.
 *
 * @param bitrate_index  0=125k  1=250k  2=500k  3=1M
 */
void can_change_bitrate(uint8_t bitrate_index);

/**
 * @brief Drain any pending CAN frames from the internal FreeRTOS queue and
 *        dispatch them to the widget layer via signal_dispatch_frame().
 *
 * This function is *LVGL-thread only*: it must be called from a context that
 * already holds the LVGL mutex (see rdm_lvgl_lock/rdm_lvgl_unlock in
 * main.c).  It is intended to be invoked from the LVGL task loop so that the
 * CAN receive task never touches LVGL directly.
 *
 * The function processes a small bounded batch of frames per call to avoid
 * starving other LVGL work if the bus is very busy.
 */
void can_process_queued_frames(void);

/**
 * Transmit a single CAN frame.  Thread-safe (TWAI driver handles locking).
 * Uses a short timeout to avoid blocking the LVGL task on bus errors.
 *
 * @param can_id  Standard 11-bit CAN ID (0x000–0x7FF).
 * @param data    Pointer to data bytes (up to 8).
 * @param dlc     Data length code (0–8).
 * @return ESP_OK on success, or an error code.
 */
esp_err_t can_transmit_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc);

/**
 * Read CAN bus diagnostic counters from the TWAI driver.
 * All output pointers are optional (pass NULL to skip).
 */
esp_err_t can_get_diagnostics(uint32_t *state, uint32_t *msgs_to_tx,
                              uint32_t *msgs_to_rx, uint32_t *tx_error_counter,
                              uint32_t *rx_error_counter, uint32_t *bus_error_count,
                              uint32_t *rx_missed);

/**
 * Return the CAN ID of the most recently received frame (0 if none yet).
 */
uint32_t can_get_last_rx_id(void);

/**
 * Return the cumulative count of successfully received CAN frames.
 * Incremented in the RX task; safe to read from any context.
 */
uint32_t can_get_rx_frame_count(void);

/**
 * Suspend normal CAN operation.  Stops the RX task and uninstalls
 * the TWAI driver so the caller can take ownership of the peripheral
 * (e.g. for bus scanning).
 */
void can_suspend(void);

/**
 * Resume normal CAN operation after can_suspend().  Rebuilds the
 * acceptance filter, reinstalls the TWAI driver, starts the peripheral,
 * and recreates the RX task.
 */
void can_resume(void);

/**
 * Return the TWAI timing config for a bitrate index.
 * @param index  0=125k  1=250k  2=500k  3=1M
 */
twai_timing_config_t can_get_timing_for_bitrate(uint8_t index);

/** True if can_suspend() was called and resume hasn't completed. */
bool can_is_suspended(void);

/**
 * Best-effort recovery from a "CAN unavailable" state. Called by the
 * Device Settings CAN diagnostics card when it sees twai_get_status_info
 * fail. Re-runs whichever path is appropriate:
 *   - if suspended: retries can_resume() (e.g. previous resume failed)
 *   - else if driver is uninstalled: does a fresh install + start + RX task
 * Self-rate-limited to one attempt per 5 seconds so a stuck failure mode
 * doesn't burn CPU. Returns true if recovery was attempted this call.
 */
bool can_recover(void);
