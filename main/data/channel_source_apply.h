/*
 * channel_source_apply.h — shared "bind a preconfig source to a channel"
 *                          path used by both the web /api/channels/bind-source
 *                          handler AND the first-run wizard's channels step.
 *
 * Until this module existed the two flows duplicated the apply logic:
 *   1. derive signal_name from preconfig label
 *   2. register/update the signal in the runtime registry
 *   3. persist the signal entry into the active layout's signals[]
 *   4. point the channel at the signal name
 *   5. resolve the channel's signal_index from the now-updated registry
 *
 * Having two copies meant adding a new preconfig (or a new field) had two
 * places to update; one would inevitably drift. Centralising it here means
 * the web modal and the on-device wizard apply preconfigs identically.
 *
 * Threading: caller must hold the LVGL mutex.
 */
#pragma once

#include "channel_manager.h"
#include "../ui/settings/preset_picker.h"  /* preconfig_item_t */
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bind the given preconfig source row to the given channel.
 *
 *   - Derives a signal_name from item->label using the same rule as the
 *     web channels endpoint (uppercase, runs of non-alnum collapse to "_").
 *   - Registers or updates the signal in the runtime registry so the
 *     decoded value starts flowing immediately (no layout-reload latency).
 *   - Persists the signal entry into the active layout's signals[] via
 *     ecu_preset_write_signal_to_layout(). Survives reboot.
 *   - Calls channel_manager_set_signal() + channel_manager_resolve_signals()
 *     so listeners (widgets, the wizard's row refresh timer) see the new
 *     binding right away.
 *
 * Returns ESP_OK on success. ESP_ERR_INVALID_ARG if c/item is NULL or the
 * derived signal name is empty. Other errors propagated from the
 * underlying layout-persist call.
 */
esp_err_t channel_apply_preconfig(channel_t *c, const preconfig_item_t *item);

#ifdef __cplusplus
}
#endif
