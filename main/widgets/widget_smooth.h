#pragma once
/*
 * widget_smooth.h — reusable needle/fill smoothing for value-driven widgets.
 *
 * A meter needle / bar fill / arc only repaints when its bound value changes,
 * so it steps at the data rate (e.g. ~26 Hz on CAN) even when there's spare
 * CPU. This helper eases the *displayed* value toward the live value at the
 * display refresh rate, turning that spare CPU into smooth ~60 fps motion.
 *
 * Usage in a widget:
 *   - embed a `widget_smooth_t` in the per-instance data
 *   - widget_smooth_config(&s, w, apply_cb, range) once after create (range = max-min)
 *   - in the signal callback, after computing the clamped display value:
 *         widget_smooth_set(&s, value, is_stale);
 *     where apply_cb(w, v) pushes a value to the display (value-gated paint).
 *   - widget_smooth_free(&s) in destroy.
 *
 * smoothing_ms == 0 => instant (snaps; original behaviour). All calls run on
 * the LVGL task.
 */
#include "lvgl.h"
#include "widget_types.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*widget_smooth_apply_fn)(widget_t *w, float value);

typedef struct {
	uint16_t                smoothing_ms;  /* 0 = off (instant). Set from JSON. */
	float                   target;        /* latest value (display units) */
	float                   current;       /* eased displayed value */
	float                   range;         /* (max-min) for the settle epsilon */
	bool                    active;        /* currently easing */
	bool                    inited;        /* first value snaps (no glide from 0) */
	lv_timer_t             *timer;         /* lazy, self-pausing easing timer */
	widget_t               *w;
	widget_smooth_apply_fn  apply;
} widget_smooth_t;

/* Wire up the helper. Call after the widget's display objects exist. */
void widget_smooth_config(widget_smooth_t *s, widget_t *w,
                          widget_smooth_apply_fn apply, float range);

/* Feed a new value (already clamped to the widget's range, in display units).
 * Snaps when smoothing is off / first value / stale, otherwise eases. */
void widget_smooth_set(widget_smooth_t *s, float value, bool is_stale);

/* Drop any in-flight ease so the next value snaps (use on stale / rebind). */
void widget_smooth_reset(widget_smooth_t *s);

/* Stop + free the easing timer. Call from the widget's destroy. */
void widget_smooth_free(widget_smooth_t *s);

#ifdef __cplusplus
}
#endif
