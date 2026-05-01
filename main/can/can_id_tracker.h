/*
 * can_id_tracker.h - Per-CAN-ID statistics for live diagnostics.
 *
 * Bounded table that records the last data bytes, frame count, and rolling
 * Hz of every CAN ID seen on the bus. Fed from can_process_queued_frames()
 * (LVGL task) and read by ui_can_list.c (LVGL task) - single-threaded
 * access, no mutex needed. New IDs are appended in arrival order; once the
 * table is full, additional IDs are silently dropped (logged once).
 *
 * Standard 11-bit and extended 29-bit IDs share the same table - the raw
 * uint32_t identifier is stored as-is.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_ID_TRACKER_MAX_IDS 64

typedef struct {
    uint32_t can_id;
    uint8_t  data[8];
    uint8_t  dlc;
    bool     extended;
    uint32_t rx_count;
    int64_t  first_seen_us;
    int64_t  last_seen_us;

    /* Hz sliding-window: recompute_hz() reads rx_count - rx_count_at_last_sample
     * over (now - last_sample_us). UI calls recompute_hz() once per second so
     * the displayed Hz is stable but the bytes refresh at the UI cadence. */
    uint32_t rx_count_at_last_sample;
    int64_t  last_sample_us;
    float    last_hz;
} can_id_entry_t;

/** Record a frame. Called from can_process_queued_frames() on LVGL task. */
void can_id_tracker_record(uint32_t can_id, bool extended,
                           const uint8_t *data, uint8_t dlc);

/** Wipe all tracked IDs. Called from the "Reset" button on the list UI. */
void can_id_tracker_reset(void);

/** Number of distinct IDs currently tracked. */
uint16_t can_id_tracker_count(void);

/** Read-only pointer to the entry at idx, or NULL if out of range. The
 *  pointer is stable for the lifetime of the table - safe to cache as
 *  long as the caller is on the LVGL task. */
const can_id_entry_t *can_id_tracker_get(uint16_t idx);

/** Recompute the rolling Hz for every entry. Cheap (linear over count).
 *  Call once per second from the UI before refreshing labels. */
void can_id_tracker_recompute_hz(void);

#ifdef __cplusplus
}
#endif
