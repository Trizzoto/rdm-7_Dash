/*
 * channel_source_apply.c — single source-bind path for channels.
 *
 * Both the web /api/channels/bind-source handler and the first-run
 * wizard's channels step call channel_apply_preconfig() here so any
 * future change to "what happens when you pick a preconfig source"
 * lands in one place. See the header for the contract.
 */

#include "channel_source_apply.h"
#include "../widgets/signal.h"
#include "../can/can_manager.h"
#include "../layout/ecu_presets.h"
#include "../layout/layout_manager.h"
#include "esp_log.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ch_src_apply";

/* Derive a signal_name from a preconfig label using the same rule as
 * web_server_channels.c::_derive_signal_name (which itself mirrors
 * config_modal.c::label_to_signal_name): uppercase, runs of non-alnum
 * collapse to single underscore, trim trailing underscores.
 *
 *   "ENGINE RPM"   -> "ENGINE_RPM"
 *   "WHEEL SPD FL" -> "WHEEL_SPD_FL"
 */
static void _derive_signal_name(const char *label, char *out, size_t sz) {
    if (!label || !out || sz == 0) {
        if (out && sz) out[0] = '\0';
        return;
    }
    size_t j = 0;
    for (size_t i = 0; label[i] && j < sz - 1; i++) {
        char c = label[i];
        if (isalnum((unsigned char)c))
            out[j++] = (char)toupper((unsigned char)c);
        else if (j > 0 && out[j - 1] != '_')
            out[j++] = '_';
    }
    while (j > 0 && out[j - 1] == '_') j--;
    out[j] = '\0';
}

/* Update or register the runtime signal entry so the channel binds to a
 * live decode immediately — without this, the layout-disk write is
 * invisible until the next layout reload, and resolve_signals() falls
 * back to whatever was previously in the registry. */
static esp_err_t _apply_runtime(const preconfig_item_t *item,
                                const char *signal_name) {
    if (!item || !signal_name || !signal_name[0]) return ESP_ERR_INVALID_ARG;

    uint32_t can_id = (uint32_t)strtol(item->can_id, NULL, 16);
    int16_t idx = signal_find_by_name(signal_name);
    if (idx >= 0) {
        signal_t *s = signal_get_by_index((uint16_t)idx);
        if (s) {
            s->can_id     = can_id;
            s->bit_start  = item->bit_start;
            s->bit_length = item->bit_length;
            s->scale      = item->scale;
            s->offset     = item->value_offset;
            s->is_signed  = item->is_signed;
            s->endian     = item->endianess;
            /* Reset decode freshness so the next frame fires
             * notify_subscribers — see signal_dispatch_frame's
             * was_stale/value gate. Without this an existing
             * non-stale slot whose current_value matches the new
             * decode silently swallows the first update. */
            s->is_stale       = true;
            s->current_value  = 0.0f;
            s->last_update_ms = 0;
        }
        return ESP_OK;
    }
    idx = signal_register(signal_name, can_id,
                          item->bit_start, item->bit_length,
                          item->scale, item->value_offset,
                          item->is_signed, item->endianess, "");
    return (idx >= 0) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t channel_apply_preconfig(channel_t *c, const preconfig_item_t *item) {
    if (!c || !item || !item->label) return ESP_ERR_INVALID_ARG;

    char signal_name[32];
    _derive_signal_name(item->label, signal_name, sizeof(signal_name));
    if (!signal_name[0]) return ESP_ERR_INVALID_ARG;

    /* 1. Update the runtime registry first so live values flow now. */
    esp_err_t rt = _apply_runtime(item, signal_name);
    if (rt != ESP_OK) {
        ESP_LOGW(TAG, "runtime apply failed for %s: %d", signal_name, rt);
        /* Fall through — disk write may still succeed; resolve_signals
         * will pick up the binding on the next layout reload. */
    }

    /* 2. Persist into the active layout's signals[] so the binding
     *    survives reboot. Reuse the shared ecu_preset writer — same
     *    field shape as the bulk-preset apply path. */
    char layout[64];
    esp_err_t le = layout_manager_get_active(layout, sizeof(layout));
    if (le == ESP_OK) {
        uint32_t can_id = (uint32_t)strtol(item->can_id, NULL, 16);
        esp_err_t we = ecu_preset_write_signal_to_layout(
            layout, signal_name,
            can_id, item->bit_start, item->bit_length,
            item->scale, item->value_offset,
            item->is_signed, item->endianess,
            /* unit: preconfig items don't carry a unit string */ NULL,
            item->decimals);
        if (we != ESP_OK) {
            ESP_LOGW(TAG, "layout persist failed for %s: %d", signal_name, we);
        }
    } else {
        ESP_LOGW(TAG, "no active layout (%d), skipping persist", le);
    }

    /* 3. Point the channel at the new signal name + resolve so
     *    signal_index updates and listeners refire. */
    channel_manager_set_signal(c, signal_name);
    channel_manager_resolve_signals();

    /* 4. Rebuild the TWAI hardware acceptance filter so frames with the
     *    new signal's CAN ID actually reach the receive task. Without
     *    this, when the user picks a source whose CAN ID isn't already
     *    in the filter mask (e.g. Battery Voltage on a different ID
     *    than the active ECU's other signals), the channel binds but no
     *    live value ever arrives — the frame gets dropped in hardware.
     *
     *    Skipped when the bus is in promiscuous mode (ACCEPT_ALL),
     *    which is the case during the wizard's ECU probe. */
    if (!can_is_promiscuous()) {
        reconfigure_can_filter();
    }

    ESP_LOGI(TAG, "channel '%s' <- preconfig %s/%s (%s) -> signal '%s'",
             c->id,
             item->ecu     ? item->ecu     : "?",
             item->version ? item->version : "?",
             item->label,
             signal_name);
    return ESP_OK;
}
