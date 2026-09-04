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
                      const channel_math_operand_t *b, uint8_t op,
                      const channel_math_operand_t *cc, uint8_t op2) {
	if (!c || !a || !b) return false;
	if (op > CH_MATH_DIV) return false;
	if (cc && op2 > CH_MATH_DIV) return false;
	/* All-constant is a fixed number — that's not a derived channel, and
	 * with no live operand the output would never refresh. Reject. */
	if (a->is_const && b->is_const && (!cc || cc->is_const)) {
		ESP_LOGW(TAG, "'%s': at least one operand must be a channel", c->id);
		return false;
	}

	/* Validate all three into locals BEFORE touching the channel. A third
	 * operand that fails validation must not leave the first two written
	 * over the channel's previous, working expression — the caller is told
	 * "no" and nothing has changed. */
	/* Zero-initialised: a constant operand only writes the terminator, and
	 * the whole field is memcpy'd onto the channel below. */
	char  ids[3][sizeof(c->math_a)] = {{0}};
	bool  is_const[3] = {0};
	float consts[3]   = {0};
	const channel_math_operand_t *ops[3] = { a, b, cc };
	for (int i = 0; i < (cc ? 3 : 2); i++) {
		if (!_store_operand(c, ops[i], ids[i], sizeof(ids[i]),
		                    &is_const[i], &consts[i])) return false;
	}

	memcpy(c->math_a, ids[0], sizeof(c->math_a));
	memcpy(c->math_b, ids[1], sizeof(c->math_b));
	c->math_a_is_const = is_const[0];
	c->math_b_is_const = is_const[1];
	c->math_a_const    = consts[0];
	c->math_b_const    = consts[1];
	if (cc) {
		memcpy(c->math_c, ids[2], sizeof(c->math_c));
		c->math_c_is_const = is_const[2];
		c->math_c_const    = consts[2];
		c->math_op2        = op2;
		c->math_c_enabled  = true;
	} else {
		c->math_c[0]       = '\0';
		c->math_c_is_const = false;
		c->math_c_const    = 0.0f;
		c->math_op2        = 0;
		c->math_c_enabled  = false;
	}
	c->math_op = op;
	c->math_enabled = true;

	_register_output_signal(c);
	char sig_name[32];
	channel_math_signal_name(c, sig_name, sizeof(sig_name));
	/* set_signal binds + notifies listeners + persists synchronously (same
	 * high-value-edit policy as decode edits). Also covers the dirty mark
	 * that persists the math block itself. */
	channel_manager_set_signal(c, sig_name);

	if (c->math_c_enabled)
		ESP_LOGI(TAG, "'%s' = ('%s' op%u '%s') op%u '%s' -> signal %s",
		         c->id, c->math_a, c->math_op, c->math_b,
		         c->math_op2, c->math_c, sig_name);
	else
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
	c->math_c[0] = '\0';
	c->math_a_is_const = c->math_b_is_const = c->math_c_is_const = false;
	c->math_a_const = c->math_b_const = c->math_c_const = 0.0f;
	c->math_op = 0;
	c->math_op2 = 0;
	c->math_c_enabled = false;
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

/* Fold one operand into the running (value, unit) pair.
 *
 * The running unit is what the expression is currently expressed in, or
 * NULL once the dimension is something no unit here can name. Rules, in
 * one place so the two-operand and three-operand forms cannot disagree:
 *
 *   + and −  keep the dimension. Bring the operand into the running unit
 *            when both are known; adopt the operand's unit when the
 *            running value is a bare constant with no unit of its own.
 *   × and ÷  by a CONSTANT keep the dimension too — scaling by a number
 *            does not change what the number measures. This is what makes
 *            the × 100 in an L/100km expression harmless.
 *   × and ÷  by a CHANNEL do not: L/h ÷ km/h is neither L/h nor km/h, so
 *            the running unit is dropped. A linear unit_convert on a
 *            dimension it was never given would silently mis-scale.
 *
 * @p derived tracks WHY the running unit is NULL — "not a unit anyone
 * named" (a bare constant, which may still adopt one) vs "a dimension we
 * have deliberately dropped" (which must never adopt one again).
 *
 * Returns false when the operand makes the result meaningless (divide by
 * ~zero): the caller skips this tick and the output goes stale on its own.
 */
static bool _fold(float *v, const char **unit, bool *derived,
                  uint8_t op, float ov, const char *ounit) {
	switch (op) {
	case CH_MATH_ADD:
	case CH_MATH_SUB:
		if (*unit && ounit && strcmp(ounit, *unit) != 0)
			ov = unit_convert(ov, ounit, *unit);
		*v = (op == CH_MATH_ADD) ? (*v + ov) : (*v - ov);
		if (!*unit && ounit && !*derived) *unit = ounit;
		return true;
	case CH_MATH_MUL:
		*v *= ov;
		if (ounit) {
			if (!*unit && !*derived) *unit = ounit;   /* number × channel */
			else { *unit = NULL; *derived = true; }
		}
		return true;
	case CH_MATH_DIV:
		if (fabsf(ov) < 1e-9f) return false;
		*v /= ov;
		/* Even number ÷ channel inverts the dimension (1/unit), which no
		 * unit string here names — unlike ×, there is nothing to adopt. */
		if (ounit) { *unit = NULL; *derived = true; }
		return true;
	default:
		return false;
	}
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

		float v = a;
		const char *res_unit = a_unit;
		bool derived = false;
		if (!_fold(&v, &res_unit, &derived, c->math_op, b, b_unit)) continue;

		if (c->math_c_enabled) {
			float cv; const char *c_unit;
			if (!_operand_resolve(c->math_c, c->math_c_is_const,
			                      c->math_c_const, &cv, &c_unit)) continue;
			if (!_fold(&v, &res_unit, &derived, c->math_op2, cv, c_unit)) continue;
		}
		if (!isfinite(v)) continue;

		/* Output-unit conversion: honour this channel's units_native only
		 * while the running dimension is still one a unit names. Once it
		 * has been dropped (channel × / ÷ channel) the output unit is the
		 * user's to label — an L/100km channel is exactly that case. */
		if (res_unit && c->units_native[0] &&
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
