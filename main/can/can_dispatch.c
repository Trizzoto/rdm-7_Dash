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
#include "widgets/widget_registry.h" /* widget_registry_snapshot, widget_t */
#include "widgets/widget_meter.h"    /* widget_meter_get_value_idx */
#include <string.h>

/* Forward declaration — process_can_message is defined in ui_Screen3.c */
extern void process_can_message(const twai_message_t *message);

/* ── Dispatch table storage ─────────────────────────────────────────── */

can_dispatch_entry_t can_dispatch_entries[CAN_DISPATCH_MAX_ENTRIES];
int                  can_dispatch_count = 0;
int16_t              can_id_to_dispatch_index[CAN_DISPATCH_ID_TABLE_SZ];

/* Text/meter widgets by value index — rebuilt by rebuild_can_dispatch ───── */
static widget_t *s_text_by_value[13][TEXT_PER_VALUE_MAX];
static uint8_t   s_num_text_by_value[13];
static widget_t *s_meter_by_value[13][METER_PER_VALUE_MAX];
static uint8_t   s_num_meter_by_value[13];

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

    /* Discover which configuration slots are actually present on the current
     * layout by inspecting the widget registry.  This lets us keep the CAN
     * dispatch table tightly focused on live widgets without any per-frame
     * string lookups. */

    bool panel_slot_used[8]     = {false};
    bool warning_slot_used[8]   = {false};
    bool indicator_slot_used[2] = {false};
    bool bar_slot_used[2]       = {false};
    bool text_value_used[13]    = {false};
    bool meter_value_used[13]   = {false};
    bool rpm_used               = false;
    bool speed_used             = false;
    bool gear_used              = false;

    memset(s_num_text_by_value,  0, sizeof(s_num_text_by_value));
    memset(s_num_meter_by_value, 0, sizeof(s_num_meter_by_value));

    widget_t *widgets[WIDGET_REGISTRY_MAX];
    uint8_t widget_count = 0;
    widget_registry_snapshot(widgets, WIDGET_REGISTRY_MAX, &widget_count);

    for (uint8_t i = 0; i < widget_count; i++) {
        widget_t *w = widgets[i];
        if (!w)
            continue;

        switch (w->type) {
        case WIDGET_PANEL: {
            uint8_t slot = (uint8_t)(uintptr_t)w->type_data;
            if (slot < 8)
                panel_slot_used[slot] = true;
            break;
        }
        case WIDGET_WARNING: {
            uint8_t slot = (uint8_t)(uintptr_t)w->type_data;
            if (slot < 8)
                warning_slot_used[slot] = true;
            break;
        }
        case WIDGET_INDICATOR: {
            uint8_t slot = (uint8_t)(uintptr_t)w->type_data;
            if (slot < 2)
                indicator_slot_used[slot] = true;
            break;
        }
        case WIDGET_BAR: {
            uint8_t slot = (uint8_t)(uintptr_t)w->type_data;
            if (slot < 2)
                bar_slot_used[slot] = true;
            break;
        }
        case WIDGET_TEXT: {
            uint8_t vidx = (uint8_t)(uintptr_t)w->type_data;
            if (vidx < 13) {
                text_value_used[vidx] = true;
                if (s_num_text_by_value[vidx] < TEXT_PER_VALUE_MAX) {
                    s_text_by_value[vidx][s_num_text_by_value[vidx]++] = w;
                }
            }
            break;
        }
        case WIDGET_METER: {
            uint8_t vidx = widget_meter_get_value_idx(w);
            if (vidx < 13) {
                meter_value_used[vidx] = true;
                if (s_num_meter_by_value[vidx] < METER_PER_VALUE_MAX) {
                    s_meter_by_value[vidx][s_num_meter_by_value[vidx]++] = w;
                }
            }
            break;
        }
        case WIDGET_RPM_BAR:
            rpm_used = true;
            break;
        case WIDGET_SPEED:
            speed_used = true;
            break;
        case WIDGET_GEAR:
            gear_used = true;
            break;
        default:
            break;
        }
    }

    /* Populate dispatch entries only for widgets that are both configured and
     * present on the current layout.  This keeps the dispatcher in sync with
     * JSON layouts while preserving the existing O(1) integer lookup path. */

    /* Warnings: 8 slots */
    for (int i = 0; i < 8; i++) {
        if (!warning_slot_used[i])
            continue;
        if (warning_configs[i].can_id == 0)
            continue;
        int idx = get_or_add_entry(warning_configs[i].can_id);
        if (idx >= 0 && can_dispatch_entries[idx].num_warning < 8)
            can_dispatch_entries[idx].warning_indices[
                can_dispatch_entries[idx].num_warning++] = (uint8_t)i;
    }

    /* Indicators: 2 slots */
    for (int i = 0; i < 2; i++) {
        if (!indicator_slot_used[i])
            continue;
        if (indicator_configs[i].can_id == 0)
            continue;
        int idx = get_or_add_entry(indicator_configs[i].can_id);
        if (idx >= 0 && can_dispatch_entries[idx].num_indicator < 2)
            can_dispatch_entries[idx].indicator_indices[
                can_dispatch_entries[idx].num_indicator++] = (uint8_t)i;
    }

    /* Values: 13 slots.
     *
     * Index mapping (kept in sync with config_store.c):
     *   0–7  : panel value slots
     *   8    : RPM_VALUE_ID   (9)
     *   9    : SPEED_VALUE_ID (10)
     *   10   : GEAR_VALUE_ID  (11)
     *   11   : BAR1_VALUE_ID  (12)
     *   12   : BAR2_VALUE_ID  (13)
     */
#define RPM_VALUE_IDX   8
#define SPEED_VALUE_IDX 9
#define GEAR_VALUE_IDX  10
#define BAR1_VALUE_IDX  11
#define BAR2_VALUE_IDX  12

    for (int i = 0; i < 13; i++) {
        /* Skip slots that have no corresponding widget instance on the layout. */
        if (i < 8 && !panel_slot_used[i] && !text_value_used[i] && !meter_value_used[i])
            continue;
        if (i == RPM_VALUE_IDX && !rpm_used && !text_value_used[i] && !meter_value_used[i])
            continue;
        if (i == SPEED_VALUE_IDX && !speed_used && !text_value_used[i] && !meter_value_used[i])
            continue;
        if (i == GEAR_VALUE_IDX && !gear_used && !text_value_used[i] && !meter_value_used[i])
            continue;
        if (i == BAR1_VALUE_IDX && !bar_slot_used[0] && !text_value_used[i] && !meter_value_used[i])
            continue;
        if (i == BAR2_VALUE_IDX && !bar_slot_used[1] && !text_value_used[i] && !meter_value_used[i])
            continue;

        if (!values_config[i].enabled || values_config[i].can_id == 0)
            continue;
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

void can_dispatch_get_text_widgets_for_value(uint8_t value_idx,
        widget_t **out, uint8_t max, uint8_t *count)
{
    if (!out || !count || value_idx >= 13) {
        if (count) *count = 0;
        return;
    }
    uint8_t n = s_num_text_by_value[value_idx];
    if (n > max) n = max;
    for (uint8_t i = 0; i < n; i++)
        out[i] = s_text_by_value[value_idx][i];
    *count = n;
}

void can_dispatch_get_meter_widgets_for_value(uint8_t value_idx,
        widget_t **out, uint8_t max, uint8_t *count)
{
    if (!out || !count || value_idx >= 13) {
        if (count) *count = 0;
        return;
    }
    uint8_t n = s_num_meter_by_value[value_idx];
    if (n > max) n = max;
    for (uint8_t i = 0; i < n; i++)
        out[i] = s_meter_by_value[value_idx][i];
    *count = n;
}
