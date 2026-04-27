#pragma once
#include "lvgl.h"
#include "ui/screens/ui_Screen3.h"
#include "widget_types.h"
#include "widget_night_helpers.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ── Night-mode overrides for bar ──────────────────────────────────────── */
typedef struct {
	NIGHT_FIELD_COLOR(bar_low_color)
	NIGHT_FIELD_COLOR(bar_high_color)
	NIGHT_FIELD_COLOR(bar_in_range_color)
	NIGHT_FIELD_COLOR(bar_bg_color)
	NIGHT_FIELD_COLOR(bar_border_color)
	NIGHT_FIELD_COLOR(label_color)
	NIGHT_FIELD_COLOR(value_color)
	NIGHT_FIELD_IMAGE(bar_image, 64)
	NIGHT_FIELD_IMAGE(bar_image_full, 64)
} bar_night_overrides_t;

/* ── Per-instance state for bar widgets ────────────────────────────────── */
typedef struct {
	uint8_t  slot;              /* 0=BAR1, 1=BAR2 */
	char     label[32];
	int32_t  bar_min;
	int32_t  bar_max;
	int32_t  bar_low;
	int32_t  bar_high;
	lv_color_t bar_low_color;
	lv_color_t bar_high_color;
	lv_color_t bar_in_range_color;
	bool     show_bar_value;
	bool     invert_bar_value;
	uint8_t  decimals;
	char     label_font[32];
	char     value_font[32];
	/* ── Appearance overrides ── */
	lv_color_t bar_bg_color;         /* default: THEME_COLOR_PANEL */
	uint8_t    bar_radius;           /* default: 5 */
	uint8_t    bar_border_width;     /* default: 2 */
	lv_color_t bar_border_color;     /* default: THEME_COLOR_PANEL */
	uint8_t    indicator_radius;     /* default: 5 */
	lv_color_t label_color;          /* default: THEME_COLOR_TEXT_PRIMARY */
	lv_color_t value_color;          /* default: THEME_COLOR_TEXT_PRIMARY */
	/* ── Image-based bar (optional) ── */
	/* Anchor-based non-linear scale. Maps a single DATA value to a specific
	 * position along the bar fill, splitting the range into two linear
	 * segments. Use case: "I want 90°C coolant at 50% of the bar so the
	 * danger zone (90-110) takes the second half." Default leaves the bar
	 * linear (anchor at midpoint, position 50%). */
	int32_t  anchor_value;           /* default: bar_min + (bar_max-bar_min)/2 */
	uint8_t  anchor_position;        /* 0..100, default: 50 */
	bool     anchor_enabled;         /* false (default) = linear pass-through */
	char     bar_image[64];          /* track/background image name (default: "") */
	char     bar_image_full[64];     /* fill image name (default: "") */
	lv_img_dsc_t *bar_img_dsc;      /* runtime: loaded track image descriptor */
	lv_img_dsc_t *bar_img_full_dsc;  /* runtime: loaded fill image descriptor */
	lv_obj_t *img_bg_obj;            /* runtime: background image LVGL object */
	lv_obj_t *img_full_obj;          /* runtime: full image LVGL object */
	lv_obj_t *img_clip_obj;          /* runtime: clipping container for fill */
	char     signal_name[32];
	int16_t  signal_index;
	/* LVGL object pointers (runtime only, per-instance) */
	lv_obj_t *bar_obj;
	lv_obj_t *label_obj;
	lv_obj_t *value_obj;
	/* Runtime tracking for night-mode image swap — name currently loaded
	 * into bar_img_dsc / bar_img_full_dsc, so we only reload on a change. */
	char     current_bar_image[64];
	char     current_bar_image_full[64];
	/* Night-mode appearance overrides (only applied when night_mode active) */
	bar_night_overrides_t night;
} bar_data_t;

/** Create BAR1 and BAR2 horizontal bar widgets and their labels on parent. */
void widget_bar_create(lv_obj_t *parent);

/** Immediate bar UI update. bar_index: 0=BAR1, 1=BAR2. */
void update_bar_ui_immediate(int bar_index, int32_t bar_value,
							 double final_value, int config_index);

/** Async bar UI update (lv_async_call compatible). */
void update_bar_ui(void *param);

/** Color-wheel popup creators for bar thresholds. */
void create_bar_low_color_wheel_popup(uint8_t value_id);
void create_bar_high_color_wheel_popup(uint8_t value_id);
void create_bar_in_range_color_wheel_popup(uint8_t value_id);

/** Reapply the bar's internal LVGL range based on bar_min / bar_max scaled
 *  by 10^decimals. Call from config_modal whenever any of those fields
 *  change so the live widget reflects the new resolution immediately.
 *  No-op on image-mode bars (they recompute fill percentage every frame). */
void widget_bar_sync_range(bar_data_t *bd);

/** Config callbacks for bar range/threshold inputs (used by config_modal). */
void bar_range_input_event_cb(lv_event_t *e);
void bar_low_value_event_cb(lv_event_t *e);
void bar_high_value_event_cb(lv_event_t *e);
void bar_low_color_event_cb(lv_event_t *e);
void bar_high_color_event_cb(lv_event_t *e);
void bar_in_range_color_event_cb(lv_event_t *e);

/** Returns pointer to last_bar_can_received[bar_idx] for timeout tracking. */
uint64_t *widget_bar_get_last_can_time(uint8_t bar_idx);

/* Bar value labels are defined here; ui.h already declares ui_Bar_1_Value etc.
   but the actual definitions live in widget_bar.c */
extern lv_obj_t *ui_Bar_1_Label;
extern lv_obj_t *ui_Bar_2_Label;
extern lv_obj_t *ui_Bar_1_Value;
extern lv_obj_t *ui_Bar_2_Value;

/**
 * Phase 2 — Factory function.
 * Allocates and returns a widget_t wired with the bar vtable.
 * @param slot  0 = BAR1, 1 = BAR2.
 * @return      Heap-allocated widget_t *, caller must eventually call
 * w->destroy(w).
 */
widget_t *widget_bar_create_instance(uint8_t slot);

uint8_t widget_bar_get_slot(const widget_t *w);
bool widget_bar_has_signal(const widget_t *w);

#ifdef __cplusplus
}
#endif
