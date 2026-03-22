/*
 * can_bus_test.h -- Automated CAN bus scan: bitrate detection.
 *
 * Runs a FreeRTOS task that temporarily takes ownership of the TWAI peripheral
 * (via can_suspend/can_resume) and tests each bitrate in listen-only mode.
 * Communicates results to the LVGL thread via lv_async_call().
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CAN_SCAN_IDLE,
    CAN_SCAN_STOPPING,
    CAN_SCAN_TESTING_BITRATE,
    CAN_SCAN_RESTORING,
    CAN_SCAN_COMPLETE,
    CAN_SCAN_CANCELLED,
    CAN_SCAN_ERROR
} can_scan_state_t;

typedef struct {
    uint8_t  bitrate_index;
    uint32_t frames_received;
    uint32_t bus_errors;
    uint32_t unique_ids[32];
    uint8_t  unique_id_count;
    bool     traffic_detected;
} can_scan_bitrate_result_t;

typedef struct {
    can_scan_state_t          state;
    uint8_t                   current_bitrate_idx;
    can_scan_bitrate_result_t results[4];
    int8_t                    recommended_bitrate; /* -1 if none found */
} can_scan_report_t;

/** Start a bus scan. Returns false if already running. */
bool can_bus_test_start(void);

/** Request cancellation of a running scan. */
void can_bus_test_cancel(void);

/** True while the scan task is active. */
bool can_bus_test_is_running(void);

/** Current state of the scan state machine. */
can_scan_state_t can_bus_test_get_state(void);

/** Index of the bitrate currently being tested (0-3). */
uint8_t can_bus_test_get_current_bitrate(void);

/** Read-only pointer to the scan report (valid after COMPLETE/CANCELLED). */
const can_scan_report_t *can_bus_test_get_report(void);

/**
 * Register a callback that fires (via lv_async_call) whenever the scan
 * state changes. The UI uses this to update the overlay.
 */
typedef void (*can_scan_ui_cb_t)(void);
void can_bus_test_set_ui_callback(can_scan_ui_cb_t cb);

#ifdef __cplusplus
}
#endif
