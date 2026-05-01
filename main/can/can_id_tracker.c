/*
 * can_id_tracker.c - Per-CAN-ID statistics. See header for design notes.
 */
#include "can_id_tracker.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "can_id_track";

static can_id_entry_t s_entries[CAN_ID_TRACKER_MAX_IDS];
static uint16_t       s_count = 0;
static bool           s_overflow_logged = false;

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

void can_id_tracker_record(uint32_t can_id, bool extended,
                           const uint8_t *data, uint8_t dlc)
{
    if (dlc > 8) dlc = 8;
    int64_t now = esp_timer_get_time();

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
    if (data && dlc > 0) memcpy(e->data, data, dlc);
    e->rx_count++;
    e->last_seen_us = now;
}

void can_id_tracker_reset(void)
{
    s_count = 0;
    s_overflow_logged = false;
    /* Don't bother memsetting - new entries zero themselves on insert. */
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
