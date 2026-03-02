/*
 * can_dispatch.c — CAN ID dispatch table management.
 *
 * Owns the O(1) CAN-ID-to-widget-index lookup table.
 * rebuild_can_dispatch() rebuilds it from the current config arrays.
 *
 * can_dispatch_process_frame() is the single entry point called by the
 * CAN receive task; the per-frame routing logic lives in
 * process_can_message() (ui_Screen3.c) which has full access to all
 * static UI state and the LVGL objects.
 */
#include "can_dispatch.h"
#include "ui/screens/ui_Screen3.h"   /* warning_configs, indicator_configs, values_config */
#include <string.h>

/* Forward declaration — process_can_message is defined in ui_Screen3.c */
extern void process_can_message(const twai_message_t *message);

/* ── Dispatch table storage ─────────────────────────────────────────── */

can_dispatch_entry_t can_dispatch_entries[CAN_DISPATCH_MAX_ENTRIES];
int                  can_dispatch_count = 0;
int16_t              can_id_to_dispatch_index[CAN_DISPATCH_ID_TABLE_SZ];

/* ── Internal helpers ────────────────────────────────────────────────── */

static int get_or_add_entry(uint32_t can_id)
{
    uint32_t sid = can_id & 0x7FFu;
    if (sid < CAN_DISPATCH_ID_TABLE_SZ) {
        int16_t idx = can_id_to_dispatch_index[sid];
        if (idx >= 0) return idx;
    }
    if (can_dispatch_count >= CAN_DISPATCH_MAX_ENTRIES) return -1;
    int idx = can_dispatch_count++;
    can_dispatch_entries[idx].can_id        = sid;
    can_dispatch_entries[idx].num_warning   = 0;
    can_dispatch_entries[idx].num_indicator = 0;
    can_dispatch_entries[idx].num_values    = 0;
    if (sid < CAN_DISPATCH_ID_TABLE_SZ)
        can_id_to_dispatch_index[sid] = (int16_t)idx;
    return idx;
}

/* ── Public API ──────────────────────────────────────────────────────── */

void rebuild_can_dispatch(void)
{
    for (int i = 0; i < CAN_DISPATCH_ID_TABLE_SZ; i++)
        can_id_to_dispatch_index[i] = -1;
    can_dispatch_count = 0;

    for (int i = 0; i < 8; i++) {
        if (warning_configs[i].can_id == 0) continue;
        int idx = get_or_add_entry(warning_configs[i].can_id);
        if (idx >= 0 && can_dispatch_entries[idx].num_warning < 8)
            can_dispatch_entries[idx].warning_indices[
                can_dispatch_entries[idx].num_warning++] = (uint8_t)i;
    }
    for (int i = 0; i < 2; i++) {
        if (indicator_configs[i].can_id == 0) continue;
        int idx = get_or_add_entry(indicator_configs[i].can_id);
        if (idx >= 0 && can_dispatch_entries[idx].num_indicator < 2)
            can_dispatch_entries[idx].indicator_indices[
                can_dispatch_entries[idx].num_indicator++] = (uint8_t)i;
    }
    for (int i = 0; i < 13; i++) {
        if (!values_config[i].enabled || values_config[i].can_id == 0) continue;
        int idx = get_or_add_entry(values_config[i].can_id);
        if (idx >= 0 && can_dispatch_entries[idx].num_values < 13)
            can_dispatch_entries[idx].value_indices[
                can_dispatch_entries[idx].num_values++] = (uint8_t)i;
    }
}

void can_dispatch_process_frame(const twai_message_t *msg)
{
    process_can_message(msg);
}
