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

    /* Bitmap of byte-0 values seen on this ID, for values 0..15 (bit N set =
     * a frame with byte 0 == N has arrived). Multiplexed ECUs cycle several
     * payloads through one ID and tag each with a frame index there — Link's
     * Generic Dash sends 14 of them on 0x3E8 — so ID presence alone tells
     * auto-detect almost nothing about such a stream. This is what lets it
     * score those frames individually. Values above 15 are not tracked: no
     * catalogued stream uses them, and 16 bits keeps the entry small.
     *
     * Assumes the mux field is byte 0. That is the near-universal convention
     * and all that preset auto-detect needs, but it is NOT what the decoder
     * supports — channel_manager_set_mux() accepts any start/length within
     * the frame. For an arbitrary mux field, use the focus buffer below.
     *
     * Sits in the two bytes of padding that already sat between `extended`
     * and `rx_count`, so it costs nothing; below `last_seen_us` it would
     * have grown the struct by 8 bytes each, 512 in total. */
    uint16_t mux_seen;

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

/* ── Focus buffer: per-mux-value payloads for ONE id ──────────────────────
 *
 * The main table keeps a single data[8] per ID — the most recent frame,
 * whatever its mux value. That is fine for a bus overview and useless for
 * building a multiplexed channel: on a stream cycling 14 payloads through
 * one ID, a poll catches a random one, so a live decode preview flickers
 * across unrelated quantities instead of showing the field being edited.
 *
 * The focus buffer fixes that for the ID the user is currently working on:
 * point it at (can_id, mux geometry) and it retains the latest payload for
 * each distinct mux value separately. One ID at a time, because holding
 * this for all 64 would cost 64x — and only one is ever being edited.
 *
 * The mux field is read with can_extract_bits(..., endian, false), matching
 * signal_dispatch_frame() exactly, so what the preview decodes is what the
 * running decoder will decode.
 * ────────────────────────────────────────────────────────────────────── */

#define CAN_ID_TRACKER_MAX_MUX_SLOTS 16

typedef struct {
    uint16_t mux_value;
    uint8_t  data[8];
    uint8_t  dlc;
    uint32_t rx_count;      /* frames seen carrying this mux value */
    int64_t  last_seen_us;
} can_mux_slot_t;

/** Point the focus buffer at an ID and mux geometry, discarding whatever it
 *  held. Pass mux_bit_length 0 to focus an ID without splitting by mux (one
 *  slot, mux_value 0). Safe to call from any task: this only publishes a
 *  request, which the recording task adopts on its next frame. Calling it
 *  with the same arguments is a no-op and does NOT discard collected slots,
 *  so a UI can call it on every poll. */
void can_id_tracker_set_focus(uint32_t can_id, bool extended,
                              uint8_t mux_bit_start, uint8_t mux_bit_length,
                              int endian);

/** Stop focusing. The buffer is emptied on the next recorded frame. */
void can_id_tracker_clear_focus(void);

/** The focused ID, or 0 when focus is off. */
uint32_t can_id_tracker_focus_id(void);

/** Number of distinct mux values collected for the focused ID (<= MAX). */
uint8_t can_id_tracker_focus_count(void);

/** Slot at idx, or NULL when out of range. Like can_id_tracker_get(), the
 *  pointer is only stable on the recording task; a reader on another task
 *  accepts the same benign torn read the bus monitor already documents. */
const can_mux_slot_t *can_id_tracker_focus_get(uint8_t idx);

#ifdef __cplusplus
}
#endif
