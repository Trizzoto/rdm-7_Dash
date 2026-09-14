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
	/* When true, `scale` is ignored and the live stoichiometric ratio is used
	 * instead — see unit_convert_set_stoich(). Only λ <-> AFR needs this. */
	bool        stoich;
} unit_conv_t;

/* The live stoich. A plain float: written from the LVGL task, read from the
 * LVGL and HTTP tasks. A single word store/load is atomic on the S3, and the
 * worst a torn read could produce is one frame at the previous ratio. */
static float s_stoich = UNIT_STOICH_PETROL;

bool unit_convert_set_stoich(float afr) {
	/* NaN fails both comparisons, so this also refuses non-finite input. */
	if (!(afr >= UNIT_STOICH_MIN && afr <= UNIT_STOICH_MAX)) return false;
	s_stoich = afr;
	return true;
}

float unit_convert_get_stoich(void) {
	return s_stoich;
}

static const unit_conv_t CONVS[] = {
	/* Pressure (native is usually kPa). */
	{"kPa",  "bar",  0.01f,         0.0f, false},
	{"kPa",  "psi",  0.14503774f,   0.0f, false},
	{"kPa",  "MPa",  0.001f,        0.0f, false},
	{"kPa",  "inHg", 0.29529983f,   0.0f, false},
	{"bar",  "psi",  14.503774f,    0.0f, false},
	{"bar",  "MPa",  0.1f,          0.0f, false},

	/* Temperature. */
	{"°C",   "°F",   1.8f,          32.0f, false},
	{"°C",   "K",    1.0f,          273.15f, false},

	/* Speed. */
	{"km/h", "mph",  0.62137119f,   0.0f, false},
	{"m/s",  "km/h", 3.6f,          0.0f, false},
	{"m/s",  "mph",  2.23693629f,   0.0f, false},

	/* Air-fuel ratio. Lets a wideband reported as lambda show as AFR, or an
	 * AFR sensor read back as lambda. The factor is the live stoich, not a
	 * constant: λ 1.00 is 14.7 AFR on petrol but 9.8 on E85, and hardcoding
	 * 14.7 showed an E85 car petrol-equivalent AFR that disagreed with its
	 * own tuning software. 14.7 below is documentation only. */
	{"λ",    "AFR",  14.7f,         0.0f, true},
};

#define N_CONVS (sizeof(CONVS) / sizeof(CONVS[0]))

float unit_convert(float v, const char *from, const char *to) {
	if (!from || !to || !from[0] || !to[0] || strcmp(from, to) == 0)
		return v;
	for (size_t i = 0; i < N_CONVS; ++i) {
		float scale = CONVS[i].stoich ? s_stoich : CONVS[i].scale;
		if (strcmp(from, CONVS[i].from) == 0 && strcmp(to, CONVS[i].to) == 0)
			return v * scale + CONVS[i].offset;
		/* Reverse direction. */
		if (strcmp(from, CONVS[i].to) == 0 && strcmp(to, CONVS[i].from) == 0) {
			if (scale == 0.0f) return v; /* guard (never in table) */
			return (v - CONVS[i].offset) / scale;
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
