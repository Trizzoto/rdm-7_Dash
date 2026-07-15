/*
 * channel_manager.c — runtime channel state, listener pattern, persistence.
 *
 * Phase 1 implementation. Focused on the path that unblocks widget binding:
 *   - activate canonical channels
 *   - track current_value via signal subscription
 *   - emit metadata-change events on edits
 *   - load/save /lfs/channels.json (defaults-only emit)
 *
 * Deferred to later phases:
 *   - Custom channel creation (Phase 2)
 *   - Per-zone color overrides (Phase 2)
 *   - Calculated channel hook fires (Phase 3)
 *   - Resolve-on-signal-registry-change reseat (Phase 2)
 *
 * See docs/v2_channel_architecture.md §1.3 for the C API contract.
 */

#include "channel_manager.h"
#include "channel_math.h"
#include "canonical_channels.h"
#include "unit_convert.h"
#include "signal.h"
#include "layout/ecu_presets.h"
#include "ui/lvgl_helpers.h"   /* rdm_lvgl_lock / rdm_lvgl_unlock */
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"    /* esp_register_shutdown_handler */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h" /* vTaskDelay for transient-read retry backoff */
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>        /* fsync, fileno */
#include <sys/stat.h>
#include "esp_heap_caps.h"

static const char *TAG = "channel_mgr";

/* ── Storage ──────────────────────────────────────────────────────── */

#define CHM_INITIAL_CAP 32
#define CHM_GROW_CHUNK  16
#define CHM_MAX         128

/* channels.json on-disk schema.
 *   v2 — one-time shared-binding heal (see channel_manager_load_from_lfs).
 *   v3 — channels OWN their CAN decode (ADR 0005): can_id/bits/scale/offset/
 *        endian/unit emitted under a "decode" object, plus a one-time
 *        v2->v3 migration that seeds channel decode from the active layout's
 *        signals[] (channel_manager_migrate_decode_from_registry).
 * Bumping this re-runs any future one-shot migrations exactly once on an
 * older file. */
#define CHM_SCHEMA_VERSION 3

#define CHM_FILE_PATH    "/lfs/channels.json"
#define CHM_TMP_PATH     CHM_FILE_PATH ".tmp"      /* atomic-write staging file */
#define CHM_BAK_PATH     CHM_FILE_PATH ".bak"      /* previous-good recovery copy */
#define CHM_CORRUPT_PATH CHM_FILE_PATH ".corrupt"  /* preserved unparseable file */
#define CHM_SAVE_DEBOUNCE_US (500 * 1000)   /* 500 ms */

static channel_t **s_channels = NULL;
static size_t s_count = 0;
static size_t s_cap = 0;
static bool s_initialised = false;
static bool s_dirty = false;
static esp_timer_handle_t s_save_timer = NULL;
/* Bulk-edit depth. While >0, every mutation only marks the manager dirty;
 * no channels.json write happens until end_bulk. Lets the ECU wizard apply
 * dozens of channel binds with ONE final flush instead of one full-file
 * write per edit (which stalled the LVGL task + overflowed the CAN queue). */
static uint16_t s_bulk_depth = 0;

/* What schema_version save_to_lfs writes. Tracks the on-disk version so saves
 * that happen BEFORE the deferred v2->v3 decode migration completes keep the
 * file at its pre-migration version — preserving the "migration pending" state
 * across an unexpected reboot in the narrow window between channel_manager_init
 * and the first layout load. The decode migration bumps this to v3 once it has
 * actually copied decode out of the registry. */
static int  s_disk_schema_version = CHM_SCHEMA_VERSION;
/* Set true when a pre-v3 file (or a fresh/reseed device) is loaded; consumed
 * once by channel_manager_migrate_decode_from_registry() after the active
 * layout's signals[] have populated the registry. */
static bool s_decode_migration_pending = false;
/* Guard so esp_register_shutdown_handler(channel_manager_flush) only ever
 * runs once even if init() is entered again after a shutdown/re-init. */
static bool s_shutdown_handler_registered = false;

/* Helper to safely copy strings to fixed-size buffers. */
static void safe_strcpy(char *dst, const char *src, size_t cap) {
	if (!dst || cap == 0) return;
	if (!src) { dst[0] = '\0'; return; }
	strncpy(dst, src, cap - 1);
	dst[cap - 1] = '\0';
}

/* ── Internal: grow channel array if needed ──────────────────────── */
static bool ensure_capacity(size_t needed) {
	if (needed <= s_cap) return true;
	if (needed > CHM_MAX) {
		ESP_LOGE(TAG, "channel cap %d exceeded", CHM_MAX);
		return false;
	}
	size_t new_cap = s_cap ? s_cap : CHM_INITIAL_CAP;
	while (new_cap < needed) new_cap += CHM_GROW_CHUNK;
	channel_t **nb = heap_caps_realloc(s_channels,
		new_cap * sizeof(channel_t *), MALLOC_CAP_SPIRAM);
	if (!nb) nb = realloc(s_channels, new_cap * sizeof(channel_t *));
	if (!nb) return false;
	s_channels = nb;
	s_cap = new_cap;
	return true;
}

/* Forward decls */
static void chm_signal_cb(float value, bool is_stale, void *user_data);
static void chm_save_timer_cb(void *arg);

/* ── Channel allocation ───────────────────────────────────────────── */

static channel_t *channel_alloc(void) {
	channel_t *c = heap_caps_calloc(1, sizeof(channel_t), MALLOC_CAP_SPIRAM);
	if (!c) c = calloc(1, sizeof(channel_t));
	if (!c) return NULL;
	c->signal_index = -1;
	/* CAN decode (ADR 0005) — zeroed by calloc means "no decode" (can_id == 0).
	 * Seed the multiplicative/endian fields to sane neutrals so a partially
	 * filled decode never scales by zero; only meaningful once can_id != 0. */
	c->decode_scale  = 1.0f;
	c->decode_offset = 0.0f;
	c->endian        = 1;            /* Intel (little) — matches layout default */
	c->low_warn      = CHANNEL_THRESHOLD_UNSET_LOW;
	c->high_warn     = CHANNEL_THRESHOLD_UNSET_HIGH;
	c->color_normal        = CHANNEL_USE_DEFAULT_COLOR;
	c->color_low_warn      = CHANNEL_USE_DEFAULT_COLOR;
	c->color_high_warn     = CHANNEL_USE_DEFAULT_COLOR;
	c->is_stale = true;
	c->last_zone = CHZONE_NORMAL;
	return c;
}

static void channel_free(channel_t *c) {
	if (!c) return;
	/* Drop signal subscription if any */
	if (c->signal_index >= 0) {
		signal_unsubscribe(c->signal_index, chm_signal_cb, c);
		c->signal_index = -1;
	}
	/* Drop all listeners */
	channel_listener_t *l = c->listeners;
	while (l) {
		channel_listener_t *next = l->next;
		free(l);
		l = next;
	}
	free(c);
}

/* Populate a channel struct from a canonical definition. */
static void channel_apply_canonical_defaults(channel_t *c,
	const canonical_channel_def_t *def)
{
	safe_strcpy(c->id, def->id, sizeof(c->id));
	safe_strcpy(c->label, def->label, sizeof(c->label));
	c->tier = def->tier;
	c->group = def->group;
	c->card = def->card;
	c->is_canonical = true;

	safe_strcpy(c->units_native, def->units_native, sizeof(c->units_native));
	safe_strcpy(c->units_display, def->units_display_def, sizeof(c->units_display));
	c->decimals = def->decimals;

	c->min = def->min_default;
	c->max = def->max_default;
	c->sanity_min = CHANNEL_SANITY_UNSET_LOW;
	c->sanity_max = CHANNEL_SANITY_UNSET_HIGH;

	c->low_warn = def->low_warn;
	c->high_warn = def->high_warn;

	/* Per-zone colors stay at CHANNEL_USE_DEFAULT_COLOR by default —
	 * normal-zone gets its canonical hint. */
	c->color_normal = def->color_normal;
}

/* ── Fire listeners on metadata change ───────────────────────────── */

static void chm_notify_listeners(channel_t *c) {
	if (!c) return;
	for (channel_listener_t *l = c->listeners; l; l = l->next) {
		if (l->cb) l->cb(c, l->user_data);
	}
}

/* Shared tail for the simple field mutators below: notify listeners, mark
 * the debounced-write dirty flag, and report success. Mutators with a
 * different persistence need (channel_manager_set_signal's synchronous
 * flush, channel_manager_set_decode's persist_now branch) don't use this —
 * they have real reasons to differ, not just copy-paste drift. */
static bool chm_notify_and_dirty(channel_t *c) {
	chm_notify_listeners(c);
	channel_manager_mark_dirty();
	return true;
}

/* ── Listener add / remove ───────────────────────────────────────── */

void channel_manager_subscribe(channel_t *c, channel_changed_cb cb, void *user) {
	if (!c || !cb) return;
	/* Skip duplicate */
	for (channel_listener_t *l = c->listeners; l; l = l->next)
		if (l->cb == cb && l->user_data == user) return;
	channel_listener_t *l = calloc(1, sizeof(channel_listener_t));
	if (!l) return;
	l->cb = cb;
	l->user_data = user;
	l->next = c->listeners;
	c->listeners = l;
	c->listener_count++;
}

void channel_manager_unsubscribe(channel_t *c, channel_changed_cb cb, void *user) {
	if (!c) return;
	channel_listener_t **pp = &c->listeners;
	while (*pp) {
		if ((*pp)->cb == cb && (*pp)->user_data == user) {
			channel_listener_t *gone = *pp;
			*pp = gone->next;
			free(gone);
			c->listener_count--;
			return;
		}
		pp = &(*pp)->next;
	}
}

/* ── Signal callback — channel value tracking ────────────────────── */

static void chm_signal_cb(float value, bool is_stale, void *user_data) {
	channel_t *c = (channel_t *)user_data;
	if (!c) return;

	/* Sanity-bounds gate: drop values outside [sanity_min, sanity_max].
	 * Unset sentinels never trigger. */
	if (!is_stale) {
		if (c->sanity_min != CHANNEL_SANITY_UNSET_LOW && value < c->sanity_min) is_stale = true;
		if (c->sanity_max != CHANNEL_SANITY_UNSET_HIGH && value > c->sanity_max) is_stale = true;
	}

	c->is_stale = is_stale;
	if (!is_stale) {
		c->current_value = value;
		c->last_update_ms = (uint32_t)(esp_timer_get_time() / 1000);
	}

	/* Track zone for the future shared-threshold-eval optimisation, but
	 * DO NOT fire listeners on zone transitions. Widgets already handle
	 * their own per-value rendering via the direct signal callback —
	 * firing channel listeners from this hot path caused panel flicker
	 * (widget re-snapshots + invalidates mid-render every time the value
	 * crossed a threshold). Listeners are reserved for METADATA edits
	 * (range, threshold, signal binding changes from the Channels tab),
	 * which is what chm_notify_listeners is called from in the set_*
	 * mutation functions. */
	channel_zone_t new_zone = CHZONE_NORMAL;
	if (!is_stale) {
		canonical_channel_def_t view = {
			.low_warn = c->low_warn,
			.high_warn = c->high_warn,
		};
		new_zone = canonical_channel_value_zone(&view, value);
	}
	c->last_zone = new_zone;
}

/* ── Lookup ───────────────────────────────────────────────────────── */

channel_t *channel_manager_get(const char *id) {
	if (!id || id[0] == '\0' || !s_initialised) return NULL;
	for (size_t i = 0; i < s_count; ++i)
		if (strcmp(s_channels[i]->id, id) == 0) return s_channels[i];
	return NULL;
}

size_t channel_manager_count(void) {
	return s_count;
}

void channel_format_display_value(const channel_t *c, int16_t signal_index,
                                  float native_value, uint8_t decimals,
                                  char *buf, size_t cap) {
	if (!buf || cap == 0) return;
	/* Convert only when the channel actually carries a different, convertible
	 * display unit. unit_convert() is identity for unknown pairs, so the
	 * strcmp gate is what keeps value→label-mapped channels (gear/mode, which
	 * are unit-less so native==display) on the signal_format_value path. */
	if (c && c->units_display[0] && c->units_native[0] &&
	    strcmp(c->units_display, c->units_native) != 0) {
		float vd = unit_convert(native_value, c->units_native, c->units_display);
		snprintf(buf, cap, "%.*f", decimals, (double)vd);
		return;
	}
	signal_format_value(signal_index, native_value, decimals, buf, cap);
}

channel_t *channel_manager_at(size_t idx) {
	if (idx >= s_count) return NULL;
	return s_channels[idx];
}

/* ── Activate canonical channel ───────────────────────────────────── */

channel_t *channel_manager_activate(const char *canonical_id) {
	if (!s_initialised || !canonical_id) return NULL;

	channel_t *existing = channel_manager_get(canonical_id);
	if (existing) return existing;

	const canonical_channel_def_t *def = canonical_channel_find(canonical_id);
	if (!def) {
		ESP_LOGW(TAG, "activate: unknown canonical id '%s'", canonical_id);
		return NULL;
	}

	if (!ensure_capacity(s_count + 1)) return NULL;

	channel_t *c = channel_alloc();
	if (!c) return NULL;
	channel_apply_canonical_defaults(c, def);

	s_channels[s_count++] = c;
	channel_manager_mark_dirty();
	return c;
}

channel_t *channel_manager_create_custom(
	const char *id, const char *label, channel_group_t group,
	channel_cardinality_t card,
	const char *units_native, const char *units_display, uint8_t decimals,
	float min, float max)
{
	if (!s_initialised || !id) return NULL;

	/* Enforce the custom_ prefix convention. The "custom_" prefix is
	 * how the firmware + UI distinguish user-defined channels from the
	 * canonical catalog (see channel_id_is_custom()). Refuse to coin a
	 * canonical id here. */
	if (!channel_id_is_custom(id)) {
		ESP_LOGW(TAG, "create_custom: id '%s' missing 'custom_' prefix", id);
		return NULL;
	}
	if (channel_manager_get(id)) {
		ESP_LOGW(TAG, "create_custom: id '%s' already exists", id);
		return NULL;
	}
	if (!ensure_capacity(s_count + 1)) return NULL;

	channel_t *c = channel_alloc();
	if (!c) return NULL;

	safe_strcpy(c->id,    id,    sizeof(c->id));
	safe_strcpy(c->label, label ? label : id, sizeof(c->label));
	c->tier  = 2;                /* mid priority — same as most canonicals */
	c->group = group;
	c->card  = card;
	c->is_canonical = false;

	safe_strcpy(c->units_native,
	            units_native  ? units_native  : "", sizeof(c->units_native));
	safe_strcpy(c->units_display,
	            units_display ? units_display : "", sizeof(c->units_display));
	c->decimals = (decimals > 3) ? 3 : decimals;

	c->min = min;
	c->max = max;
	c->sanity_min = CHANNEL_SANITY_UNSET_LOW;
	c->sanity_max = CHANNEL_SANITY_UNSET_HIGH;

	/* Thresholds + colors stay at sentinel-unset; user can dial them
	 * in later via the wizard / web modal. */

	s_channels[s_count++] = c;
	channel_manager_mark_dirty();
	ESP_LOGI(TAG, "create_custom: '%s' (label='%s') in group %d",
	         c->id, c->label, (int)group);
	return c;
}

bool channel_manager_delete(const char *id) {
	for (size_t i = 0; i < s_count; ++i) {
		if (strcmp(s_channels[i]->id, id) == 0) {
			channel_free(s_channels[i]);
			/* swap-remove to keep array tight */
			s_channels[i] = s_channels[--s_count];
			channel_manager_mark_dirty();
			return true;
		}
	}
	return false;
}

/* ── Field mutators ──────────────────────────────────────────────── */

bool channel_manager_set_label(channel_t *c, const char *label) {
	if (!c || !label) return false;
	safe_strcpy(c->label, label, sizeof(c->label));
	return chm_notify_and_dirty(c);
}

bool channel_manager_set_signal(channel_t *c, const char *signal_name) {
	if (!c) return false;
	/* Drop old subscription */
	if (c->signal_index >= 0) {
		signal_unsubscribe(c->signal_index, chm_signal_cb, c);
		c->signal_index = -1;
	}
	if (signal_name && signal_name[0] != '\0') {
		safe_strcpy(c->signal_name, signal_name, sizeof(c->signal_name));
		int16_t idx = signal_find_by_name(signal_name);
		if (idx >= 0) {
			c->signal_index = idx;
			signal_subscribe(idx, chm_signal_cb, c);
		}
	} else {
		c->signal_name[0] = '\0';
	}

	/* ADR 0005: a channel's CAN decode is meaningful ONLY while it is bound to
	 * a CAN signal. When rebinding to a non-CAN source (OBD2-polled / internal)
	 * or unbinding, drop any stale decode the channel carried from a previous
	 * CAN binding. Otherwise channel_to_full_json keeps emitting a `decode`
	 * block (so the web shows the CAN bit-decode editor on an OBD2/unbound
	 * channel) and, worse, register_decoded_signals() would UPSERT the new
	 * OBD2 signal back to source=CAN with a bogus decode on the next layout
	 * load — corrupting provenance. The CAN bind paths (channel_apply_preconfig
	 * / bind-source custom / wizard) call channel_manager_set_decode() right
	 * after this to (re)install the correct decode, so clearing here is safe:
	 * a freshly-registered CAN signal reports source CAN and is preserved. */
	bool new_is_can = false;
	if (c->signal_index >= 0) {
		signal_t *ns = signal_get_by_index((uint16_t)c->signal_index);
		if (ns && (signal_source_t)ns->source == SIGNAL_SOURCE_CAN) new_is_can = true;
	}
	if (!new_is_can && c->can_id != 0) {
		c->can_id = 0;
		c->bit_start = 0;
		c->bit_length = 0;
		c->decode_scale = 1.0f;
		c->decode_offset = 0.0f;
		c->is_signed = false;
		c->endian = 1;
		c->decode_unit[0] = '\0';
	}

	c->is_stale = true;
	chm_notify_listeners(c);
	/* Signal bindings are rare, high-value edits — and an automotive dash
	 * can lose 12V abruptly at key-off with no graceful shutdown, so a
	 * 500 ms debounce window can drop a just-made binding. Persist the
	 * binding synchronously NOW (flash-endurance cost is negligible for
	 * such an infrequent edit) instead of only marking dirty.
	 *
	 * Exception: during a bulk edit (ECU wizard apply) the synchronous
	 * flush is deferred to end_bulk so dozens of binds coalesce into one
	 * write — see s_bulk_depth. */
	s_dirty = true;
	if (s_bulk_depth == 0) channel_manager_flush();
	return true;
}

bool channel_manager_set_units_display(channel_t *c, const char *units) {
	if (!c || !units) return false;
	safe_strcpy(c->units_display, units, sizeof(c->units_display));
	return chm_notify_and_dirty(c);
}

bool channel_manager_set_units_native(channel_t *c, const char *units) {
	if (!c || !units || !units[0]) return false;
	if (strcmp(c->units_native, units) == 0) return true;   /* no-op */
	char old[8];
	safe_strcpy(old, c->units_native, sizeof(old));
	/* Re-express value-space fields (range, thresholds, sanity) from the old
	 * native unit into the new one so they keep their physical meaning — the
	 * stored numbers always live in units_native. Unset sentinels stay put. */
	if (unit_convert_supported(old, units)) {
		c->min = unit_convert(c->min, old, units);
		c->max = unit_convert(c->max, old, units);
		if (c->low_warn  != CHANNEL_THRESHOLD_UNSET_LOW)
			c->low_warn  = unit_convert(c->low_warn,  old, units);
		if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH)
			c->high_warn = unit_convert(c->high_warn, old, units);
		if (c->sanity_min != CHANNEL_SANITY_UNSET_LOW)
			c->sanity_min = unit_convert(c->sanity_min, old, units);
		if (c->sanity_max != CHANNEL_SANITY_UNSET_HIGH)
			c->sanity_max = unit_convert(c->sanity_max, old, units);
	}
	safe_strcpy(c->units_native, units, sizeof(c->units_native));
	return chm_notify_and_dirty(c);
}

bool channel_manager_set_decimals(channel_t *c, uint8_t decimals) {
	if (!c) return false;
	c->decimals = decimals;
	return chm_notify_and_dirty(c);
}

bool channel_manager_set_range(channel_t *c, float min, float max) {
	if (!c || max <= min) return false;
	c->min = min;
	c->max = max;
	return chm_notify_and_dirty(c);
}

bool channel_manager_set_sanity(channel_t *c, float smin, float smax) {
	if (!c) return false;
	c->sanity_min = smin;
	c->sanity_max = smax;
	return chm_notify_and_dirty(c);
}

bool channel_manager_set_threshold(channel_t *c, channel_zone_t zone, float value) {
	if (!c) return false;
	switch (zone) {
	case CHZONE_LOW_WARN:      c->low_warn = value;      break;
	case CHZONE_HIGH_WARN:     c->high_warn = value;     break;
	default: return false;
	}
	return chm_notify_and_dirty(c);
}

bool channel_manager_clear_threshold(channel_t *c, channel_zone_t zone) {
	if (!c) return false;
	switch (zone) {
	case CHZONE_LOW_WARN:      c->low_warn      = CHANNEL_THRESHOLD_UNSET_LOW;  break;
	case CHZONE_HIGH_WARN:     c->high_warn     = CHANNEL_THRESHOLD_UNSET_HIGH; break;
	default: return false;
	}
	return chm_notify_and_dirty(c);
}

bool channel_manager_set_zone_color(channel_t *c, channel_zone_t zone, uint32_t color) {
	if (!c) return false;
	switch (zone) {
	case CHZONE_LOW_WARN:      c->color_low_warn = color;      break;
	case CHZONE_NORMAL:        c->color_normal = color;        break;
	case CHZONE_HIGH_WARN:     c->color_high_warn = color;     break;
	default: return false;
	}
	return chm_notify_and_dirty(c);
}

bool channel_manager_reset_to_defaults(channel_t *c) {
	if (!c || !c->is_canonical) return false;
	const canonical_channel_def_t *def = canonical_channel_find(c->id);
	if (!def) return false;
	/* Preserve signal_name + signal subscription across reset — they're
	 * user-specific even on a canonical channel. */
	char saved_signal[32];
	int16_t saved_idx = c->signal_index;
	safe_strcpy(saved_signal, c->signal_name, sizeof(saved_signal));

	channel_apply_canonical_defaults(c, def);

	safe_strcpy(c->signal_name, saved_signal, sizeof(c->signal_name));
	c->signal_index = saved_idx;
	return chm_notify_and_dirty(c);
}

/* ── Backwards-compat migration: legacy widget → channel auto-populate ── */

channel_t *channel_manager_record_legacy_widget(const legacy_widget_data_t *data) {
	if (!s_initialised || !data || !data->signal_name || data->signal_name[0] == '\0')
		return NULL;

	/* Two-step canonical lookup:
	 *   1. Case-insensitive direct match against canonical IDs. Catches
	 *      users whose signal names happen to match (rpm/RPM,
	 *      coolant_temp/COOLANT_TEMP, etc.).
	 *   2. ECU normalized-name map fallback. Maps the dash's own
	 *      standard ECU vocabulary (RPM, MAP, THROTTLE, IGNITION, …)
	 *      to canonical channels (rpm, manifold_pressure,
	 *      throttle_position, ignition_timing, …). Catches the
	 *      majority of widgets in real layouts because the dash
	 *      already normalizes signal names via the ECU preset
	 *      apply path.
	 * If both miss, no auto-bind happens — widget keeps using its
	 * legacy fields. Users can manually bind via the Channels tab. */
	const canonical_channel_def_t *def = canonical_channel_find_ci(data->signal_name);
	if (!def) {
		const char *canonical_id = ecu_signal_name_to_canonical(data->signal_name);
		if (canonical_id) def = canonical_channel_find(canonical_id);
	}
	if (!def) {
		/* No mapping for this signal — widget falls back to its own
		 * legacy values (still renders correctly via the backwards-compat
		 * path). Users with unusual signal names can manually bind via
		 * the Channels tab once that UI lands. */
		return NULL;
	}

	/* Activate the canonical channel if not yet active. */
	channel_t *c = channel_manager_get(def->id);
	bool newly_activated = false;
	if (!c) {
		c = channel_manager_activate(def->id);
		newly_activated = true;
	}
	if (!c) return NULL;

	/* Populate when the channel is newly activated OR when it exists
	 * but has no signal binding yet. The "no signal yet" case covers
	 * the first-v14-boot sequence:
	 *   1. channel_manager_init seeds OBD2 defaults with empty
	 *      signal_name → saves channels.json
	 *   2. layout loads → widgets call record_legacy_widget → the
	 *      OBD2 defaults exist but have no signal binding, so we
	 *      populate them with the widget's signal + values
	 *   3. next save persists the user's bindings
	 * Subsequent boots: channels have signal bindings → first-write-
	 * wins skips so user edits aren't clobbered. */
	bool can_populate = newly_activated || (c->signal_name[0] == '\0');
	if (can_populate) {
		/* Signal binding */
		safe_strcpy(c->signal_name, data->signal_name, sizeof(c->signal_name));
		int16_t idx = signal_find_by_name(data->signal_name);
		if (idx >= 0) {
			c->signal_index = idx;
			signal_subscribe(idx, chm_signal_cb, c);
		}
		/* Range — apply only if non-unset and differs from canonical default */
		if (data->min != INT32_MIN && data->min != def->min_default)
			c->min = data->min;
		if (data->max != INT32_MIN && data->max != def->max_default)
			c->max = data->max;
		/* Thresholds — apply only if non-unset */
		if (data->high_warn != INT32_MIN)
			c->high_warn = data->high_warn;
		/* Color hints — apply only if non-default */
		if (data->color_normal != CHANNEL_USE_DEFAULT_COLOR &&
		    data->color_normal != def->color_normal)
			c->color_normal = data->color_normal;
		if (data->color_high_warn != CHANNEL_USE_DEFAULT_COLOR)
			c->color_high_warn = data->color_high_warn;

		channel_manager_mark_dirty();
		ESP_LOGI(TAG, "migrated legacy widget signal='%s' → channel '%s'",
		         data->signal_name, def->id);
	}

	return c;
}

/* ── Signal pipeline (placeholder — actual dispatch happens via the ──
 * signal_subscribe callback we attach in set_signal). The exposed
 * dispatch helper is for explicit poking from tests / replay. */
size_t channel_manager_dispatch_signal(uint16_t signal_index, float value, bool stale) {
	size_t n = 0;
	for (size_t i = 0; i < s_count; ++i) {
		channel_t *c = s_channels[i];
		if (c->signal_index == (int16_t)signal_index) {
			chm_signal_cb(value, stale, c);
			n++;
		}
	}
	return n;
}

/* The firmware-side strict matcher used to live here. It was used by
 * channel_manager_resolve_signals as a fallback when a channel's stored
 * signal_name didn't resolve in the freshly-loaded layout's signal
 * registry. That auto-match locked in whatever signal the matcher picked
 * first, which broke reverse-compat when the user loaded a layout
 * authored against a different ECU vocabulary — the channel would stay
 * pointing at a strict-matched signal that didn't necessarily reflect
 * the new layout's actual widget bindings.
 *
 * Now `resolve_signals` clears the binding on miss instead, and the
 * widget-driven `record_legacy_widget` path rebuilds the channel from
 * the actual widget data as widgets load. The web UI still does
 * strict-matching client-side (`_strictSignalMatch` in index.html) for
 * its activate-and-suggest flow.
 *
 * Helpers kept removed to satisfy -Werror=unused-function. Reach for
 * git history if a firmware-side strict matcher is needed again. */
#if 0  /* retired — see comment above */
static int16_t _chm_strict_match(const char *channel_id) {
	if (!channel_id || !channel_id[0]) return -1;

	char chan_norm[32];
	_chm_normalize_name(channel_id, chan_norm, sizeof(chan_norm));
	if (chan_norm[0] == '\0') return -1;

	uint16_t n = signal_get_count();

	/* Pass 1: exact normalized match. */
	for (uint16_t i = 0; i < n; i++) {
		signal_t *s = signal_get_by_index(i);
		if (!s) continue;
		char sig_norm[32];
		_chm_normalize_name(s->name, sig_norm, sizeof(sig_norm));
		if (strcmp(sig_norm, chan_norm) == 0) return (int16_t)i;
	}

	/* Pass 2: channel id is a component of exactly one signal name. */
	int comp_hits = 0;
	int16_t comp_hit_idx = -1;
	for (uint16_t i = 0; i < n; i++) {
		signal_t *s = signal_get_by_index(i);
		if (!s) continue;
		char sig_norm[32];
		_chm_normalize_name(s->name, sig_norm, sizeof(sig_norm));
		if (_chm_has_component(sig_norm, chan_norm)) {
			comp_hits++;
			comp_hit_idx = (int16_t)i;
			if (comp_hits > 1) break;
		}
	}
	if (comp_hits == 1) return comp_hit_idx;

	/* Pass 3: a channel-id component matches exactly one signal name. */
	const char *p = chan_norm;
	while (*p) {
		const char *next = strchr(p, '_');
		size_t len = next ? (size_t)(next - p) : strlen(p);
		if (len > 2) {
			char part[32];
			if (len >= sizeof(part)) len = sizeof(part) - 1;
			memcpy(part, p, len);
			part[len] = '\0';

			int hits = 0;
			int16_t hit_idx = -1;
			for (uint16_t i = 0; i < n; i++) {
				signal_t *s = signal_get_by_index(i);
				if (!s) continue;
				char sig_norm[32];
				_chm_normalize_name(s->name, sig_norm, sizeof(sig_norm));
				if (strcmp(sig_norm, part) == 0) {
					hits++;
					hit_idx = (int16_t)i;
					if (hits > 1) break;
				}
			}
			if (hits == 1) return hit_idx;
		}
		if (!next) break;
		p = next + 1;
	}

	return -1;
}
#endif  /* retired _chm_strict_match block */

void channel_manager_resolve_signals(void) {
	bool dirty = false;
	for (size_t i = 0; i < s_count; ++i) {
		channel_t *c = s_channels[i];

		/* Drop any existing subscription before re-resolving. */
		if (c->signal_index >= 0) {
			signal_unsubscribe(c->signal_index, chm_signal_cb, c);
			c->signal_index = -1;
		}

		/* Empty signal_name = user explicitly unbound. Don't auto-rebind
		 * here — let the explicit picker action set the next binding. */
		if (c->signal_name[0] == '\0') {
			c->is_stale = true;
			continue;
		}

		/* Try the stored signal name first (case-sensitive exact). If it
		 * resolves, the user's previous binding is preserved across the
		 * reload — that handles "user manually picked a source" and
		 * "same layout reloaded" both. */
		int16_t idx = signal_find_by_name(c->signal_name);

		/* Canonical-vocabulary fallback. On an exact-name miss for a
		 * canonical channel, map each registered signal through the curated
		 * ECU dictionary (ecu_signal_name_to_canonical) and bind the one
		 * whose canonical id equals this channel's id. This is the same
		 * mapping the first-run wizard's force-rebind uses — doing it here
		 * lets a channel light up on a plain reboot (or when a layout from a
		 * different ECU vocabulary is loaded) WITHOUT re-running the wizard.
		 * Miss-only (never overrides an exact match) and dictionary-based
		 * (not fuzzy string matching), so it can't silently mis-bind. The
		 * stored signal_name is left unchanged so the channel keeps
		 * re-mapping per layout rather than pinning to one ECU's name. */
		if (idx < 0 && c->is_canonical && c->id[0] != '\0') {
			uint16_t scount = signal_get_count();
			for (uint16_t s = 0; s < scount; ++s) {
				signal_t *sig = signal_get_by_index(s);
				if (!sig) continue;
				const char *canon = ecu_signal_name_to_canonical(sig->name);
				if (canon && strcmp(canon, c->id) == 0) {
					idx = (int16_t)s;
					ESP_LOGI(TAG, "channel '%s': '%s' not in registry — "
					         "bound via canonical map to layout signal '%s'",
					         c->id, c->signal_name, sig->name);
					break;
				}
			}
		}

		if (idx >= 0) {
			c->signal_index = idx;
			signal_subscribe(idx, chm_signal_cb, c);
		} else {
			/* Stored signal isn't in the current registry — usually
			 * because the user just loaded a layout that uses a
			 * different ECU vocabulary (MaxxECU "RPM" vs Ford FG
			 * "ENGINE_RPM"). PRESERVE signal_name so the user's
			 * manually-configured channel survives the round trip and
			 * lights up again when they return to a compatible layout.
			 * The widget can still display live data on the current
			 * layout because each widget falls back to its own
			 * `signal:` field when the bound channel isn't resolvable
			 * (see widget from_json — channel branch is gated on
			 * signal_index >= 0). */
			ESP_LOGI(TAG,
			         "channel '%s': stored signal '%s' not in current "
			         "registry — keeping binding stale until a compatible "
			         "layout is loaded",
			         c->id, c->signal_name);
		}
		c->is_stale = true;
	}
	if (dirty) channel_manager_mark_dirty();
}

/* ── Channel-owned CAN decode (ADR 0005) ─────────────────────────────
 *
 * The channel OWNS its decode (device-local), so a shared layout renders
 * against the TARGET dash's channels rather than importing the source dash's
 * signals[]. These helpers (a) push channel decode into the runtime signal
 * registry so signal_dispatch_frame() decodes the bus from channels.json, and
 * (b) one-time migrate existing devices' decode out of the active layout's
 * signals[] into their channels. */

/* (re)register one channel's decode into the signal registry + (re)bind its
 * subscription. No-op for channels without a decode or signal name. UPSERTs,
 * so the channel's decode wins over any same-name layout signal already in the
 * registry (latest-registration-wins — see signal_register_with_source). */
static void _register_channel_decode(channel_t *c) {
	if (!c || c->can_id == 0 || c->signal_name[0] == '\0') return;
	int16_t idx = signal_register_with_source(
		c->signal_name, c->can_id, c->bit_start, c->bit_length,
		c->decode_scale, c->decode_offset, c->is_signed, c->endian,
		c->decode_unit[0] ? c->decode_unit : "", SIGNAL_SOURCE_CAN);
	if (idx < 0) return;
	/* Bind the channel's value callback to the slot. signal_subscribe does NOT
	 * de-dupe, so only (re)subscribe when the index actually changed; on a
	 * change drop the stale subscription first. */
	if (c->signal_index != idx) {
		if (c->signal_index >= 0)
			signal_unsubscribe(c->signal_index, chm_signal_cb, c);
		c->signal_index = idx;
		signal_subscribe(idx, chm_signal_cb, c);
		c->is_stale = true;
	}
}

void channel_manager_register_decoded_signals(void) {
	if (!s_initialised) return;
	int n = 0;
	for (size_t i = 0; i < s_count; ++i) {
		channel_t *c = s_channels[i];
		if (c->can_id == 0 || c->signal_name[0] == '\0') continue;
		_register_channel_decode(c);
		n++;
	}
	if (n) ESP_LOGI(TAG, "register_decoded_signals: %d channel decode(s) -> registry", n);

	/* Math channels own a synthetic output signal (MATH_<ID>) — restore those
	 * on the same paths that restore CAN decodes so a reload never strands a
	 * calculated channel without its registry entry. */
	channel_math_register_signals();
}

bool channel_manager_set_decode(channel_t *c, uint32_t can_id,
                                uint8_t bit_start, uint8_t bit_length,
                                float scale, float offset, bool is_signed,
                                uint8_t endian, const char *unit,
                                bool persist_now) {
	if (!c) return false;
	/* Reject out-of-range decode geometry at the one authoritative writer so no
	 * caller (web channels editor, ECU wizard, legacy layout import) can store a
	 * binding that silently decodes to 0. A CAN frame is 8 bytes / 64 bits;
	 * can_id is an 11-bit standard or 29-bit extended id (0 = unbound, in which
	 * case the geometry is irrelevant and not checked). can_extract_bits also
	 * guards OOB at decode time, but rejecting here keeps channels.json clean. */
	if (can_id > 0x1FFFFFFF ||
	    (can_id != 0 && (bit_length < 1 || bit_length > 64 ||
	                     bit_start > 63 || bit_start + bit_length > 64))) {
		ESP_LOGW(TAG, "set_decode rejected: can_id=0x%lX bit_start=%u bit_length=%u",
		         (unsigned long)can_id, bit_start, bit_length);
		return false;
	}
	c->can_id       = can_id;
	c->bit_start    = bit_start;
	c->bit_length   = bit_length;
	c->decode_scale = scale;
	c->decode_offset= offset;
	c->is_signed    = is_signed;
	c->endian       = endian;
	safe_strcpy(c->decode_unit, unit ? unit : "", sizeof(c->decode_unit));

	/* Make it decode live immediately (UPSERT + bind). */
	_register_channel_decode(c);

	chm_notify_listeners(c);
	if (persist_now) {
		/* Single high-value edit (web decode editor): persist NOW like signal
		 * bindings — abrupt 12V loss at key-off gives no graceful shutdown. */
		s_dirty = true;
		channel_manager_flush();
	} else {
		/* Bulk/loop caller (ECU wizard) or a path that already flushed via
		 * set_signal: coalesce into one debounced write instead of a
		 * full-file rewrite per channel. */
		channel_manager_mark_dirty();
	}
	return true;
}

/* Ensure a channel owns the given CAN signal's decode, mirroring the first-run
 * wizard's resolution (_wiz_ensure_channel_for_signal): canonical via the ECU
 * vocabulary / case-insensitive id, else a custom_<lowercased-name> channel.
 * Collision guard: if the canonical channel is already bound to a DIFFERENT
 * signal, fall through to a custom channel so two distinct signals never share
 * one channel. Returns the channel (newly created or matched) or NULL. */
static channel_t *_channelize_signal(const signal_t *sig) {
	if (!sig || !sig->name[0]) return NULL;

	const canonical_channel_def_t *def = NULL;
	const char *canon = ecu_signal_name_to_canonical(sig->name);
	if (canon) def = canonical_channel_find(canon);
	if (!def)  def = canonical_channel_find_ci(sig->name);

	channel_t *ch = NULL;
	if (def) {
		ch = channel_manager_get(def->id);
		if (ch && ch->signal_name[0] && strcmp(ch->signal_name, sig->name) != 0) {
			/* The canonical channel is bound under a DIFFERENT name. If that
			 * name maps to the SAME canonical quantity (a different ECU
			 * vocabulary — channel 'rpm' bound to "RPM" while this layout's
			 * signal is "ENGINE_RPM"), it is NOT a real collision: reuse the
			 * canonical channel and copy decode onto it rather than minting a
			 * spurious custom_engine_rpm duplicate. signal_name is left intact
			 * so cross-vocabulary re-mapping in resolve_signals still works.
			 * Only a genuinely different quantity forks to a custom channel. */
			const char *other = ecu_signal_name_to_canonical(ch->signal_name);
			const canonical_channel_def_t *other_def = other
				? canonical_channel_find(other)
				: canonical_channel_find_ci(ch->signal_name);
			if (!other_def || strcmp(other_def->id, def->id) != 0)
				ch = NULL;             /* different quantity — go custom */
		} else if (!ch) {
			ch = channel_manager_activate(def->id);
		}
	}

	if (!ch) {
		char cid[32];
		size_t j = 0;
		for (const char *p = "custom_"; *p && j < sizeof(cid) - 1; p++) cid[j++] = *p;
		for (size_t k = 0; sig->name[k] && j < sizeof(cid) - 1; k++) {
			char ch_c = sig->name[k];
			cid[j++] = (ch_c >= 'A' && ch_c <= 'Z') ? (char)(ch_c - 'A' + 'a') : ch_c;
		}
		cid[j] = '\0';
		ch = channel_manager_get(cid);
		if (!ch)
			ch = channel_manager_create_custom(
				cid, sig->name, CHGRP_DIAGNOSTIC, CHCARD_SCALAR,
				sig->unit, sig->unit, 0, 0.0f, 100.0f);
	}

	if (ch && ch->signal_name[0] == '\0')
		safe_strcpy(ch->signal_name, sig->name, sizeof(ch->signal_name));
	return ch;
}

void channel_manager_migrate_decode_from_registry(void) {
	if (!s_initialised || !s_decode_migration_pending) return;

	int copied = 0, channelized = 0, skipped = 0;
	uint16_t scount = signal_get_count();
	for (uint16_t s = 0; s < scount; ++s) {
		signal_t *sig = signal_get_by_index(s);
		if (!sig) continue;
		/* Only CAN bit-field signals carry portable decode. OBD2 (polled) and
		 * INTERNAL (synthesized) signals re-register themselves at boot and
		 * have no can_id/bits to copy. */
		if (sig->source != SIGNAL_SOURCE_CAN || sig->can_id == 0) continue;

		/* Find an existing channel bound to this signal name. */
		channel_t *owner = NULL;
		for (size_t i = 0; i < s_count; ++i) {
			if (s_channels[i]->signal_name[0] &&
			    strcmp(s_channels[i]->signal_name, sig->name) == 0) {
				owner = s_channels[i];
				break;
			}
		}
		if (!owner) {
			owner = _channelize_signal(sig);
			if (owner) channelized++;
			else       skipped++;     /* channel cap hit / alloc failed */
		}
		if (!owner) continue;

		/* Copy decode only when the channel doesn't already own one — never
		 * clobber decode a user already set (e.g. a re-run or partial state). */
		if (owner->can_id == 0) {
			owner->can_id       = sig->can_id;
			owner->bit_start    = sig->bit_start;
			owner->bit_length   = sig->bit_length;
			owner->decode_scale = sig->scale;
			owner->decode_offset= sig->offset;
			owner->is_signed    = sig->is_signed;
			owner->endian       = sig->endian;
			safe_strcpy(owner->decode_unit, sig->unit, sizeof(owner->decode_unit));
			copied++;
		}
	}

	/* One-shot completion — but ONLY stamp v3 if every CAN signal got a channel.
	 * If the channel cap (CHM_MAX) was hit, leave migration pending and the file
	 * at its pre-v3 version: the on-disk layout signals[] decode is still intact
	 * (migration never rewrites the layout), so nothing is lost and the
	 * migration retries on a later boot (e.g. after the user prunes channels). */
	if (skipped == 0) {
		s_decode_migration_pending = false;
		s_disk_schema_version = CHM_SCHEMA_VERSION;
	} else {
		ESP_LOGW(TAG, "decode migration: %d signal(s) could not be channelized "
		         "(channel cap %d) — leaving migration PENDING", skipped, CHM_MAX);
	}
	s_dirty = true;
	channel_manager_save_to_lfs();
	ESP_LOGI(TAG, "decode migration -> v%d: %d copied, %d channelized, %d skipped",
	         s_disk_schema_version, copied, channelized, skipped);
}

/* ── JSON I/O ─────────────────────────────────────────────────────── */

/* Helper: only emit if differs from canonical default. Exact == is fine —
 * both sides originate from the same float literal / round-tripped JSON. */
static bool field_eq_default_float(const channel_t *c, float v, float def_v) {
	(void)c; return v == def_v;
}

static cJSON *channel_to_json(const channel_t *c) {
	if (!c) return NULL;
	cJSON *j = cJSON_CreateObject();
	if (!j) return NULL;

	cJSON_AddStringToObject(j, "id", c->id);

	/* For canonical channels, only emit fields that DIFFER from the
	 * canonical default. For custom channels, emit everything. */
	const canonical_channel_def_t *def = c->is_canonical ?
		canonical_channel_find(c->id) : NULL;

	if (!def || strcmp(c->label, def->label) != 0)
		cJSON_AddStringToObject(j, "label", c->label);

	/* group/card come from the canonical def on reload for a canonical
	 * channel, but a custom channel has no def to restore them from. Without
	 * persisting them the load path's placeholder (CHGRP_DIAGNOSTIC /
	 * CHCARD_SCALAR) stuck permanently, so a custom "Turbo Speed" filed under
	 * Engine silently migrated to Diagnostic on the next boot. Additive and
	 * optional — an older channels.json without these keys still loads and
	 * just keeps the placeholder, so no schema bump. */
	if (!def) {
		cJSON_AddNumberToObject(j, "group", (int)c->group);
		cJSON_AddNumberToObject(j, "card", (int)c->card);
	}

	if (c->signal_name[0] != '\0')
		cJSON_AddStringToObject(j, "signal", c->signal_name);

	if (!def || strcmp(c->units_native, def->units_native) != 0)
		cJSON_AddStringToObject(j, "units_native", c->units_native);

	if (!def || strcmp(c->units_display, def->units_display_def) != 0)
		cJSON_AddStringToObject(j, "units_display", c->units_display);

	if (!def || c->decimals != def->decimals)
		cJSON_AddNumberToObject(j, "decimals", c->decimals);

	if (!def || !field_eq_default_float(c, c->min, def->min_default))
		cJSON_AddNumberToObject(j, "min", c->min);
	if (!def || !field_eq_default_float(c, c->max, def->max_default))
		cJSON_AddNumberToObject(j, "max", c->max);

	if (c->sanity_min != CHANNEL_SANITY_UNSET_LOW)
		cJSON_AddNumberToObject(j, "sanity_min", c->sanity_min);
	if (c->sanity_max != CHANNEL_SANITY_UNSET_HIGH)
		cJSON_AddNumberToObject(j, "sanity_max", c->sanity_max);

	if (!def || c->low_warn != def->low_warn) {
		if (c->low_warn != CHANNEL_THRESHOLD_UNSET_LOW)
			cJSON_AddNumberToObject(j, "low_warn", c->low_warn);
	}
	if (!def || c->high_warn != def->high_warn) {
		if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH)
			cJSON_AddNumberToObject(j, "high_warn", c->high_warn);
	}

	/* Zone color overrides — emit only when explicitly customized */
	if (c->color_normal != CHANNEL_USE_DEFAULT_COLOR &&
	    (!def || c->color_normal != def->color_normal))
		cJSON_AddNumberToObject(j, "color", (int)c->color_normal);
	if (c->color_low_warn != CHANNEL_USE_DEFAULT_COLOR)
		cJSON_AddNumberToObject(j, "color_low_warn", (int)c->color_low_warn);
	if (c->color_high_warn != CHANNEL_USE_DEFAULT_COLOR)
		cJSON_AddNumberToObject(j, "color_high_warn", (int)c->color_high_warn);

	/* Math/derived source — whole block or nothing. Additive key: older
	 * firmware ignores it (channel falls back to its persisted signal
	 * binding, which simply never updates). Operands are typed: a string
	 * is a channel id, a number is a constant. */
	if (c->math_enabled) {
		cJSON *m = cJSON_AddObjectToObject(j, "math");
		if (m) {
			if (c->math_a_is_const) cJSON_AddNumberToObject(m, "a", c->math_a_const);
			else                    cJSON_AddStringToObject(m, "a", c->math_a);
			if (c->math_b_is_const) cJSON_AddNumberToObject(m, "b", c->math_b_const);
			else                    cJSON_AddStringToObject(m, "b", c->math_b);
			cJSON_AddNumberToObject(m, "op", c->math_op);
		}
	}

	/* CAN decode (ADR 0005) — defaults-only: omitted entirely when the channel
	 * has no decode (can_id == 0, i.e. internal/OBD2/unset). When present, the
	 * whole block is emitted so a re-load reconstructs the exact bit-field. */
	if (c->can_id != 0) {
		cJSON *d = cJSON_AddObjectToObject(j, "decode");
		if (d) {
			cJSON_AddNumberToObject(d, "can_id", (double)c->can_id);
			cJSON_AddNumberToObject(d, "bit_start", c->bit_start);
			cJSON_AddNumberToObject(d, "bit_length", c->bit_length);
			cJSON_AddNumberToObject(d, "scale", c->decode_scale);
			cJSON_AddNumberToObject(d, "offset", c->decode_offset);
			cJSON_AddBoolToObject(d, "is_signed", c->is_signed);
			cJSON_AddNumberToObject(d, "endian", c->endian);
			if (c->decode_unit[0] != '\0')
				cJSON_AddStringToObject(d, "unit", c->decode_unit);
		}
	}

	return j;
}

static bool channel_from_json(channel_t *c, cJSON *j) {
	if (!c || !j) return false;

	cJSON *it;
	it = cJSON_GetObjectItemCaseSensitive(j, "label");
	if (cJSON_IsString(it) && it->valuestring) safe_strcpy(c->label, it->valuestring, sizeof(c->label));

	/* Custom channels only — canonical ones take group/card from their def,
	 * so these keys are absent for them (channel_to_json). Range-checked:
	 * a hand-edited or corrupt file must not index past the enum. */
	it = cJSON_GetObjectItemCaseSensitive(j, "group");
	if (cJSON_IsNumber(it) && it->valueint >= 0 && it->valueint < CHGRP__COUNT)
		c->group = (channel_group_t)it->valueint;

	it = cJSON_GetObjectItemCaseSensitive(j, "card");
	if (cJSON_IsNumber(it) && it->valueint >= CHCARD_SCALAR &&
	    it->valueint <= CHCARD_BOOLEAN)
		c->card = (channel_cardinality_t)it->valueint;

	it = cJSON_GetObjectItemCaseSensitive(j, "signal");
	if (cJSON_IsString(it) && it->valuestring) safe_strcpy(c->signal_name, it->valuestring, sizeof(c->signal_name));

	/* units_native loads as-is — stored range/thresholds are already in this
	 * unit (set_units_native converted them at edit time, not load time). */
	it = cJSON_GetObjectItemCaseSensitive(j, "units_native");
	if (cJSON_IsString(it) && it->valuestring) safe_strcpy(c->units_native, it->valuestring, sizeof(c->units_native));

	it = cJSON_GetObjectItemCaseSensitive(j, "units_display");
	if (cJSON_IsString(it) && it->valuestring) safe_strcpy(c->units_display, it->valuestring, sizeof(c->units_display));

	it = cJSON_GetObjectItemCaseSensitive(j, "decimals");
	if (cJSON_IsNumber(it)) c->decimals = (uint8_t)it->valueint;

	it = cJSON_GetObjectItemCaseSensitive(j, "min");
	if (cJSON_IsNumber(it)) c->min = (float)it->valuedouble;
	it = cJSON_GetObjectItemCaseSensitive(j, "max");
	if (cJSON_IsNumber(it)) c->max = (float)it->valuedouble;

	it = cJSON_GetObjectItemCaseSensitive(j, "sanity_min");
	if (cJSON_IsNumber(it)) c->sanity_min = (float)it->valuedouble;
	it = cJSON_GetObjectItemCaseSensitive(j, "sanity_max");
	if (cJSON_IsNumber(it)) c->sanity_max = (float)it->valuedouble;

	it = cJSON_GetObjectItemCaseSensitive(j, "low_warn");
	if (cJSON_IsNumber(it)) c->low_warn = (float)it->valuedouble;
	it = cJSON_GetObjectItemCaseSensitive(j, "high_warn");
	if (cJSON_IsNumber(it)) c->high_warn = (float)it->valuedouble;

	it = cJSON_GetObjectItemCaseSensitive(j, "color");
	if (cJSON_IsNumber(it)) c->color_normal = (uint32_t)it->valueint;
	it = cJSON_GetObjectItemCaseSensitive(j, "color_low_warn");
	if (cJSON_IsNumber(it)) c->color_low_warn = (uint32_t)it->valueint;
	it = cJSON_GetObjectItemCaseSensitive(j, "color_high_warn");
	if (cJSON_IsNumber(it)) c->color_high_warn = (uint32_t)it->valueint;

	/* Math/derived source. Absent block leaves math disabled. Operands are
	 * typed (string = channel id, number = constant); at least one must be
	 * a channel or the entry is ignored. */
	cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "math");
	if (cJSON_IsObject(m)) {
		cJSON *a  = cJSON_GetObjectItemCaseSensitive(m, "a");
		cJSON *b  = cJSON_GetObjectItemCaseSensitive(m, "b");
		cJSON *op = cJSON_GetObjectItemCaseSensitive(m, "op");
		bool a_ch    = cJSON_IsString(a) && a->valuestring && a->valuestring[0];
		bool a_const = cJSON_IsNumber(a);
		bool b_ch    = cJSON_IsString(b) && b->valuestring && b->valuestring[0];
		bool b_const = cJSON_IsNumber(b);
		if ((a_ch || a_const) && (b_ch || b_const) && (a_ch || b_ch)) {
			if (a_ch) safe_strcpy(c->math_a, a->valuestring, sizeof(c->math_a));
			else      { c->math_a[0] = '\0'; c->math_a_is_const = true;
			            c->math_a_const = (float)a->valuedouble; }
			if (b_ch) safe_strcpy(c->math_b, b->valuestring, sizeof(c->math_b));
			else      { c->math_b[0] = '\0'; c->math_b_is_const = true;
			            c->math_b_const = (float)b->valuedouble; }
			c->math_op = cJSON_IsNumber(op) ? (uint8_t)op->valueint : 0;
			c->math_enabled = true;
		}
	}

	/* CAN decode (ADR 0005). Absent block leaves the calloc/alloc defaults
	 * (can_id == 0 == "no decode"). */
	cJSON *dec = cJSON_GetObjectItemCaseSensitive(j, "decode");
	if (cJSON_IsObject(dec)) {
		it = cJSON_GetObjectItemCaseSensitive(dec, "can_id");
		if (cJSON_IsNumber(it)) c->can_id = (uint32_t)it->valuedouble;
		it = cJSON_GetObjectItemCaseSensitive(dec, "bit_start");
		if (cJSON_IsNumber(it)) c->bit_start = (uint8_t)it->valueint;
		it = cJSON_GetObjectItemCaseSensitive(dec, "bit_length");
		if (cJSON_IsNumber(it)) c->bit_length = (uint8_t)it->valueint;
		it = cJSON_GetObjectItemCaseSensitive(dec, "scale");
		if (cJSON_IsNumber(it)) c->decode_scale = (float)it->valuedouble;
		it = cJSON_GetObjectItemCaseSensitive(dec, "offset");
		if (cJSON_IsNumber(it)) c->decode_offset = (float)it->valuedouble;
		it = cJSON_GetObjectItemCaseSensitive(dec, "is_signed");
		if (cJSON_IsBool(it)) c->is_signed = cJSON_IsTrue(it);
		it = cJSON_GetObjectItemCaseSensitive(dec, "endian");
		if (cJSON_IsNumber(it)) c->endian = (uint8_t)it->valueint;
		it = cJSON_GetObjectItemCaseSensitive(dec, "unit");
		if (cJSON_IsString(it) && it->valuestring)
			safe_strcpy(c->decode_unit, it->valuestring, sizeof(c->decode_unit));

		/* Self-heal invalid geometry: a corrupt / hand-edited / imported
		 * channels.json bypasses channel_manager_set_decode's validation. An
		 * out-of-range field can't decode a real 8-byte frame, so drop the
		 * decode (can_id = 0) rather than run garbage. */
		if (c->can_id && (c->bit_length == 0 || c->bit_length > 64 ||
		                  (uint16_t)c->bit_start + (uint16_t)c->bit_length > 64 ||
		                  c->endian > 1)) {
			ESP_LOGW(TAG, "channel '%s': invalid decode (bit %u+%u endian %u) — dropping",
			         c->id, c->bit_start, c->bit_length, c->endian);
			c->can_id = 0;
		}
	}

	/* Sanity / self-heal: a high_warn at or below the channel min (or a
	 * low_warn at or above max) can never be a meaningful alert — it would
	 * fire on every in-range value ("always red"). Older data / a bad
	 * migration could leave a stray 0; treat it as unset so it heals on load. */
	if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH && c->high_warn <= c->min)
		c->high_warn = CHANNEL_THRESHOLD_UNSET_HIGH;
	if (c->low_warn != CHANNEL_THRESHOLD_UNSET_LOW && c->low_warn >= c->max)
		c->low_warn = CHANNEL_THRESHOLD_UNSET_LOW;

	return true;
}

/* Read + parse one channels-file path. Returns the cJSON root (caller
 * deletes) or NULL if the file is missing / unreadable / unparseable.
 *   out_existed  — false only when the file is not present (stat fails).
 *   out_io_error — true when the bytes could NOT be read (fopen/malloc/short
 *                  read) even after retries, i.e. a TRANSIENT failure rather
 *                  than corruption. NULL-with-io_error=false means the file was
 *                  read fine but does not parse (genuine corruption).
 * Callers use out_io_error to avoid demoting a good file to .corrupt/.bak just
 * because of momentary heap pressure or an FS glitch at boot. */
static cJSON *chm_read_and_parse(const char *path, bool *out_existed,
                                 bool *out_io_error) {
	if (out_existed) *out_existed = false;
	if (out_io_error) *out_io_error = false;
	struct stat st;
	if (stat(path, &st) != 0) return NULL;
	if (out_existed) *out_existed = true;

	/* Retry transient I/O failures (fopen glitch, momentary OOM, short read)
	 * with a real yield between attempts so a good file isn't mistaken for
	 * corrupt. A failure that persists across all attempts sets io_error. */
	for (int attempt = 0; attempt < 3; ++attempt) {
		if (attempt) vTaskDelay(pdMS_TO_TICKS(20));
		FILE *f = fopen(path, "r");
		if (!f) { if (out_io_error) *out_io_error = true; continue; }
		char *buf = malloc((size_t)st.st_size + 1);
		if (!buf) { fclose(f); if (out_io_error) *out_io_error = true; continue; }
		size_t n = fread(buf, 1, (size_t)st.st_size, f);
		fclose(f);
		if (n < (size_t)st.st_size) {
			free(buf);
			if (out_io_error) *out_io_error = true;
			continue;
		}
		buf[n] = '\0';
		if (out_io_error) *out_io_error = false;   /* read succeeded */
		cJSON *root = cJSON_Parse(buf);
		free(buf);
		return root;   /* NULL here = parsed-but-bad = genuine corruption */
	}
	return NULL;   /* transient failure persisted; io_error is set */
}

/* ── Heal: break spurious shared signal bindings ──────────────────────
 *
 * Before commit d9c8130 the web editor's _strictSignalMatch had a loose
 * fallback that matched a single PART of a channel id to a whole signal, so a
 * family of distinct canonical channels (egt_cyl_1..8) could all auto-bind to
 * one shared signal (EGT). That binding got persisted into channels.json, and
 * the web fix only prevents NEW mis-binds — it does NOT heal a file that
 * already has them. Symptom: every widget bound to those channels resolves to
 * the same signal_index, so injecting a test value into one moves them all,
 * and warnings/alerts compare against the wrong source.
 *
 * Heal pass: find groups of >= 2 CANONICAL channels sharing the same non-empty
 * signal_name. Within a group keep the single channel whose normalized id
 * equals the normalized signal name (the genuine match, if exactly one
 * exists) and clear the binding on the rest; if no unambiguous keeper exists,
 * clear them all. Cleared channels keep their identity + metadata — only the
 * signal source is dropped, so they light up again the moment the user
 * re-binds a real source. Custom channels are never touched (the user bound
 * those explicitly). Idempotent: a re-run on a healed file finds no groups. */
static void chm_normalize_name(const char *in, char *out, size_t cap) {
	size_t j = 0;
	for (size_t i = 0; in && in[i] && j + 1 < cap; ++i) {
		char ch = in[i];
		if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
		if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'))
			out[j++] = ch;
		/* Separators (_ / space / -) are dropped so "egt_cyl_1" -> "egtcyl1",
		 * "EGT" -> "egt", "COOLANT_TEMP" -> "coolanttemp". */
	}
	out[j] = '\0';
}

static int chm_heal_shared_bindings(void) {
	int cleared = 0;
	for (size_t i = 0; i < s_count; ++i) {
		channel_t *ci = s_channels[i];
		if (!ci->is_canonical || ci->signal_name[0] == '\0') continue;

		/* Collect canonical channels sharing this exact signal_name. Cleared
		 * channels (below) get an empty signal_name, so later iterations skip
		 * them and groups are never double-processed. */
		size_t group[CHM_MAX];
		size_t gn = 0;
		for (size_t k = 0; k < s_count && gn < CHM_MAX; ++k) {
			channel_t *ck = s_channels[k];
			if (ck->is_canonical && ck->signal_name[0] != '\0' &&
			    strcmp(ck->signal_name, ci->signal_name) == 0)
				group[gn++] = k;
		}
		if (gn < 2) continue;   /* not shared */

		/* Unambiguous keeper: exactly one channel whose normalized id matches
		 * the normalized signal name. */
		char sig_norm[40];
		chm_normalize_name(ci->signal_name, sig_norm, sizeof(sig_norm));
		int keep = -1, keep_hits = 0;
		for (size_t g = 0; g < gn; ++g) {
			char id_norm[40];
			chm_normalize_name(s_channels[group[g]]->id, id_norm, sizeof(id_norm));
			if (strcmp(id_norm, sig_norm) == 0) { keep = (int)group[g]; keep_hits++; }
		}
		if (keep_hits != 1) keep = -1;   /* ambiguous -> clear all */

		for (size_t g = 0; g < gn; ++g) {
			size_t idx = group[g];
			if ((int)idx == keep) continue;
			channel_t *c = s_channels[idx];
			ESP_LOGW(TAG, "heal: channel '%s' shared signal '%s' with %u other "
			         "channel(s) — unbinding (re-assign a source in the editor)",
			         c->id, c->signal_name, (unsigned)(gn - 1));
			if (c->signal_index >= 0) {
				signal_unsubscribe(c->signal_index, chm_signal_cb, c);
				c->signal_index = -1;
			}
			c->signal_name[0] = '\0';
			c->is_stale = true;
			cleared++;
		}
	}
	if (cleared > 0) {
		ESP_LOGW(TAG, "heal: cleared %d spurious shared signal binding(s); "
		         "those channels now need a source re-assigned", cleared);
		s_dirty = true;
	}
	return cleared;
}

/* Try the staged .tmp then the previous-good .bak. On success, republish the
 * recovered copy as the live file (so the next boot is clean) and return its
 * root. Returns NULL when nothing usable exists. A recovery copy that is
 * present but only failed transiently (io_error) is left in place, not removed,
 * so a later boot can still use it. */
static cJSON *chm_recover_from_copies(void) {
	const char *recover_from[] = { CHM_TMP_PATH, CHM_BAK_PATH };
	for (size_t i = 0; i < sizeof(recover_from) / sizeof(recover_from[0]); ++i) {
		bool rexist = false, rio = false;
		cJSON *rroot = chm_read_and_parse(recover_from[i], &rexist, &rio);
		if (rroot) {
			ESP_LOGW(TAG, "recovered channels from %s", recover_from[i]);
			rename(recover_from[i], CHM_FILE_PATH);
			return rroot;
		}
		if (rexist && !rio) remove(recover_from[i]);  /* present but corrupt */
	}
	return NULL;
}

/* Preserve the bad live file as .corrupt, then recover from .tmp / .bak. */
static cJSON *chm_preserve_and_recover(void) {
	remove(CHM_CORRUPT_PATH);                 /* drop any stale copy */
	rename(CHM_FILE_PATH, CHM_CORRUPT_PATH);  /* preserve the bad file */
	return chm_recover_from_copies();
}

esp_err_t channel_manager_load_from_lfs(void) {
	bool existed = false, io_error = false;
	cJSON *root = chm_read_and_parse(CHM_FILE_PATH, &existed, &io_error);

	if (!root && !existed) {
		/* No live file. This is the normal fresh-device case, but it is ALSO
		 * what a power loss between save_to_lfs's two renames leaves behind
		 * (live->.bak done, .tmp->live not yet), with a fully-fsync'd .tmp and
		 * the previous-good .bak both sitting on disk. Try to recover those
		 * before declaring "no file" — otherwise init() reseeds defaults and
		 * the reseed save truncates the good .tmp, losing the user's data. */
		root = chm_recover_from_copies();
		if (!root) {
			return ESP_ERR_NOT_FOUND;   /* truly nothing — caller seeds defaults */
		}
		ESP_LOGW(TAG, "%s absent — recovered from a staged copy", CHM_FILE_PATH);
	}

	if (!root && existed) {
		/* Live file is present but unusable. Either it failed to PARSE (power
		 * loss mid-write / flash corruption) or — after chm_read_and_parse's
		 * retries — could not be READ at all (io_error, a persistent FS/heap
		 * problem). Either way, do NOT let init() reseed-and-overwrite it:
		 * preserve the bad/unreadable copy as .corrupt for diagnostics, then
		 * try to recover from the staged .tmp or previous-good .bak so the
		 * user's bindings survive. The common momentary-glitch case was already
		 * absorbed by the read retries, so reaching here means a real problem. */
		ESP_LOGW(TAG, "%s %s — preserving as %s and attempting recovery",
		         CHM_FILE_PATH, io_error ? "unreadable" : "failed to parse",
		         CHM_CORRUPT_PATH);
		root = chm_preserve_and_recover();
		if (!root) {
			/* No recoverable copy. Return ESP_FAIL; init() may reseed now
			 * that the corrupt file has been moved out of the way (so it
			 * can't be silently re-saved over with defaults). */
			ESP_LOGE(TAG, "no recoverable channels file — defaults will be seeded");
			return ESP_FAIL;
		}
	}

	cJSON *chans = cJSON_GetObjectItemCaseSensitive(root, "channels");
	if (!cJSON_IsArray(chans)) {
		/* Parses but has no channels array — structurally corrupt (e.g. a
		 * torn write that still happens to be valid JSON). This used to
		 * return ESP_OK, silently dropping every binding AND skipping .bak
		 * recovery. Treat it exactly like an unparseable file. */
		ESP_LOGW(TAG, "%s parses but has no channels array — attempting recovery",
		         CHM_FILE_PATH);
		cJSON_Delete(root);
		root = chm_preserve_and_recover();
		if (!root) {
			ESP_LOGE(TAG, "no recoverable channels file — defaults will be seeded");
			return ESP_FAIL;
		}
		chans = cJSON_GetObjectItemCaseSensitive(root, "channels");
		if (!cJSON_IsArray(chans)) {
			/* Recovered copy is structurally bad too — give up cleanly. */
			cJSON_Delete(root);
			return ESP_FAIL;
		}
	}

	cJSON *jver = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
	int file_ver = cJSON_IsNumber(jver) ? jver->valueint : 1;
	/* Mirror the on-disk version so saves before the deferred decode migration
	 * don't prematurely stamp v3. */
	s_disk_schema_version = (file_ver < 1) ? 1 : file_ver;

	int sz = cJSON_GetArraySize(chans);
	for (int i = 0; i < sz; ++i) {
		cJSON *jc = cJSON_GetArrayItem(chans, i);
		cJSON *jid = cJSON_GetObjectItemCaseSensitive(jc, "id");
		if (!cJSON_IsString(jid) || !jid->valuestring) continue;

		channel_t *c = NULL;
		if (canonical_channel_exists(jid->valuestring)) {
			c = channel_manager_activate(jid->valuestring);
		} else if (channel_id_is_custom(jid->valuestring)) {
			/* Re-create the custom channel so its decode, thresholds, colors and
			 * signal binding survive a reboot. The first-run wizard, studio
			 * import, and the v2->v3 decode migration all mint custom_* channels
			 * and persist them here; the old "Phase 2 skip" silently dropped them
			 * on the next boot, and the following save rewrote channels.json
			 * without them (permanent loss). group/card aren't persisted
			 * (channel_to_json omits them) so default to the wizard's choice;
			 * channel_from_json below restores everything else. A duplicate
			 * custom id returns NULL here (already exists) and is skipped. */
			c = channel_manager_create_custom(jid->valuestring, jid->valuestring,
			                                  CHGRP_DIAGNOSTIC, CHCARD_SCALAR,
			                                  "", "", 0, 0.0f, 100.0f);
		}
		if (!c) continue;
		channel_from_json(c, jc);
		/* Re-subscribe to signal if name was set in JSON. Guard against a
		 * duplicate id in the file subscribing the same channel twice:
		 * signal_subscribe doesn't de-dupe, so a second slot would dangle after
		 * the channel is freed (channel_free unsubscribes only once), corrupting
		 * memory on every later CAN frame. signal_index starts at -1, so this
		 * only skips a genuine repeat. */
		if (c->signal_name[0] != '\0') {
			int16_t idx = signal_find_by_name(c->signal_name);
			if (idx >= 0 && c->signal_index != idx) {
				c->signal_index = idx;
				signal_subscribe(idx, chm_signal_cb, c);
			}
		}
	}

	cJSON_Delete(root);

	/* One-time migration (channels.json schema v1 -> v2): heal any pre-d9c8130
	 * shared signal bindings left by the old fuzzy auto-matcher, which bound
	 * DISTINCT canonical channels (egt_cyl_1..8) to one signal. This runs
	 * exactly once — on the first boot of a pre-v2 file — so it cleans up the
	 * legacy artifact WITHOUT ever touching shares the user sets up on purpose
	 * later (deliberate signal sharing is how layouts stay portable). After the
	 * version bump below, the pass never re-runs. */
	if (file_ver < 2) {
		int healed = chm_heal_shared_bindings();
		ESP_LOGI(TAG, "channels.json migrate v%d->2 (%d shared binding(s) healed)",
		         file_ver, healed);
		/* Persist the version bump even when nothing was healed, so the
		 * one-time pass doesn't re-scan — and can't clear an intentional
		 * share — on subsequent boots. */
		if (s_disk_schema_version < 2) s_disk_schema_version = 2;
		s_dirty = true;
		channel_manager_save_to_lfs();
	}

	/* v2 -> v3 (ADR 0005): the decode migration needs the active layout's
	 * signals[] in the registry, which isn't loaded yet at init time. Flag it
	 * here; channel_manager_migrate_decode_from_registry() consumes the flag
	 * from the layout-load path (after _load_signals) and bumps to v3 then. */
	s_decode_migration_pending = (file_ver < 3);

	s_dirty = false;
	ESP_LOGI(TAG, "loaded %zu channels from %s", s_count, CHM_FILE_PATH);
	return ESP_OK;
}

esp_err_t channel_manager_save_to_lfs(void) {
	/* Serialize the live channel array under the LVGL mutex. Every mutator
	 * (web handlers, the on-device wizard, signal dispatch) runs on the LVGL
	 * task or holds this lock, and channel_manager_delete() frees + swap-
	 * removes an entry — so iterating s_channels from the esp_timer debounce-
	 * save task (chm_save_timer_cb) without the lock could read freed memory or
	 * a torn signal_name/label string and persist garbage. Build the JSON
	 * STRING under the lock, then do the slow flash I/O unlocked so rendering
	 * isn't stalled. The mutex is recursive, so callers already holding it
	 * (end_bulk on the LVGL task) are fine. On a lock timeout, DEFER rather than
	 * serialize unguarded: a 500 ms timeout does NOT prove LVGL is wedged — a
	 * long layout reload / wizard apply legitimately holds the recursive mutex
	 * for seconds while actively mutating s_channels (ensure_capacity's realloc
	 * can move/free the pointer array; channel_free frees entries), so an
	 * unguarded iterate could read freed memory or persist torn strings. s_dirty
	 * stays set and the debounce timer re-arms, so the save just happens a bit
	 * later once the lock frees. */
	if (!rdm_lvgl_lock(500)) {
		ESP_LOGW(TAG, "save: LVGL lock busy — deferring save");
		return ESP_ERR_TIMEOUT;
	}

	cJSON *root = cJSON_CreateObject();
	if (!root) {
		rdm_lvgl_unlock();
		return ESP_ERR_NO_MEM;
	}
	/* Emit the tracked on-disk version (not the compile-time constant): stays
	 * at the pre-migration version until the v2->v3 decode migration completes,
	 * so a reboot in that window still re-runs the migration. */
	cJSON_AddNumberToObject(root, "schema_version", s_disk_schema_version);

	cJSON *arr = cJSON_AddArrayToObject(root, "channels");
	for (size_t i = 0; i < s_count; ++i) {
		cJSON *jc = channel_to_json(s_channels[i]);
		if (jc) cJSON_AddItemToArray(arr, jc);
	}
	size_t saved_count = s_count;

	char *json = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	rdm_lvgl_unlock();
	if (!json) return ESP_ERR_NO_MEM;

	size_t len = strlen(json);

	/* Atomic write: stage into CHM_TMP_PATH, fsync it to flash, then
	 * rename() over the live file. rename() is atomic on LittleFS, so a
	 * power loss mid-write leaves either the old file or the new file
	 * intact — never a half-written channels.json. On any failure we
	 * remove the temp file and return WITHOUT having touched the live
	 * file. (Layouts already have this guarantee; channels.json did not,
	 * so a crash mid-fwrite could corrupt it and wipe all user bindings
	 * on the next boot reseed.) */
	FILE *f = fopen(CHM_TMP_PATH, "w");
	if (!f) { cJSON_free(json); return ESP_FAIL; }
	size_t w = fwrite(json, 1, len, f);
	cJSON_free(json);
	if (w != len) {
		fclose(f);
		remove(CHM_TMP_PATH);
		ESP_LOGE(TAG, "save: short write (%zu/%zu) to %s", w, len, CHM_TMP_PATH);
		return ESP_FAIL;
	}
	/* fflush drains stdio's userspace buffer, fsync commits to flash —
	 * both before the close/rename so the renamed file is durable. */
	if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
		fclose(f);
		remove(CHM_TMP_PATH);
		ESP_LOGE(TAG, "save: flush/fsync failed for %s", CHM_TMP_PATH);
		return ESP_FAIL;
	}
	if (fclose(f) != 0) {
		remove(CHM_TMP_PATH);
		ESP_LOGE(TAG, "save: close failed for %s", CHM_TMP_PATH);
		return ESP_FAIL;
	}

	/* Keep a previous-good copy (best-effort) so load_from_lfs can recover
	 * if the live file later fails to parse. Ignore the rename result —
	 * a missing live file (first save) simply means no .bak yet. */
	rename(CHM_FILE_PATH, CHM_BAK_PATH);

	/* Atomic publish. If this fails the live file may be gone but the
	 * .bak still holds the previous good copy and the .tmp holds the new
	 * one, so the recovery path on next load can still find valid data. */
	if (rename(CHM_TMP_PATH, CHM_FILE_PATH) != 0) {
		ESP_LOGE(TAG, "save: rename %s -> %s failed", CHM_TMP_PATH, CHM_FILE_PATH);
		remove(CHM_TMP_PATH);
		return ESP_FAIL;
	}

	s_dirty = false;
	ESP_LOGI(TAG, "saved %zu channels to %s", saved_count, CHM_FILE_PATH);
	return ESP_OK;
}

/* ── Backup / restore (channels.json is device-local per ADR-0005, otherwise
 * unrecoverable on a hardware swap or FS corruption) ─────────────────────── */

esp_err_t channel_manager_export_raw(char **out_buf, size_t *out_len) {
	if (!out_buf) return ESP_ERR_INVALID_ARG;
	*out_buf = NULL;
	if (out_len) *out_len = 0;
	/* Commit any debounced edits so the export reflects the latest state. */
	channel_manager_flush();
	FILE *f = fopen(CHM_FILE_PATH, "rb");
	if (!f) return ESP_ERR_NOT_FOUND;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0) { fclose(f); return ESP_FAIL; }
	char *buf = malloc((size_t)sz + 1);
	if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
	size_t rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[rd] = '\0';
	*out_buf = buf;
	if (out_len) *out_len = rd;
	return ESP_OK;
}

esp_err_t channel_manager_import_raw(const char *json, size_t len) {
	if (!json || len == 0) return ESP_ERR_INVALID_ARG;
	/* Validate before touching the live file: must parse, carry a "channels"
	 * array within the channel cap, and a schema_version this firmware can load
	 * (>=1 and not from the future). Rejecting here means a bad upload can never
	 * replace a working config. */
	cJSON *root = cJSON_Parse(json);
	if (!root) return ESP_ERR_INVALID_ARG;
	cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "channels");
	cJSON *ver = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
	int n = cJSON_IsArray(arr) ? cJSON_GetArraySize(arr) : -1;
	bool ok = n >= 0 && n <= CHM_MAX && cJSON_IsNumber(ver) &&
	          ver->valuedouble >= 1 && ver->valuedouble <= CHM_SCHEMA_VERSION;
	cJSON_Delete(root);
	if (!ok) return ESP_ERR_INVALID_ARG;

	/* Atomic write, mirroring save_to_lfs (stage -> fsync -> rename, keep .bak).
	 * Takes effect on the next boot via channel_manager_load_from_lfs(); the
	 * caller reboots rather than hot-swapping the live registry (which would
	 * dangle widgets' channel subscriptions). */
	FILE *f = fopen(CHM_TMP_PATH, "w");
	if (!f) return ESP_FAIL;
	size_t w = fwrite(json, 1, len, f);
	if (w != len) { fclose(f); remove(CHM_TMP_PATH); return ESP_FAIL; }
	if (fflush(f) != 0 || fsync(fileno(f)) != 0) { fclose(f); remove(CHM_TMP_PATH); return ESP_FAIL; }
	if (fclose(f) != 0) { remove(CHM_TMP_PATH); return ESP_FAIL; }
	rename(CHM_FILE_PATH, CHM_BAK_PATH);
	if (rename(CHM_TMP_PATH, CHM_FILE_PATH) != 0) { remove(CHM_TMP_PATH); return ESP_FAIL; }
	ESP_LOGI(TAG, "imported channels.json (%zu bytes, %d channels) — reboot to apply",
	         len, n);
	return ESP_OK;
}

static void chm_save_timer_cb(void *arg) {
	(void)arg;
	if (!s_dirty) return;
	/* If the save deferred because the LVGL lock was busy (a long layout/wizard
	 * apply legitimately holds it for seconds), it's still dirty — re-arm to
	 * retry rather than drop the pending save until the next edit. */
	if (channel_manager_save_to_lfs() == ESP_ERR_TIMEOUT &&
	    s_dirty && s_save_timer) {
		esp_timer_start_once(s_save_timer, CHM_SAVE_DEBOUNCE_US);
	}
}

void channel_manager_mark_dirty(void) {
	s_dirty = true;
	/* During a bulk edit, don't even arm the debounce timer — end_bulk
	 * does a single flush once all edits are in. */
	if (s_bulk_depth > 0) return;
	if (s_save_timer) {
		esp_timer_stop(s_save_timer);
		esp_timer_start_once(s_save_timer, CHM_SAVE_DEBOUNCE_US);
	}
}

void channel_manager_begin_bulk(void) {
	s_bulk_depth++;
}

void channel_manager_end_bulk(void) {
	if (s_bulk_depth > 0) s_bulk_depth--;
	/* On the outermost end, commit everything accumulated during the bulk
	 * edit in ONE synchronous write. */
	if (s_bulk_depth == 0 && s_dirty) channel_manager_flush();
}

void channel_manager_flush(void) {
	/* Cancel any pending debounced save so we don't double-write, then
	 * commit synchronously if there's anything outstanding. Registered as
	 * an esp_register_shutdown_handler so clean reboots persist; also
	 * called directly from set_signal for immediate binding durability
	 * (abrupt 12V loss at key-off gives no graceful shutdown window). */
	if (s_save_timer) esp_timer_stop(s_save_timer);
	if (s_dirty) channel_manager_save_to_lfs();
}

/* ── Default OBD2 channel set for a brand-new dash ────────────────── */

static const char *DEFAULT_OBD2_CHANNELS[] = {
	"rpm",
	"coolant_temp",
	"vehicle_speed",
	"throttle_position",
	"engine_load",
	"intake_air_temp",
	"battery_voltage",
	"fuel_level",
	"gear",
	"ambient_temp",
};

static void seed_default_obd2_set(void) {
	size_t n = sizeof(DEFAULT_OBD2_CHANNELS) / sizeof(DEFAULT_OBD2_CHANNELS[0]);
	for (size_t i = 0; i < n; ++i) {
		channel_manager_activate(DEFAULT_OBD2_CHANNELS[i]);
	}
	ESP_LOGI(TAG, "seeded %zu default OBD2 channels", n);
}

/* ── Dash-system channels — fed by signal_internal.c ────────────────
 *
 * These are RDM-7's own runtime stats (FPS, free heap, wifi rssi…).
 * The signals are always running on every dash regardless of vehicle
 * or ECU, so the matching canonical channels should be active +
 * bound out of the box — no separate activation step in the UI.
 *
 * ensure_dash_system_channels() runs on every boot AFTER the
 * channels.json load, so it idempotently adds any missing dash
 * channels on existing devices (e.g. after a firmware update
 * that introduces a new dash channel). */
static const struct {
	const char *channel_id;
	const char *signal_name;
} DEFAULT_DASH_SYSTEM_CHANNELS[] = {
	{"dash_fps",        "FPS"},
	{"dash_cpu",        "CPU_PERCENT"},
	{"dash_free_heap",  "FREE_HEAP_KB"},
	{"dash_free_psram", "FREE_PSRAM_KB"},
	{"dash_uptime",     "UPTIME_S"},
	{"dash_chip_temp",  "CHIP_TEMP"},
	{"dash_wifi_rssi",  "WIFI_RSSI"},
};

static void ensure_dash_system_channels(void) {
	size_t n = sizeof(DEFAULT_DASH_SYSTEM_CHANNELS) /
	           sizeof(DEFAULT_DASH_SYSTEM_CHANNELS[0]);
	int added = 0;
	for (size_t i = 0; i < n; ++i) {
		const char *cid  = DEFAULT_DASH_SYSTEM_CHANNELS[i].channel_id;
		const char *sname = DEFAULT_DASH_SYSTEM_CHANNELS[i].signal_name;
		if (channel_manager_get(cid)) continue;   /* already active */

		channel_t *c = channel_manager_activate(cid);
		if (!c) {
			ESP_LOGW(TAG, "failed to activate dash channel '%s'", cid);
			continue;
		}
		channel_manager_set_signal(c, sname);
		added++;
	}
	if (added > 0) {
		ESP_LOGI(TAG, "auto-activated %d dash-system channel(s)", added);
		channel_manager_save_to_lfs();
	}
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

esp_err_t channel_manager_init(void) {
	if (s_initialised) return ESP_OK;

	const esp_timer_create_args_t timer_args = {
		.callback = chm_save_timer_cb,
		.arg = NULL,
		.name = "chm_save",
	};
	esp_err_t terr = esp_timer_create(&timer_args, &s_save_timer);
	if (terr != ESP_OK) {
		ESP_LOGW(TAG, "save timer create failed: %d (saves will be synchronous)", terr);
		s_save_timer = NULL;
	}

	s_initialised = true;

	/* Persist pending edits on every clean reboot/restart path. Guarded so
	 * a shutdown()+init() cycle doesn't double-register the handler. */
	if (!s_shutdown_handler_registered) {
		if (esp_register_shutdown_handler(channel_manager_flush) == ESP_OK) {
			s_shutdown_handler_registered = true;
		} else {
			ESP_LOGW(TAG, "could not register shutdown flush handler");
		}
	}

	esp_err_t err = channel_manager_load_from_lfs();
	if (err == ESP_ERR_NOT_FOUND) {
		ESP_LOGI(TAG, "no /lfs/channels.json — seeding default OBD2 set");
		/* Fresh device: born pre-v3 so the first layout load channelizes any
		 * CAN signals the default/active layout carries (ADR 0005 migration). */
		s_disk_schema_version = 2;
		s_decode_migration_pending = true;
		seed_default_obd2_set();
		channel_manager_save_to_lfs();
	} else if (err != ESP_OK) {
		/* Reseed only reaches here AFTER load_from_lfs has already moved any
		 * unparseable live file aside to *.corrupt and exhausted .tmp/.bak
		 * recovery — so seeding defaults (and the save below) can no longer
		 * silently overwrite a present-but-corrupt user file before a
		 * recovery attempt. */
		ESP_LOGW(TAG, "load failed (%d) — seeding default OBD2 set", err);
		/* Reseeded defaults have no decode; let the next layout load re-derive
		 * channel decode from its signals[] (ADR 0005 migration). */
		s_disk_schema_version = 2;
		s_decode_migration_pending = true;
		seed_default_obd2_set();
	}

	/* Run on every boot, not just fresh installs — keeps dash-system
	 * channels up to date when a firmware update adds new ones. */
	ensure_dash_system_channels();

	return ESP_OK;
}

void channel_manager_shutdown(void) {
	if (!s_initialised) return;
	if (s_dirty) channel_manager_save_to_lfs();
	if (s_save_timer) {
		esp_timer_stop(s_save_timer);
		esp_timer_delete(s_save_timer);
		s_save_timer = NULL;
	}
	for (size_t i = 0; i < s_count; ++i) channel_free(s_channels[i]);
	free(s_channels);
	s_channels = NULL;
	s_count = 0;
	s_cap = 0;
	s_initialised = false;
}
