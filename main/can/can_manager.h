/*
 * can_manager.h — TWAI peripheral lifecycle and CAN receive task.
 *
 * Initialises the hardware, manages the acceptance filter, and owns the
 * FreeRTOS task that receives frames and forwards them to the dispatch layer.
 */
#pragma once
#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Initialise the TWAI hardware only (no receive task).
 * Loads the saved bitrate from NVS, builds the dispatch table and acceptance
 * filter, installs the driver, and starts the peripheral.
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
 * acceptance filter from current config, reinstall, restart, and
 * re-create the task.  Call after any change to CAN IDs in config.
 */
void reconfigure_can_filter(void);

/**
 * Compute a hardware acceptance filter from the current warning /
 * indicator / value CAN IDs.  Writes result into *out_filter.
 */
void build_twai_filter_from_configs(twai_filter_config_t *out_filter);

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
