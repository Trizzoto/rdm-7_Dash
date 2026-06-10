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
#include "cJSON.h"

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

/* ── Studio "full config" layout import ──────────────────────────────
 *
 * Portable layouts authored off-device (RDM Studio export, marketplace
 * .rdm bundles) carry channel-level config on their signals[] entries:
 * warn thresholds (low_warn / high_warn) and the full CAN decode. The
 * dash's own editor never emits those keys (buildFirmwarePayload strips
 * them — thresholds and decode are channel-owned on-device, ADR 0005),
 * so their presence in an incoming layout is the signature of an import
 * that should be adopted into /lfs/channels.json. */

/**
 * Cheap pre-scan: true when any signals[] entry carries importable
 * channel config (a numeric low_warn / high_warn, or a CAN decode).
 * Lets the save handler skip the LVGL lock + import walk entirely for
 * normal editor saves.
 */
bool channel_layout_signals_carry_config(const cJSON *signals_arr);

/**
 * Adopt channel config from a layout's signals[] array into the channel
 * manager (create→port loop, studio → dash):
 *
 *   - For each entry with thresholds or CAN decode, find the channel
 *     bound to that signal_name; if none exists, ensure one (ECU alias →
 *     canonical id match → auto custom_<name>, collision-guarded — same
 *     resolution as the first-run wizard's 100%-coverage contract).
 *   - Thresholds ALWAYS apply (an import is explicit user intent), then
 *     the low_warn/high_warn keys are STRIPPED from the cJSON entry so
 *     the persisted layout can't re-assert them over later on-device
 *     channel edits. Caller persists the mutated JSON afterwards.
 *   - Decode is adopted only when the channel has none (can_id == 0) —
 *     never clobbers device-edited decode (the ADR 0005 "stale decode
 *     wins" bug class). Without adoption, a custom signal's decode would
 *     be lost on the next editor save (Phase B strips signals[] decode).
 *
 * Persistence is bulk-guarded (one channels.json flush); the CAN
 * acceptance filter is rebuilt if any decode was adopted.
 *
 * Caller must hold the LVGL mutex. Returns the number of channels
 * touched.
 */
size_t channel_import_from_layout_signals(cJSON *signals_arr);

#ifdef __cplusplus
}
#endif
