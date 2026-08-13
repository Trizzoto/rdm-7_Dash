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

/* ── OBD2 scan → channels ────────────────────────────────────────────
 *
 * An OBD2 discovery scan answers with a list of supported Mode-01 PIDs.
 * On its own that's a list of hex bytes; what the user wants to know is
 * "which channels can this car feed me". That translation is one rule —
 * CANONICAL_OBD2_MAP read backwards, cross-referenced with what's already
 * in the channel set — and it lives here so the web endpoint, the desktop
 * (through that endpoint) and the dash's own editor all give the same
 * answer instead of three drifting copies. */

/* Upper bound on offers from one scan: the map has ~26 rows and grows
 * slowly, so callers can size a buffer once and never truncate. */
#define CH_OBD2_MATCH_MAX 40

typedef struct {
	const char *channel_id;   /* canonical id, e.g. "coolant_temp"     */
	const char *label;        /* human label from the canonical entry  */
	const char *units;        /* canonical native units ("" if none)   */
	const char *signal_name;  /* OBD2 signal name to bind the channel to */
	uint8_t     service;      /* OBD2 mode (1)                         */
	uint16_t    pid;          /* OBD2 PID                              */
	bool        active;       /* channel already in the live set       */
	bool        bound;        /* already has a source (don't re-bind)  */
} obd2_channel_match_t;

/**
 * Resolve a scan's answering PIDs into channel offers, in canonical map
 * order (stable across scans, so the list doesn't reshuffle on rescan).
 * PIDs with no canonical channel are skipped — a car answers plenty of
 * PIDs nobody wants on a dash.
 *
 * `pids` are raw Mode-01 PID bytes as returned by obd2_scan_result_t.
 * Caller must hold the LVGL mutex (reads the channel set).
 * Returns the number of rows written to `out`.
 */
size_t channel_obd2_matches(const uint8_t *pids, size_t n_pids,
                            obd2_channel_match_t *out, size_t max_out);

/**
 * Bind channels to their OBD2 PIDs, activating any that aren't in the
 * live set yet. One layout read + one save + one obd2_start for the whole
 * batch — binding twelve channels one at a time would rewrite the polled
 * PID list twelve times.
 *
 * Errors:
 *   ESP_ERR_INVALID_ARG — nothing to do / bad args
 *   ESP_ERR_NO_MEM      — the polled-PID list is full (OBD2_MAX_ENABLED)
 *   ESP_FAIL            — no active layout, or the save failed; the binds
 *                         are live for this session but won't survive a
 *                         reboot (caller should say so)
 * `*out_bound` receives how many channels actually got bound.
 * Caller must hold the LVGL mutex.
 */
esp_err_t channel_apply_obd2(const obd2_channel_match_t *rows, size_t n_rows,
                             size_t *out_bound);

/**
 * Drop polled OBD2 PIDs that no channel needs any more.
 *
 * Binding adds a PID to the polled set; unbinding never removed one, so the
 * set only ever grew — and it caps at OBD2_MAX_ENABLED (48). One channel at
 * a time that was a slow leak; a scan that adopts a dozen at once makes it a
 * real ceiling. Call after a channel is unbound or removed.
 *
 * Only prunes PIDs that map to a canonical channel (CANONICAL_OBD2_MAP) and
 * that no active channel is bound to. A PID enabled by hand for something
 * with no channel — the OBD2 PID picker, custom PIDs — is left alone: it was
 * a deliberate choice and nothing here can tell it isn't still wanted.
 *
 * Caller must hold the LVGL mutex. Returns the number of PIDs dropped.
 */
size_t channel_obd2_prune(void);

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
