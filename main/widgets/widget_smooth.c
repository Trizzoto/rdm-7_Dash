/* widget_smooth.c — see widget_smooth.h */
#include "widget_smooth.h"
#include <math.h>

#define SMOOTH_PERIOD_MS 16   /* easing tick ~= display refresh */

/* A jump larger than this fraction of the range is a real, fast movement (a rev,
 * a hard brake) — not the few-LSB jitter the filter exists to hide. Snap to it
 * instantly so the gauge never lags a genuine event behind the smoothing: speed
 * over smoothness for big moves, smoothing only for the small stuff. */
#define SMOOTH_SNAP_FRAC 0.15f

static void _smooth_cb(lv_timer_t *t) {
	widget_smooth_t *s = (widget_smooth_t *)t->user_data;
	if (!s || !s->active || !s->apply || !s->w || !s->w->root ||
	    !lv_obj_is_valid(s->w->root)) {
		if (s) s->active = false;
		lv_timer_pause(t);
		return;
	}
	float diff = s->target - s->current;
	float settle = s->range * 0.0008f;
	if (settle < 1e-4f) settle = 1e-4f;
	if (fabsf(diff) <= settle) {
		s->current = s->target;
		s->active = false;
		lv_timer_pause(t);
	} else {
		float k = s->smoothing_ms ? (float)SMOOTH_PERIOD_MS / (float)s->smoothing_ms : 1.0f;
		if (k > 1.0f) k = 1.0f;
		if (k < 0.05f) k = 0.05f;   /* floor so very large smoothing_ms still converges */
		s->current += diff * k;
	}
	s->apply(s->w, s->current);
}

void widget_smooth_config(widget_smooth_t *s, widget_t *w,
                          widget_smooth_apply_fn apply, float range) {
	if (!s) return;
	s->w = w;
	s->apply = apply;
	s->range = range;
}

void widget_smooth_set(widget_smooth_t *s, float value, bool is_stale) {
	if (!s || !s->apply || !s->w) return;
	if (is_stale || s->smoothing_ms == 0 || !s->inited) {
		s->current = value;
		s->target  = value;
		s->inited  = true;
		s->active  = false;
		if (s->timer) lv_timer_pause(s->timer);
		s->apply(s->w, value);
	} else if (s->range > 0.0f &&
	           fabsf(value - s->current) > s->range * SMOOTH_SNAP_FRAC) {
		/* Big jump: snap instead of easing so a real fast move tracks now. */
		s->current = value;
		s->target  = value;
		s->active  = false;
		if (s->timer) lv_timer_pause(s->timer);
		s->apply(s->w, value);
	} else {
		s->target = value;
		if (!s->timer) {
			s->timer = lv_timer_create(_smooth_cb, SMOOTH_PERIOD_MS, s);
			if (s->timer) lv_timer_pause(s->timer);
		}
		if (!s->active && s->timer) {
			s->active = true;
			lv_timer_resume(s->timer);
		}
	}
}

void widget_smooth_reset(widget_smooth_t *s) {
	if (!s) return;
	s->inited = false;
	s->active = false;
	if (s->timer) lv_timer_pause(s->timer);
}

void widget_smooth_free(widget_smooth_t *s) {
	if (s && s->timer) {
		lv_timer_del(s->timer);
		s->timer = NULL;
	}
	if (s) s->active = false;
}
