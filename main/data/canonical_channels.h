/*
 * canonical_channels.h — firmware-baked canonical channel registry.
 *
 * The canonical channel list is the well-known vocabulary every RDM-7
 * dash understands. Layouts on the marketplace reference channels by
 * their canonical `id`. On any dash that has the channel configured,
 * the layout auto-binds. See:
 *
 *   - docs/adr/0006-channel-architecture-v2.md
 *   - docs/v2_channel_architecture.md
 *   - schema/canonical_channels.md  (data source-of-truth)
 *
 * The registry itself is compile-time constant — it ships with the
 * firmware and cannot be modified at runtime. User-defined custom
 * channels (custom_ prefix) live in /lfs/channels.json instead and
 * are managed by channel_manager.
 *
 * Sentinel values:
 *   CHANNEL_THRESHOLD_UNSET_LOW   — low_warn unset
 *   CHANNEL_THRESHOLD_UNSET_HIGH  — high_warn unset
 *   CHANNEL_USE_DEFAULT_COLOR     — zone color falls back to convention
 *
 * Threshold semantics: a value v fires zones in this order:
 *     v <= low_warn            → LOW_WARN
 *     low_warn < v < high_warn → NORMAL
 *     v >= high_warn           → HIGH_WARN
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Sentinel values ──────────────────────────────────────────────── */
/* Channel range/threshold values are float (so the UI can carry decimals
 * like boost 1.5 bar or lambda 0.85). The "unset" sentinels are float-valued
 * and chosen at ±2^31 — exactly representable, so `x = SENTINEL; x != SENTINEL`
 * is reliable, and far outside any real channel range. */
#define CHANNEL_THRESHOLD_UNSET_LOW   (-2147483648.0f)
#define CHANNEL_THRESHOLD_UNSET_HIGH  ( 2147483648.0f)
#define CHANNEL_SANITY_UNSET_LOW      (-2147483648.0f)
#define CHANNEL_SANITY_UNSET_HIGH     ( 2147483648.0f)
#define CHANNEL_USE_DEFAULT_COLOR     0xFFFFFFFFu

/* ── Channel groups — semantic grouping for UI display ─────────────── */
typedef enum {
	CHGRP_ENGINE_CORE = 0,
	CHGRP_ENGINE_EXHAUST,
	CHGRP_DRIVETRAIN,
	CHGRP_ELECTRICAL,
	CHGRP_CHASSIS_DYNAMICS,
	CHGRP_CHASSIS_PER_CORNER,
	CHGRP_BRAKES_INPUTS,
	CHGRP_FUEL_DISTANCE,
	CHGRP_ENVIRONMENT,
	CHGRP_LAP_TIMING,
	CHGRP_BODY_LIGHTS,
	CHGRP_DIAGNOSTIC,
	CHGRP_DASH_SYSTEM,  /* RDM-7 dash internals: fps, free heap, wifi rssi… */
	/* NOTE: these values are PERSISTED NUMERICALLY in channels.json
	 * (channel_manager.c writes `"group": <int>`). Only ever APPEND here —
	 * inserting a group in the middle silently regroups every channel on
	 * every device already in the field. */
	CHGRP_POSITION,     /* GPS position/motion from an RDM GPS or any CAN GPS */
	CHGRP__COUNT
} channel_group_t;

/* ── Channel cardinality — value type encoding ────────────────────── */
typedef enum {
	CHCARD_SCALAR  = 0,   /* float / int — most channels */
	CHCARD_ENUM    = 1,   /* discrete state set (gear, ignition mode) */
	CHCARD_BOOLEAN = 2,   /* 0/1 (lights, switches, alarms) */
} channel_cardinality_t;

/* ── Canonical channel definition — read-only firmware data ───────── */
typedef struct {
	const char *id;                  /* "rpm", "coolant_temp", ... */
	const char *label;               /* "RPM", "Coolant Temp" */
	channel_group_t group;
	uint8_t tier;                    /* 1, 2, or 3 — UI priority */
	channel_cardinality_t card;
	const char *units_native;        /* "°C", "kPa", "rpm", ... */
	const char *units_display_def;   /* default display unit */
	uint8_t decimals;                /* 0..3 */
	float min_default;               /* display range min */
	float max_default;               /* display range max */
	float low_warn;                  /* CHANNEL_THRESHOLD_UNSET_LOW if none */
	float high_warn;                 /* CHANNEL_THRESHOLD_UNSET_HIGH if none */
	uint32_t color_normal;           /* 0xRRGGBB for the NORMAL zone */
	const char *notes;               /* optional inline notes, may be NULL */
} canonical_channel_def_t;

/* ── Registry ─────────────────────────────────────────────────────── */
extern const canonical_channel_def_t CANONICAL_CHANNELS[];
extern const size_t CANONICAL_CHANNEL_COUNT;

/* ── Group / cardinality metadata for UI ──────────────────────────── */
const char *channel_group_name(channel_group_t g);          /* "Engine — Core" */
const char *channel_group_short_name(channel_group_t g);    /* "engine" */
const char *channel_cardinality_name(channel_cardinality_t c); /* "scalar" */

/* ── Lookup helpers ───────────────────────────────────────────────── */

/**
 * Find a canonical channel definition by id. Returns NULL if not found.
 * O(N) scan — N ≈ 90, called rarely (at channel activation, layout
 * load) so no index needed.
 */
const canonical_channel_def_t *canonical_channel_find(const char *id);

/**
 * Case-insensitive lookup: name → canonical definition. Lets the
 * backwards-compat migrator catch widgets whose signal_name happens
 * to match a canonical id with different capitalization
 * (e.g. "RPM" → rpm, "CoolantTemp" → coolant_temp).
 *
 * Returns NULL if no case-folded canonical id matches.
 */
const canonical_channel_def_t *canonical_channel_find_ci(const char *name);

/**
 * True if `id` matches any canonical channel. Convenience wrapper —
 * equivalent to `canonical_channel_find(id) != NULL`.
 */
bool canonical_channel_exists(const char *id);

/**
 * True when the id starts with "custom_" — the reserved prefix for
 * user-defined channels. Custom channels live in /lfs/channels.json,
 * not in this registry.
 */
bool channel_id_is_custom(const char *id);

/**
 * Count of canonical channels in a given group. Convenience for UI
 * grouping headers.
 */
size_t canonical_channel_count_in_group(channel_group_t g);

/**
 * Color convention by zone. Used when a channel's per-zone color is
 * CHANNEL_USE_DEFAULT_COLOR — caller picks up the convention here.
 */
typedef enum {
	CHZONE_LOW_WARN = 0,
	CHZONE_NORMAL,
	CHZONE_HIGH_WARN,
} channel_zone_t;

uint32_t channel_default_zone_color(channel_zone_t zone);

/** Stable lowercase name for a zone ("low_warn"/"normal"/"high_warn"). */
const char *channel_zone_name(channel_zone_t zone);

/**
 * Compute the zone a given value falls into for this channel definition.
 * Sentinel thresholds short-circuit cleanly — never fires on the unset
 * side.
 */
channel_zone_t canonical_channel_value_zone(
	const canonical_channel_def_t *def, float value);

/* ── Canonical channel → OBD2 PID map ────────────────────────────────
 *
 * Static lookup that says "if this channel had to come from OBD2,
 * which standard PID would carry it." Used by the source picker to
 * surface the OBD2 option for a channel even when the user is on a
 * proprietary CAN preset (Haltech etc.).
 *
 * Only canonical channels that have a clean J1979 mapping are listed.
 * Channels like `gear`, `boost`, `egt_cyl_*` aren't standard OBD2 PIDs
 * — the picker will skip the OBD2 group for those.
 *
 * `obd2_signal_name` matches the corresponding obd2_pid_def_t::signal_name
 * in main/can/obd2_pids.c, so the firmware can resolve the human label
 * and decode params without duplicating that data here.
 */
typedef struct {
	const char *channel_id;        /* canonical channel id (e.g. "rpm") */
	const char *obd2_signal_name;  /* matches obd2_pid_def_t::signal_name */
	uint8_t     service;           /* OBD2 mode (typically 1) */
	uint16_t    pid;               /* OBD2 PID number */
} canonical_obd2_map_t;

extern const canonical_obd2_map_t CANONICAL_OBD2_MAP[];
extern const size_t CANONICAL_OBD2_MAP_COUNT;

/* O(N), N ≈ 30 — fine for the source picker which is called per-click,
 * not per-frame. Returns NULL when the channel has no OBD2 equivalent. */
const canonical_obd2_map_t *canonical_channel_obd2_for(const char *channel_id);

/* The inverse: "this car answered PID 0x0C — what channel is that?"
 * Used by the OBD2 scan flow to turn a list of answering PIDs into a list
 * of channels the vehicle can actually feed. Returns NULL for PIDs with no
 * canonical channel (a car answers plenty of PIDs nobody wants on a dash). */
const canonical_obd2_map_t *canonical_channel_obd2_for_pid(uint8_t service,
                                                           uint16_t pid);

#ifdef __cplusplus
}
#endif
