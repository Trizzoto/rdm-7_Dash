/*
 * channel_math.c — derived ("calculated") channels. See channel_math.h.
 */

#include "channel_math.h"
#include "unit_convert.h"
#include "widgets/signal.h"

#include <ctype.h>
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "channel_math";

static lv_timer_t *s_math_timer = NULL;

const char *channel_math_signal_name(const channel_t *c, char *buf, size_t cap) {
	if (!buf || cap == 0) return buf;
	buf[0] = '\0';
	if (!c) return buf;
	size_t n = 0;
	const char *prefix = "MATH_";
	while (prefix[n] && n < cap - 1) { buf[n] = prefix[n]; n++; }
	for (size_t i = 0; c->id[i] && n < cap - 1; i++, n++)
		buf[n] = (char)toupper((unsigned char)c->id[i]);
	buf[n] = '\0';
	return buf;
}

/* Register (upsert) the synthetic output signal for one math channel and
 * make sure the channel is bound to it. bind=false skips the set_signal
 * persistence round-trip (load path: the binding was already persisted). */
static void _register_output_signal(channel_t *c) {
	char sig_name[32];
	channel_math_signal_name(c, sig_name, sizeof(sig_name));
	signal_register_with_source(sig_name, /*can_id=*/0, 0, 0,
	                            1.0f, 0.0f, false, 1,
	                            c->units_native, SIGNAL_SOURCE_INTERNAL);
}

/* Validate one operand and copy it onto the channel fields. Returns false
 * on a bad channel reference. */
static bool _store_operand(channel_t *c, const channel_math_operand_t *o,
                           char *id_field, size_t id_cap,
                           bool *is_const_field, float *const_field) {
	if (o->is_const) {
		if (!isfinite(o->value)) return false;
		id_field[0]     = '\0';
		*is_const_field = true;
		*const_field    = o->value;
		return true;
	}
	if (!o->channel_id || !o->channel_id[0]) return false;
	/* Self-reference would feed back into the operand read one tick later
	 * and integrate garbage. Chains through OTHER math channels are fine. */
	if (strcmp(o->channel_id, c->id) == 0) {
		ESP_LOGW(TAG, "'%s': operand cannot be the channel itself", c->id);
		return false;
	}
	if (!channel_manager_get(o->channel_id)) {
		ESP_LOGW(TAG, "'%s': unknown operand channel '%s'", c->id, o->channel_id);
		return false;
	}
	strncpy(id_field, o->channel_id, id_cap - 1);
	id_field[id_cap - 1] = '\0';
	*is_const_field = false;
	*const_field    = 0.0f;
	return true;
}

bool channel_math_set(channel_t *c, const channel_math_operand_t *a,
                      const channel_math_operand_t *b, uint8_t op) {
	if (!c || !a || !b) return false;
	if (op > CH_MATH_DIV) return false;
	/* const ∘ const is a fixed number — that's not a derived channel, and
	 * with no live operand the output would never refresh. Reject. */
	if (a->is_const && b->is_const) {
		ESP_LOGW(TAG, "'%s': at least one operand must be a channel", c->id);
		return false;
	}

	if (!_store_operand(c, a, c->math_a, sizeof(c->math_a),
	                    &c->math_a_is_const, &c->math_a_const)) return false;
	if (!_store_operand(c, b, c->math_b, sizeof(c->math_b),
	                    &c->math_b_is_const, &c->math_b_const)) return false;
	c->math_op = op;
	c->math_enabled = true;

	_register_output_signal(c);
	char sig_name[32];
	channel_math_signal_name(c, sig_name, sizeof(sig_name));
	/* set_signal binds + notifies listeners + persists synchronously (same
	 * high-value-edit policy as decode edits). Also covers the dirty mark
	 * that persists the math block itself. */
	channel_manager_set_signal(c, sig_name);

	ESP_LOGI(TAG, "'%s' = '%s' op%u '%s' -> signal %s",
	         c->id, c->math_a, c->math_op, c->math_b, sig_name);
	return true;
}

bool channel_math_clear(channel_t *c) {
	if (!c) return false;
	if (!c->math_enabled) return false;
	c->math_enabled = false;
	c->math_a[0] = '\0';
	c->math_b[0] = '\0';
	c->math_a_is_const = c->math_b_is_const = false;
	c->math_a_const = c->math_b_const = 0.0f;
	c->math_op = 0;
	/* Unbind from the synthetic signal — reverts the channel to "no source"
	 * so the regular picker takes over. Persists + notifies. */
	char sig_name[32];
	channel_math_signal_name(c, sig_name, sizeof(sig_name));
	if (strcmp(c->signal_name, sig_name) == 0)
		channel_manager_set_signal(c, "");
	else
		channel_manager_mark_dirty();
	return true;
}

void channel_math_register_signals(void) {
	size_t n = channel_manager_count();
	for (size_t i = 0; i < n; i++) {
		channel_t *c = channel_manager_at(i);
		if (c && c->math_enabled)
			_register_output_signal(c);
	}
}

/* Resolve one operand to a live value + the unit it is expressed in.
 * Channel operands read their bound signal's NATIVE value and report the
 * channel's units_native (NULL when the channel has none). Constants carry
 * no unit — they adopt the other operand's unit by construction. Returns
 * false (skip this evaluation) when a channel operand is unbound/stale. */
static bool _operand_resolve(const char *channel_id, bool is_const,
                             float const_value, float *out_v,
                             const char **out_unit) {
	if (is_const) {
		*out_v = const_value;
		*out_unit = NULL;
		return true;
	}
	channel_t *c = channel_manager_get(channel_id);
	if (!c || c->signal_index < 0) return false;
	signal_t *s = signal_get_by_index((uint16_t)c->signal_index);
	if (!s || s->is_stale) return false;
	*out_v = s->current_value;
	*out_unit = c->units_native[0] ? c->units_native : NULL;
	return true;
}

static void _math_timer_cb(lv_timer_t *t) {
	(void)t;
	size_t n = channel_manager_count();
	for (size_t i = 0; i < n; i++) {
		channel_t *c = channel_manager_at(i);
		if (!c || !c->math_enabled || c->signal_name[0] == '\0') continue;

		float a, b;
		const char *a_unit, *b_unit;
		if (!_operand_resolve(c->math_a, c->math_a_is_const,
		                      c->math_a_const, &a, &a_unit)) continue;
		if (!_operand_resolve(c->math_b, c->math_b_is_const,
		                      c->math_b_const, &b, &b_unit)) continue;

		/* Commensurate: two channel operands in different but convertible
		 * units → bring B into A's unit so kPa − psi means what it should.
		 * Unknown pairs pass through (unit_convert is identity for those). */
		if (a_unit && b_unit && strcmp(a_unit, b_unit) != 0)
			b = unit_convert(b, b_unit, a_unit);

		/* The result's unit: whichever channel operand supplied one (after
		 * commensuration both channel operands are in a_unit; a constant
		 * adopts the channel operand's unit). */
		const char *res_unit = a_unit ? a_unit : b_unit;

		float v;
		switch (c->math_op) {
		case CH_MATH_ADD: v = a + b; break;
		case CH_MATH_SUB: v = a - b; break;
		case CH_MATH_MUL: v = a * b; break;
		case CH_MATH_DIV:
			if (fabsf(b) < 1e-9f) continue; /* hold last value; goes stale */
			v = a / b;
			break;
		default: continue;
		}
		if (!isfinite(v)) continue;

		/* Output-unit conversion: honour this channel's units_native when
		 * the result is dimensionally that of the channel operand — i.e.
		 * for + and −, or when one operand is a constant (scaling keeps
		 * the dimension). channel × / ÷ channel changes the dimension
		 * (kPa² , ratio); a linear convert there would silently mis-scale,
		 * so those pass through raw and the output unit is the user's
		 * choice to label. */
		bool dim_preserved = (c->math_op == CH_MATH_ADD || c->math_op == CH_MATH_SUB ||
		                      c->math_a_is_const || c->math_b_is_const);
		if (dim_preserved && res_unit && c->units_native[0] &&
		    strcmp(res_unit, c->units_native) != 0)
			v = unit_convert(v, res_unit, c->units_native);

		/* Normal external-push path: peaks, freshness, subscriber notify,
		 * channel zone evaluation all run as for any other live signal. */
		signal_set_external_value(c->signal_name, v);
	}
}

void channel_math_start(void) {
	if (s_math_timer) return;
	/* 200 ms: math sources are gauges (boost, deltas), not control loops —
	 * 5 Hz matches the internal-signal cadence class and costs ~nothing. */
	s_math_timer = lv_timer_create(_math_timer_cb, 200, NULL);
	ESP_LOGI(TAG, "math channel evaluator started (200 ms)");
}
