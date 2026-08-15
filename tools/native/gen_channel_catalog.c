/*
 * gen_channel_catalog.c — emit the firmware's channel catalogue as JSON.
 *
 * Host tool. Links the REAL firmware tables — canonical_channels.c and
 * preset_picker_data.c — and prints one JSON object on stdout:
 *
 *   { "canonical": [ ...92 canonical channel defs... ],
 *     "presets":   [ ...~350 flat ECU preconfig rows... ] }
 *
 * tools/gen_channel_catalog.py compiles + runs this and injects the output
 * into main/web/index.html between AUTO-GENERATED markers, giving the web
 * editor a catalogue that works with no dash on the network (ADR-0033).
 * Because this compiles the same arrays the firmware ships, the baked copy
 * cannot drift from the device: there is nothing to keep in sync, only one
 * table read twice. The CI check recompiles and byte-compares.
 *
 * Canonical field names mirror /api/channels/canonical exactly (minus the
 * per-device `active` flag and the derivable `group_name`), so the web can
 * consume either source through one code path. Preset rows are the raw
 * preconfig_item_t fields; the web groups them ECU→version itself.
 *
 * Build (from repo root — see gen_channel_catalog.py):
 *   cc tools/native/gen_channel_catalog.c \
 *      main/data/canonical_channels.c \
 *      main/ui/settings/preset_picker_data.c \
 *      -I main -I main/data -I main/ui/settings -I tests/native/mocks \
 *      -o build/gen_channel_catalog
 */
#include <stdio.h>
#include <string.h>

#include "canonical_channels.h"
#include "preset_picker.h"   /* preconfig_item_t — mock lvgl.h supplies types */

/* JSON string escape: backslash, quote, control chars. UTF-8 bytes (°, →)
 * pass through untouched — valid JSON, and byte-identical across runs. */
static void js(const char *s) {
	putchar('"');
	for (; s && *s; ++s) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
		else if (c < 0x20) printf("\\u%04x", c);
		else putchar(c);
	}
	putchar('"');
}

/* Numbers: integers print as integers, everything else at enough digits to
 * round-trip a float. Deterministic — same tool, same output. */
static void jn(double v) {
	if (v == (long long)v && v > -1e15 && v < 1e15)
		printf("%lld", (long long)v);
	else
		printf("%.9g", v);
}

int main(void) {
	printf("{\"canonical\":[");
	for (size_t i = 0; i < CANONICAL_CHANNEL_COUNT; ++i) {
		const canonical_channel_def_t *d = &CANONICAL_CHANNELS[i];
		if (i) putchar(',');
		printf("{\"id\":");            js(d->id);
		printf(",\"label\":");         js(d->label);
		printf(",\"group\":%d", (int)d->group);
		printf(",\"tier\":%d", (int)d->tier);
		printf(",\"cardinality\":%d", (int)d->card);
		printf(",\"units_native\":");  js(d->units_native);
		printf(",\"units_display_default\":"); js(d->units_display_def);
		printf(",\"decimals\":%d", (int)d->decimals);
		printf(",\"min_default\":");   jn(d->min_default);
		printf(",\"max_default\":");   jn(d->max_default);
		if (d->low_warn != CHANNEL_THRESHOLD_UNSET_LOW) {
			printf(",\"low_warn\":");  jn(d->low_warn);
		}
		if (d->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) {
			printf(",\"high_warn\":"); jn(d->high_warn);
		}
		printf(",\"color_normal\":%u", (unsigned)d->color_normal);
		if (d->notes) { printf(",\"notes\":"); js(d->notes); }
		putchar('}');
	}
	printf("],\"presets\":[");
	int first = 1;
	for (int i = 0; i < preconfig_items_count; ++i) {
		const preconfig_item_t *it = &preconfig_items[i];
		if (!it->ecu || !it->version || !it->label) continue;   /* sentinel */
		/* OBD2-marker rows are polled PIDs, not CAN broadcast decode —
		 * the ECU import path skips them on-device too (import-preset,
		 * source-options). Same rule here or the offline picker would
		 * offer rows the replay can never apply. */
		if (it->obd2_pid != 0) continue;
		if (!first) putchar(',');
		first = 0;
		printf("{\"ecu\":");        js(it->ecu);
		printf(",\"version\":");    js(it->version);
		printf(",\"label\":");      js(it->label);
		printf(",\"can_id\":");     js(it->can_id);   /* hex string, as stored */
		printf(",\"endian\":%d", (int)it->endianess);
		printf(",\"bit_start\":%d", (int)it->bit_start);
		printf(",\"bit_length\":%d", (int)it->bit_length);
		printf(",\"scale\":");      jn(it->scale);
		printf(",\"offset\":");     jn(it->value_offset);
		printf(",\"decimals\":%d", (int)it->decimals);
		printf(",\"is_signed\":%s", it->is_signed ? "true" : "false");
		/* Multiplexed rows (Link Generic Dash) need their frame gate in the
		 * baked catalogue too, or offline setup-from-zero would bind every
		 * Link channel to the same id with no gate — the exact breakage the
		 * on-device path was just fixed for. Emitted only when present so
		 * every other preset's JSON is unchanged. */
		if (it->mux_bit_length) {
			printf(",\"mux_bit_start\":%d",  (int)it->mux_bit_start);
			printf(",\"mux_bit_length\":%d", (int)it->mux_bit_length);
			printf(",\"mux_value\":%d",      (int)it->mux_value);
		}
		putchar('}');
	}
	printf("]}\n");
	return 0;
}
