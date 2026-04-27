#pragma once
#include "lvgl.h"
#include "ui/screens/ui_Screen3.h"
#include "widget_types.h"
#include "widget_night_helpers.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ── Night-mode overrides for warning ──────────────────────────────────── */
typedef struct {
	NIGHT_FIELD_COLOR(active_color)
	NIGHT_FIELD_COLOR(inactive_color)
	NIGHT_FIELD_COLOR(border_color)
	NIGHT_FIELD_COLOR(label_color)
	NIGHT_FIELD_IMAGE(image_name, 64)
} warning_night_overrides_t;

/* ── Per-instance state for warning widgets ────────────────────────────── */
typedef struct {
	uint8_t    slot;
	lv_color_t active_color;
	char       label[32];
	bool       is_momentary;
	bool       invert_toggle;
	/* ── Appearance overrides ── */
	lv_color_t inactive_color;       /* default: THEME_COLOR_INACTIVE (0x292C29) */
	uint8_t    border_width;         /* default: 0 */
	lv_color_t border_color;         /* default: 0x000000 */
	uint8_t    radius;               /* default: 100 (circle) */
	bool       show_label;           /* default: true */
	lv_color_t label_color;          /* default: THEME_COLOR_TEXT_PRIMARY */
	char       label_font[32];       /* "Family:size", empty = THEME_FONT_TINY */
	int8_t     label_y_offset;       /* extra vertical shift, default 0 */
	uint8_t    label_text_align;     /* 0=Left, 1=Center, 2=Right (default 1) */
	char       image_name[64];       /* RDMIMG name; empty = circle mode */
	uint8_t    active_opa;           /* opacity when active (default 255) */
	uint8_t    inactive_opa;         /* opacity when inactive (default 180) */
	bool       current_state;     /* runtime only -- NOT serialized */
	char       signal_name[32];
	int16_t    signal_index;
	/* Runtime LVGL pointers (not serialized) */
	lv_img_dsc_t *img_dsc;          /* loaded RDMIMG descriptor, or NULL */
	lv_obj_t     *img_obj;          /* LVGL image object, or NULL */
	/* Dual-object night image swap: when night.image_name is set and differs
	 * from the day image, a sibling lv_img is created with the night image
	 * source. _warning_apply_night_mode toggles visibility between the two.
	 * Color recolors are applied to both in lock-step by
	 * update_warning_ui_immediate. NULL when no separate night image. */
	lv_img_dsc_t *night_img_dsc;
	lv_obj_t     *night_img_obj;
	/* Night-mode appearance overrides (only applied when night_mode active) */
	warning_night_overrides_t night;
} warning_data_t;

/* --- Objects exposed externally ------------------------------------------*/
/* warning_circles and warning_labels are file-scope statics in
   widget_warning.c; access is via update functions below. */

/* --- API ------------------------------------------------------------------*/

/** Create all 8 warning circles, labels and transparent touch zones on parent.
 */
void widget_warning_create(lv_obj_t *parent);

/** Async (lv_async_call-compatible) UI update for a single warning. */
void update_warning_ui(void *param);

/** Immediate (same-task) UI update for a single warning. */
void update_warning_ui_immediate(uint8_t warning_idx);

/** Timer callback — currently a stub (kept for future use). */
void check_warning_timeouts(lv_timer_t *timer);

/** Create the full-screen warning configuration editor. */
void create_warning_config_menu(uint8_t warning_idx);

/* Warning-specific config callbacks ----------------------------------------*/
void warning_high_threshold_event_cb(lv_event_t *e);
void warning_low_threshold_event_cb(lv_event_t *e);
void warning_high_color_event_cb(lv_event_t *e);
void warning_low_color_event_cb(lv_event_t *e);

/**
 * Phase 2 — Factory function.
 * Allocates and returns a widget_t wired with the warning vtable.
 * @param slot  Warning circle index 0–7.
 * @return      Heap-allocated widget_t *, caller must eventually call
 * w->destroy(w).
 */
widget_t *widget_warning_create_instance(uint8_t slot);

/** Reset all warning circle LVGL pointers (call before re-creating layout). */
void widget_warning_reset(void);

/** Return the slot (0-7) from a warning widget's type_data. */
uint8_t widget_warning_get_slot(const widget_t *w);

/** Return true if this warning widget is bound to a signal. */
bool widget_warning_has_signal(const widget_t *w);

/** Direct test-state driver — sets current_state + refreshes the visual
 *  for the given slot. Used by /api/warning/test so Studio's TEST ACTIVE
 *  button can preview the active visual even when no signal is bound. */
void widget_warning_apply_test_state(uint8_t slot, bool active);

#ifdef __cplusplus
}
#endif
