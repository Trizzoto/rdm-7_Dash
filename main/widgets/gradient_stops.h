/**
 * gradient_stops.h — shared multi-stop gradient data + helpers.
 *
 * Used by widget_bar, widget_rpm_bar, and widget_arc. Each widget owns
 * its own gradient_stops_t and feeds it through the JSON round-trip; the
 * rendering side either bakes an RGB565 image (bars) or lerps the
 * indicator colour with the current value (arc).
 *
 * Schema in layout JSON:
 *   "grad_stops": [ {"pos": 0, "color": 0x07E0},
 *                   {"pos": 50, "color": 0xFFE0},
 *                   {"pos": 100, "color": 0xF800} ]
 *
 * pos is 0..100 (display %), color is the RGB565 value the firmware
 * uses everywhere else for widget colours. Stops MUST be kept sorted
 * by pos at edit time; the loader sorts defensively too.
 *
 * Backward compatibility: the prior 2-stop schema (grad_enabled +
 * grad_end_color / bar_grad_end_color) is upgraded at load time into
 * a [{0, base}, {100, end}] array. Saving writes the new array form
 * only — old fields are dropped by defaults-only emission in to_json.
 */
#pragma once

#include "cJSON.h"
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 8 is generous — Photoshop ships with 4 default stops on most presets
 * and our automotive use case (gear-style indicators, RPM zones) tops
 * out around 5. Higher caps just bloat layout JSON; we can always raise
 * later by bumping this. */
#define GRADIENT_MAX_STOPS 8

typedef struct {
    uint8_t  pos;     /* 0..100 % along the gradient axis */
    uint16_t color;   /* RGB565 — matches every other widget colour field */
} gradient_stop_t;

typedef struct {
    gradient_stop_t stops[GRADIENT_MAX_STOPS];
    uint8_t         count;     /* 0 = no gradient; valid range 2..GRADIENT_MAX_STOPS */
} gradient_stops_t;

/**
 * Parse a `grad_stops` JSON array into @p out. Sorts the result by pos
 * and clamps to GRADIENT_MAX_STOPS. Returns true on success (count >= 2);
 * false if the array is missing, malformed, or has < 2 valid entries.
 * @p out is zeroed before parsing so a partial parse leaves a clean slate.
 */
bool gradient_stops_from_json(const cJSON *grad_stops_arr, gradient_stops_t *out);

/**
 * Convenience for migration: if @p out is empty but @p legacy_end is
 * given alongside a `base` colour, install a 2-stop array
 * [{0, base}, {100, legacy_end}]. Returns true if a migration happened.
 * Used by widgets whose old schema was a single end-colour + enabled
 * flag rather than a stops array.
 */
bool gradient_stops_install_legacy_2stop(gradient_stops_t *out,
                                         uint16_t base, uint16_t legacy_end);

/**
 * Serialise @p in into a `grad_stops` cJSON array (caller adds it to its
 * config object). Returns NULL when count < 2 (no gradient to emit) so
 * the caller's defaults-only logic naturally skips the field.
 */
cJSON *gradient_stops_to_json(const gradient_stops_t *in);

/**
 * Resolve the colour at position @p t (0.0..1.0) by lerping between the
 * two stops that bracket @p t. Returns a 16-bit RGB565. Falls back to
 * black when count < 2. Out-of-range @p t clamps to the nearest stop.
 */
uint16_t gradient_stops_sample(const gradient_stops_t *g, float t);

#ifdef __cplusplus
}
#endif
