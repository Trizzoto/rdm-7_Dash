/*
 * channel_manager.h — runtime channel state + listener pattern.
 *
 * The channel manager owns the live channel_t instances on the dash. It
 * loads channel configurations from /lfs/channels.json at boot, exposes
 * them to widgets via subscribe/lookup, and persists changes back to
 * LittleFS on edit.
 *
 * Channels are either:
 *   - Canonical (id matches an entry in canonical_channels.h)
 *   - Custom    (id starts with "custom_")
 *
 * Widgets reference channels by id at load. The manager snapshots the
 * channel's values into the widget's data struct, and the widget
 * subscribes to channel-changed events so user edits to the channel
 * (in the Channels tab) propagate live.
 *
 * Hot path stays unchanged: widgets still subscribe to SIGNALS for
 * per-frame value updates. The channel layer is metadata + threshold
 * evaluation + UI configuration, not a per-frame dispatch step.
 *
 * Threading:
 *   - All channel_manager API calls happen on the LVGL task.
 *   - The JSON save is debounced and runs on the LVGL task too (uses
 *     lv_async_call to defer writes off the editor edit handler).
 *   - The signal pipeline (channel value updates from CAN/OBD2) runs
 *     on the LVGL task via the existing can_process_queued_frames path.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "canonical_channels.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward decl */
typedef struct channel channel_t;
typedef struct channel_listener channel_listener_t;

/* Listener callback signature. Fired when ANY field of the channel
 * changes (range, thresholds, signal binding, colors). Widgets typically
 * re-snapshot the fields they care about and invalidate. */
typedef void (*channel_changed_cb)(channel_t *c, void *user_data);

struct channel_listener {
	channel_changed_cb  cb;
	void               *user_data;
	channel_listener_t *next;
};

struct channel {
	/* Identity */
	char     id[32];                 /* canonical id or "custom_..." */
	char     label[32];              /* user-overridable; default = canonical label */
	uint8_t  tier;
	channel_group_t group;
	channel_cardinality_t card;

	/* True if this channel was created from a canonical_channel_def_t.
	 * False for custom channels. */
	bool     is_canonical;

	/* Source — which signal feeds the live value */
	char     signal_name[32];
	int16_t  signal_index;           /* -1 if unbound */

	/* CAN decode (ADR 0005) — the channel OWNS its decode so it is
	 * device-local and survives layout changes / sharing. Meaningful only for
	 * CAN-sourced channels; can_id == 0 means "no decode" (internal/OBD2/unset).
	 * channel_manager_register_decoded_signals() registers a registry signal
	 * named signal_name with these params so signal_dispatch_frame decodes it. */
	uint32_t can_id;
	uint8_t  bit_start;
	uint8_t  bit_length;
	float    decode_scale;
	float    decode_offset;
	bool     is_signed;
	uint8_t  endian;                 /* 0 = Motorola (big), 1 = Intel (little) */
	char     decode_unit[8];

	/* Format */
	char     units_native[8];
	char     units_display[8];
	uint8_t  decimals;

	/* Range — float so the UI can carry decimals (boost 1.5 bar, lambda 0.85) */
	float    min, max;
	float    sanity_min, sanity_max;

	/* Threshold profile — sentinel values when unset (see canonical_channels.h).
	 * High/low warn only; the "critical" tier was retired. */
	float    low_warn;
	float    high_warn;

	/* Zone colors — CHANNEL_USE_DEFAULT_COLOR for "use convention" */
	uint32_t color_normal;
	uint32_t color_low_warn;
	uint32_t color_high_warn;

	/* Math/derived source (boost = MAP − BARO, oil − 50, MAP × 1.6 …).
	 * When enabled, the channel_math evaluator computes  <a> <op> <b>  and
	 * pushes the result into this channel's own synthetic signal (MATH_<ID>,
	 * registered at load — see channel_math.h). Each operand is either a
	 * CHANNEL id (not a signal name, so re-decoding an operand doesn't break
	 * the math) or a numeric CONSTANT. Unit handling lives in the evaluator:
	 * channel operands commensurate to a common unit and the result converts
	 * to this channel's units_native when the conversion is known. */
	bool     math_enabled;
	char     math_a[32];             /* operand channel id ("" when const) */
	char     math_b[32];             /* operand channel id ("" when const) */
	bool     math_a_is_const;
	bool     math_b_is_const;
	float    math_a_const;
	float    math_b_const;
	uint8_t  math_op;                /* channel_math_op_t: 0 + / 1 - / 2 * / 3 ÷ */

	/* Live state */
	float    current_value;
	bool     is_stale;
	uint32_t last_update_ms;
	channel_zone_t last_zone;        /* tracked so we only fire zone-event on transition */

	/* Listeners */
	channel_listener_t *listeners;
	uint8_t              listener_count;

	/* Calculated-channel hook (NULL for plain signal-fed) */
	void (*calculate_fn)(channel_t *self);
};

/* ── Lifecycle ────────────────────────────────────────────────────── */

/**
 * Initialise the manager. Loads /lfs/channels.json if present, otherwise
 * seeds a Generic OBD2 default channel set so the dash always has
 * something to render.
 *
 * Must be called once after LittleFS is mounted and before any layout
 * is loaded. dashboard_init() resolution path:
 *   mount_lfs → channel_manager_init → layout_manager_load
 */
esp_err_t channel_manager_init(void);

/**
 * Tear down — release all channels + listeners. Call before reboot or
 * full re-init. Saves any pending dirty state first.
 */
void channel_manager_shutdown(void);

/* ── Lookup ───────────────────────────────────────────────────────── */

/** Find a channel by id. Returns NULL if not present in the manager. */
channel_t *channel_manager_get(const char *id);

/** Total number of channels currently active in the manager. */
size_t channel_manager_count(void);

/** Hard cap on active channels (CHM_MAX). Lets callers and the web UI
 * surface "N of MAX used" and distinguish cap-exhaustion from bad ids. */
size_t channel_manager_capacity(void);

/**
 * Format a channel value for DISPLAY in a widget. When the channel's
 * display unit differs from its native unit (and a conversion is known),
 * converts @p native_value into the display unit and numeric-formats it with
 * @p decimals. Otherwise defers to signal_format_value(@p signal_index, …)
 * so value→label maps (gear/mode) and other signal formatting still apply.
 * Pass @p c == NULL to behave exactly like signal_format_value().
 *
 * Thresholds/zones stay native — only the rendered NUMBER converts; a
 * gauge's needle/fill position is ratio-preserving so it needs no change.
 */
void channel_format_display_value(const channel_t *c, int16_t signal_index,
                                  float native_value, uint8_t decimals,
                                  char *buf, size_t cap);

/** Iterate: get channel at index 0..count-1, or NULL if out of range. */
channel_t *channel_manager_at(size_t idx);

/* ── Mutation ─────────────────────────────────────────────────────── */

/**
 * Activate a canonical channel by id. Looks up the canonical definition,
 * snapshots its defaults into a new channel_t, and adds it to the
 * manager. If the channel is already activated, returns the existing
 * instance (idempotent).
 *
 * Returns NULL if the id is not canonical (use channel_manager_create_custom
 * for custom channels).
 */
channel_t *channel_manager_activate(const char *canonical_id);

/**
 * Create a custom (user-defined) channel. id must start with "custom_".
 * Returns NULL on collision (id already exists) or invalid id.
 */
channel_t *channel_manager_create_custom(
	const char *id, const char *label, channel_group_t group,
	channel_cardinality_t card,
	const char *units_native, const char *units_display, uint8_t decimals,
	float min, float max);

/** Remove a channel by id. Returns false if not found or builtin-locked. */
bool channel_manager_delete(const char *id);

/* ── Field mutation (drives listener notifications + dirty flag) ──── */

bool channel_manager_set_label(channel_t *c, const char *label);
bool channel_manager_set_signal(channel_t *c, const char *signal_name);
bool channel_manager_set_units_display(channel_t *c, const char *units);
/* Set the channel's NATIVE (source/received) unit — what the ECU is actually
 * transmitting. Re-expresses range/threshold/sanity values from the old native
 * unit into the new one so they keep their physical meaning. units_display
 * (the convert-to unit) is independent and unchanged. */
bool channel_manager_set_units_native(channel_t *c, const char *units);
bool channel_manager_set_decimals(channel_t *c, uint8_t decimals);
bool channel_manager_set_range(channel_t *c, float min, float max);
bool channel_manager_set_sanity(channel_t *c, float smin, float smax);
bool channel_manager_set_threshold(channel_t *c, channel_zone_t zone, float value);
bool channel_manager_clear_threshold(channel_t *c, channel_zone_t zone);
bool channel_manager_set_zone_color(channel_t *c, channel_zone_t zone, uint32_t color);

/**
 * Set the channel's CAN decode (ADR 0005). Stores the bit-field decode on the
 * channel (device-local, authoritative) and, when can_id != 0 and the channel
 * has a signal_name, UPSERTs a matching runtime signal so the bus decodes live
 * immediately + (re)binds the channel's subscription. Persists synchronously
 * (decode edits are rare, high-value — abrupt 12V loss gives no graceful
 * shutdown window, mirroring channel_manager_set_signal). Returns false on a
 * NULL channel.
 *
 * persist_now: true → flush channels.json synchronously (single high-value
 * edits, e.g. the web decode editor). false → mark dirty + debounce (bulk
 * callers like the ECU wizard that set many channels in a loop, or paths that
 * already flushed via set_signal — avoids a redundant per-channel full-file
 * rewrite under the LVGL lock).
 */
bool channel_manager_set_decode(channel_t *c, uint32_t can_id,
                                uint8_t bit_start, uint8_t bit_length,
                                float scale, float offset, bool is_signed,
                                uint8_t endian, const char *unit,
                                bool persist_now);

/**
 * Reset a channel to its canonical defaults (no-op for custom channels).
 * Returns false if the channel isn't canonical or isn't found.
 */
bool channel_manager_reset_to_defaults(channel_t *c);

/* ── Persistence ──────────────────────────────────────────────────── */

/**
 * Load channels from /lfs/channels.json. Returns ESP_ERR_NOT_FOUND if
 * the file doesn't exist — caller (channel_manager_init) seeds defaults
 * in that case.
 */
esp_err_t channel_manager_load_from_lfs(void);

/**
 * Save the current channel state to /lfs/channels.json synchronously.
 * Defaults-only emit: any field equal to the canonical default is
 * omitted, keeping the JSON small.
 */
esp_err_t channel_manager_save_to_lfs(void);

/**
 * Backup/restore the whole channel registry as the raw channels.json bytes.
 * Channel config is device-local (ADR-0005) and otherwise unrecoverable on a
 * hardware swap or FS corruption.
 *
 * export: flushes pending edits, returns a malloc'd NUL-terminated copy of the
 *   on-disk file in *out_buf (caller frees); *out_len gets the byte length.
 * import: validates (parses, has a "channels" array within the cap, loadable
 *   schema_version) then atomically replaces the file. It does NOT mutate the
 *   live registry — the caller MUST reboot for it to take effect.
 */
esp_err_t channel_manager_export_raw(char **out_buf, size_t *out_len);
esp_err_t channel_manager_import_raw(const char *json, size_t len);

/**
 * Mark the manager dirty and queue a debounced save (500 ms). Multiple
 * rapid edits coalesce into one disk write. Called automatically by
 * all the set_* mutation functions.
 */
void channel_manager_mark_dirty(void);

/**
 * Flush any pending dirty state to disk synchronously NOW. Stops the
 * debounce timer and, if dirty, calls channel_manager_save_to_lfs().
 *
 * Registered as an esp_register_shutdown_handler at init so every clean
 * esp_restart()/reboot path persists pending edits. An automotive dash
 * can also lose 12V abruptly at key-off (no graceful shutdown), so
 * high-value low-frequency edits (signal bindings) flush immediately
 * via this path rather than waiting on the debounce window.
 */
void channel_manager_flush(void);

/**
 * Bulk-edit guard. Between begin/end, all mutation persistence is deferred:
 * set_signal / set_decode / mark_dirty only mark the manager dirty rather than
 * writing channels.json. end_bulk performs ONE synchronous flush if anything
 * changed. Reentrant (depth-counted) — nested begin/end pairs balance, only the
 * outermost end flushes.
 *
 * Wrap bulk operations (e.g. the first-run wizard's ECU apply, which binds
 * dozens of channels) in begin/end to avoid one full-file LittleFS write per
 * edited channel — tens-to-hundreds of redundant writes that stall the LVGL
 * task and overflow the CAN RX queue. ALWAYS pair begin with end (no early
 * return in between) or persistence stays suppressed.
 */
void channel_manager_begin_bulk(void);
void channel_manager_end_bulk(void);

/* ── Listeners ────────────────────────────────────────────────────── */

void channel_manager_subscribe(channel_t *c, channel_changed_cb cb, void *user);
void channel_manager_unsubscribe(channel_t *c, channel_changed_cb cb, void *user);

/* ── Signal pipeline ──────────────────────────────────────────────── */

/**
 * Called by the signal dispatch path when a signal updates. Manager
 * walks channels bound to this signal, applies sanity bounds, updates
 * their current_value + is_stale, evaluates zone transitions, and
 * fires channel-changed events to listeners only when something
 * UI-visible changed (zone change, staleness flip).
 *
 * Returns the number of channels updated (mostly for diagnostics).
 */
size_t channel_manager_dispatch_signal(uint16_t signal_index, float value, bool stale);

/**
 * Re-resolve signal_name → signal_index for all channels. Called after
 * the signal registry has changed (layout reload, ECU preset change).
 */
void channel_manager_resolve_signals(void);

/**
 * Register a runtime signal for every channel that owns a CAN decode (ADR
 * 0005), then subscribe each channel to it. This makes the signal-dispatch
 * engine channel-driven so the bus is decoded from channels.json rather than
 * the layout's signals[]. UPSERTs, so calling it after a layout's _load_signals
 * makes the channel's decode authoritative over any embedded layout signal of
 * the same name. Safe to call repeatedly.
 */
void channel_manager_register_decoded_signals(void);

/**
 * One-time v2->v3 migration: seed each channel's decode from the currently
 * registered signal of the same name (i.e. from the active layout's signals[]),
 * so existing devices move their decode into channels.json. MUST run after the
 * active layout's _load_signals has populated the registry. No-op once the file
 * is at schema v3. Persists on change.
 */
void channel_manager_migrate_decode_from_registry(void);

/* ── Backwards-compatible migration helpers ──────────────────────── */

/**
 * Migration sweep state — passed by widgets when reporting legacy
 * values. Any field set to one of the *_UNSET sentinels is treated as
 * "widget didn't have this configured; use canonical default".
 */
typedef struct {
	const char *signal_name;     /* widget's signal binding (required) */
	float       min;             /* widget's display min (or INT32_MIN if unset) */
	float       max;             /* widget's display max (or INT32_MIN if unset) */
	float       high_warn;       /* widget's redline/warn threshold (or INT32_MIN if unset) */
	uint32_t    color_normal;    /* widget's normal-zone color hint (or CHANNEL_USE_DEFAULT_COLOR) */
	uint32_t    color_high_warn; /* widget's warn-zone color (or CHANNEL_USE_DEFAULT_COLOR) */
} legacy_widget_data_t;

/**
 * Record a v13 widget's channel-relevant data so the channel registry
 * auto-populates from existing layouts on first v14 boot. Called by
 * widgets in their from_json when no `channel` field is present but
 * legacy fields are.
 *
 * Behavior:
 *   1. Look up a canonical channel by case-insensitive match of
 *      signal_name against canonical IDs. Returns NULL if no match
 *      (custom signal names need Phase 2 ECU-preset name maps).
 *   2. Activate the canonical channel if not already active.
 *   3. Apply widget's values as channel overrides — but ONLY for fields
 *      that differ from canonical defaults AND only if the channel
 *      doesn't already have a user override (first-write-wins, so
 *      later widgets in the same layout don't clobber earlier ones).
 *   4. Bind the channel's signal subscription to the widget's signal
 *      if not already bound.
 *   5. Mark dirty; debounced save persists.
 *
 * Returns the matched/created channel, or NULL if no canonical match.
 */
channel_t *channel_manager_record_legacy_widget(const legacy_widget_data_t *data);

#ifdef __cplusplus
}
#endif
