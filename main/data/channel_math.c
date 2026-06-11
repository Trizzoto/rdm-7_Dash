/*
 * channel_math.c — derived ("calculated") channels. See channel_math.h.
 */

#include "channel_math.h"
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

bool channel_math_set(channel_t *c, const char *a_id, const char *b_id, uint8_t op) {
	if (!c || !a_id || !b_id || !a_id[0] || !b_id[0]) return false;
	if (op > CH_MATH_DIV) return false;
	/* Self-reference would feed back into the operand read one tick later
	 * and integrate garbage. Chains through OTHER math channels are fine. */
	if (strcmp(a_id, c->id) == 0 || strcmp(b_id, c->id) == 0) {
		ESP_LOGW(TAG, "'%s': operand cannot be the channel itself", c->id);
		return false;
	}
	if (!channel_manager_get(a_id) || !channel_manager_get(b_id)) {
		ESP_LOGW(TAG, "'%s': unknown operand channel ('%s' / '%s')",
		         c->id, a_id, b_id);
		return false;
	}

	strncpy(c->math_a, a_id, sizeof(c->math_a) - 1);
	c->math_a[sizeof(c->math_a) - 1] = '\0';
	strncpy(c->math_b, b_id, sizeof(c->math_b) - 1);
	c->math_b[sizeof(c->math_b) - 1] = '\0';
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

/* Read one operand's live value through its channel's bound signal.
 * Returns false (skip this evaluation) when unbound or stale. */
static bool _operand_value(const char *channel_id, float *out) {
	channel_t *c = channel_manager_get(channel_id);
	if (!c || c->signal_index < 0) return false;
	signal_t *s = signal_get_by_index((uint16_t)c->signal_index);
	if (!s || s->is_stale) return false;
	*out = s->current_value;
	return true;
}

static void _math_timer_cb(lv_timer_t *t) {
	(void)t;
	size_t n = channel_manager_count();
	for (size_t i = 0; i < n; i++) {
		channel_t *c = channel_manager_at(i);
		if (!c || !c->math_enabled || c->signal_name[0] == '\0') continue;

		float a, b;
		if (!_operand_value(c->math_a, &a)) continue;
		if (!_operand_value(c->math_b, &b)) continue;

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
