/*
 * can_id_tracker.c - Per-CAN-ID statistics. See header for design notes.
 */
#include "can_id_tracker.h"
#include "can_decode.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "can_id_track";

static EXT_RAM_BSS_ATTR can_id_entry_t s_entries[CAN_ID_TRACKER_MAX_IDS];
static uint16_t       s_count = 0;
static bool           s_overflow_logged = false;

/* ── Focus buffer ─────────────────────────────────────────────────────────
 *
 * Split across two halves to keep it lock-free:
 *
 *   REQUEST  — written by whichever task calls set_focus (the HTTP task, in
 *              practice), read by the recording task. Word-sized scalars
 *              plus a sequence counter that is bumped LAST, so the reader
 *              either sees the whole previous request or the whole new one.
 *   LIVE     — owned exclusively by the recording task. Slots are only ever
 *              allocated, written and reset there, so no cross-task write
 *              can corrupt the count while frames are landing.
 *
 * A reader on another task (the monitor endpoint) reads LIVE without a lock,
 * accepting the same benign torn read _can_monitor_handler already documents
 * for the main table: worst case is one momentarily-mixed frame in a
 * diagnostic view. */
static EXT_RAM_BSS_ATTR can_mux_slot_t s_focus_slots[CAN_ID_TRACKER_MAX_MUX_SLOTS];

/* REQUEST half */
static volatile uint32_t s_focus_req_id     = 0;
static volatile bool     s_focus_req_ext    = false;
static volatile uint8_t  s_focus_req_start  = 0;
static volatile uint8_t  s_focus_req_len    = 0;
static volatile int      s_focus_req_endian = 1;
static volatile uint32_t s_focus_req_seq    = 0;
static volatile bool     s_focus_req_on     = false;

/* LIVE half — recording task only. */
static uint32_t s_focus_seen_seq = 0;
static uint32_t s_focus_id       = 0;
static bool     s_focus_ext      = false;
static uint8_t  s_focus_start    = 0;
static uint8_t  s_focus_len      = 0;
static int      s_focus_endian   = 1;
static uint8_t  s_focus_count    = 0;
static bool     s_focus_on       = false;
static bool     s_focus_full_logged = false;

/* Linear scan - 64 entries fits in a couple cache lines and is plenty fast
 * even at 2 kHz aggregate frame rate. A hashmap would be premature here. */
static int _find_idx(uint32_t can_id, bool extended)
{
    for (uint16_t i = 0; i < s_count; i++) {
        if (s_entries[i].can_id == can_id && s_entries[i].extended == extended)
            return (int)i;
    }
    return -1;
}

/* Adopt a pending focus request. Recording task only. Resetting the slots
 * here — rather than in set_focus() — is what keeps the buffer lock-free:
 * the count only ever changes on the task that reads it while appending. */
static void _focus_sync_request(void)
{
    uint32_t seq = s_focus_req_seq;
    if (seq == s_focus_seen_seq) return;
    s_focus_seen_seq = seq;

    s_focus_on     = s_focus_req_on;
    s_focus_id     = s_focus_req_id;
    s_focus_ext    = s_focus_req_ext;
    s_focus_start  = s_focus_req_start;
    s_focus_len    = s_focus_req_len;
    s_focus_endian = s_focus_req_endian;

    /* New target — everything collected described the old one. */
    s_focus_count = 0;
    s_focus_full_logged = false;
}

/* Store this frame under its mux value. Recording task only. */
static void _focus_record(const uint8_t *data, uint8_t dlc, int64_t now)
{
    uint16_t mux = 0;
    if (s_focus_len) {
        /* Same guard and same extraction the dispatcher uses, so a frame the
         * decoder would skip never shows up in the preview as if it decoded. */
        uint8_t mux_end = (uint8_t)((s_focus_start + s_focus_len - 1) / 8);
        if (dlc <= mux_end) return;
        mux = (uint16_t)can_extract_bits(data, s_focus_start, s_focus_len,
                                         s_focus_endian, false);
    }

    can_mux_slot_t *slot = NULL;
    for (uint8_t i = 0; i < s_focus_count; i++) {
        if (s_focus_slots[i].mux_value == mux) { slot = &s_focus_slots[i]; break; }
    }
    if (!slot) {
        if (s_focus_count >= CAN_ID_TRACKER_MAX_MUX_SLOTS) {
            /* More distinct mux values than slots. Keep the ones already
             * collected rather than thrashing — a stream with >16 frame
             * indices is outside anything catalogued, and silently rotating
             * would make the preview flicker for no gain. */
            if (!s_focus_full_logged) {
                ESP_LOGW(TAG, "focus 0x%03lX: >%d mux values - extras ignored",
                         (unsigned long)s_focus_id, CAN_ID_TRACKER_MAX_MUX_SLOTS);
                s_focus_full_logged = true;
            }
            return;
        }
        slot = &s_focus_slots[s_focus_count++];
        memset(slot, 0, sizeof(*slot));
        slot->mux_value = mux;
    }

    memcpy(slot->data, data, dlc);
    slot->dlc = dlc;
    slot->rx_count++;
    slot->last_seen_us = now;
}

void can_id_tracker_record(uint32_t can_id, bool extended,
                           const uint8_t *data, uint8_t dlc)
{
    if (dlc > 8) dlc = 8;
    int64_t now = esp_timer_get_time();

    _focus_sync_request();
    if (s_focus_on && can_id == s_focus_id && extended == s_focus_ext
        && data && dlc > 0) {
        _focus_record(data, dlc, now);
    }

    int idx = _find_idx(can_id, extended);
    if (idx < 0) {
        if (s_count >= CAN_ID_TRACKER_MAX_IDS) {
            if (!s_overflow_logged) {
                ESP_LOGW(TAG, "ID table full (%d) - additional IDs ignored",
                         CAN_ID_TRACKER_MAX_IDS);
                s_overflow_logged = true;
            }
            return;
        }
        idx = (int)s_count++;
        can_id_entry_t *e = &s_entries[idx];
        memset(e, 0, sizeof(*e));
        e->can_id        = can_id;
        e->extended      = extended;
        e->first_seen_us = now;
        e->last_sample_us = now;
    }

    can_id_entry_t *e = &s_entries[idx];
    e->dlc = dlc;
    if (data && dlc > 0) {
        memcpy(e->data, data, dlc);
        /* Remember which frame indices this ID has carried. The stored data[]
         * only ever holds the LAST frame, so without this a multiplexed
         * stream is indistinguishable from a single-payload one. */
        if (data[0] < 16) e->mux_seen |= (uint16_t)(1u << data[0]);
    }
    e->rx_count++;
    e->last_seen_us = now;
}

void can_id_tracker_reset(void)
{
    s_count = 0;
    s_overflow_logged = false;
    /* Don't bother memsetting - new entries zero themselves on insert. */

    /* Re-arm the focus buffer through the request path rather than zeroing
     * s_focus_count here: reset is called from the UI/HTTP side, and the
     * count belongs to the recording task. Bumping the sequence makes it
     * clear its own slots on the next frame. */
    s_focus_req_seq++;
}

void can_id_tracker_set_focus(uint32_t can_id, bool extended,
                              uint8_t mux_bit_start, uint8_t mux_bit_length,
                              int endian)
{
    if (mux_bit_length > 16 || mux_bit_start > 63 ||
        (uint16_t)mux_bit_start + mux_bit_length > 64) {
        ESP_LOGW(TAG, "set_focus rejected: start=%u len=%u",
                 mux_bit_start, mux_bit_length);
        return;
    }
    /* Idempotent: a UI polling every second must not wipe the slots it is
     * about to render just by asking for the same thing again. */
    if (s_focus_req_on && s_focus_req_id == can_id &&
        s_focus_req_ext == extended && s_focus_req_start == mux_bit_start &&
        s_focus_req_len == mux_bit_length && s_focus_req_endian == endian) {
        return;
    }

    s_focus_req_id     = can_id;
    s_focus_req_ext    = extended;
    s_focus_req_start  = mux_bit_start;
    s_focus_req_len    = mux_bit_length;
    s_focus_req_endian = endian;
    s_focus_req_on     = true;
    s_focus_req_seq++;          /* published LAST — see the design note above */
}

void can_id_tracker_clear_focus(void)
{
    if (!s_focus_req_on) return;
    s_focus_req_on = false;
    s_focus_req_id = 0;
    s_focus_req_seq++;
}

uint32_t can_id_tracker_focus_id(void)
{
    return s_focus_on ? s_focus_id : 0;
}

uint8_t can_id_tracker_focus_count(void)
{
    return s_focus_on ? s_focus_count : 0;
}

const can_mux_slot_t *can_id_tracker_focus_get(uint8_t idx)
{
    if (!s_focus_on || idx >= s_focus_count) return NULL;
    return &s_focus_slots[idx];
}

uint16_t can_id_tracker_count(void)
{
    return s_count;
}

const can_id_entry_t *can_id_tracker_get(uint16_t idx)
{
    if (idx >= s_count) return NULL;
    return &s_entries[idx];
}

void can_id_tracker_recompute_hz(void)
{
    int64_t now = esp_timer_get_time();
    for (uint16_t i = 0; i < s_count; i++) {
        can_id_entry_t *e = &s_entries[i];
        int64_t dt_us = now - e->last_sample_us;
        if (dt_us <= 0) continue;
        uint32_t delta = e->rx_count - e->rx_count_at_last_sample;
        e->last_hz = (float)delta * 1e6f / (float)dt_us;
        e->rx_count_at_last_sample = e->rx_count;
        e->last_sample_us = now;
    }
}
