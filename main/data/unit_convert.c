/*
 * unit_convert.c — linear unit conversion table for channel display units.
 *
 * Each entry converts FROM -> TO as out = v * scale + offset. The reverse
 * direction (TO -> FROM) is derived automatically as in = (v - offset) / scale,
 * so only one direction per pair is listed. Unit strings must match the
 * canonical channel unit literals exactly (UTF-8, e.g. "°C").
 */

#include "unit_convert.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

typedef struct {
	const char *from;
	const char *to;
	float       scale;
	float       offset;
} unit_conv_t;

static const unit_conv_t CONVS[] = {
	/* Pressure (native is usually kPa). */
	{"kPa",  "bar",  0.01f,         0.0f},
	{"kPa",  "psi",  0.14503774f,   0.0f},
	{"kPa",  "MPa",  0.001f,        0.0f},
	{"kPa",  "inHg", 0.29529983f,   0.0f},
	{"bar",  "psi",  14.503774f,    0.0f},
	{"bar",  "MPa",  0.1f,          0.0f},

	/* Temperature. */
	{"°C",   "°F",   1.8f,          32.0f},
	{"°C",   "K",    1.0f,          273.15f},

	/* Speed. */
	{"km/h", "mph",  0.62137119f,   0.0f},
	{"m/s",  "km/h", 3.6f,          0.0f},
	{"m/s",  "mph",  2.23693629f,   0.0f},

	/* Air-fuel ratio (gasoline stoich: λ 1.0 = 14.7 AFR). Lets a wideband
	 * reported as lambda show as AFR, or an AFR sensor read back as lambda. */
	{"λ",    "AFR",  14.7f,         0.0f},
};

#define N_CONVS (sizeof(CONVS) / sizeof(CONVS[0]))

float unit_convert(float v, const char *from, const char *to) {
	if (!from || !to || !from[0] || !to[0] || strcmp(from, to) == 0)
		return v;
	for (size_t i = 0; i < N_CONVS; ++i) {
		if (strcmp(from, CONVS[i].from) == 0 && strcmp(to, CONVS[i].to) == 0)
			return v * CONVS[i].scale + CONVS[i].offset;
		/* Reverse direction. */
		if (strcmp(from, CONVS[i].to) == 0 && strcmp(to, CONVS[i].from) == 0) {
			if (CONVS[i].scale == 0.0f) return v; /* guard (never in table) */
			return (v - CONVS[i].offset) / CONVS[i].scale;
		}
	}
	return v; /* unknown pair — pass through unchanged (relabel, no scale) */
}

size_t unit_convert_targets(const char *from, const char **out, size_t max) {
	if (!from || !from[0] || !out || max == 0) return 0;
	size_t n = 0;
	for (size_t i = 0; i < N_CONVS && n < max; ++i) {
		const char *cand = NULL;
		if (strcmp(from, CONVS[i].from) == 0)      cand = CONVS[i].to;
		else if (strcmp(from, CONVS[i].to) == 0)   cand = CONVS[i].from;
		if (!cand) continue;
		bool dup = false;
		for (size_t j = 0; j < n; ++j)
			if (strcmp(out[j], cand) == 0) { dup = true; break; }
		if (!dup) out[n++] = cand;
	}
	return n;
}

bool unit_convert_supported(const char *from, const char *to) {
	if (!from || !to || !from[0] || !to[0]) return false;
	if (strcmp(from, to) == 0) return true; /* identity is trivially fine */
	for (size_t i = 0; i < N_CONVS; ++i) {
		if ((strcmp(from, CONVS[i].from) == 0 && strcmp(to, CONVS[i].to) == 0) ||
		    (strcmp(from, CONVS[i].to) == 0 && strcmp(to, CONVS[i].from) == 0))
			return true;
	}
	return false;
}
