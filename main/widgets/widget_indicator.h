#pragma once
#include "lvgl.h"
#include "ui/screens/ui_Screen3.h"
#include "widget_types.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ── Per-instance state for indicator widgets ──────────────────────────── */
typedef struct {
	uint8_t  slot;              /* 0=left, 1=right */
	uint8_t  input_source;      /* 0=Wire, 1=CAN */
	bool     animation_enabled;
	bool     is_momentary;
	bool     current_state;     /* runtime only -- NOT serialized */
	char     signal_name[32];
	int16_t  signal_index;
	/* Color-based state rendering (replaces opacity-based) */
	lv_color_t color_on;        /* default: amber 0xFFBF00 */
	uint8_t    opa_on;          /* default: 255 (fully visible) */
	lv_color_t color_off;       /* default: 0x333333 (dark grey) */
	uint8_t    opa_off;         /* default: 0 (invisible) */
} indicator_data_t;

/* --- API -----------------------------------------------------------------*/
/** Create indicator images and transparent touch areas on parent. */
void widget_indicator_create(lv_obj_t *parent);

/** Immediate (same-task) UI update for one indicator (0=left,1=right). */
void update_indicator_ui_immediate(uint8_t indicator_idx);

/** Async (lv_async_call-compatible) update — param is heap uint8_t* index. */
void update_indicator_ui(void *param);

/** Refresh the indicator config popup preview (if open). */
void update_config_preview(uint8_t indicator_idx);

/** Apply analog (wire) state to both indicators; skips CAN-sourced channels. */
void indicator_apply_analog_state(bool left_on, bool right_on);

/** LVGL timer callback for indicator blink animation. */
void indicator_animation_timer_cb(lv_timer_t *timer);

/** Create the full-screen indicator configuration editor. */
void create_indicator_config_menu(uint8_t indicator_idx);

/* The animation timer handle must be accessible from ui_Screen3.c for pause. */
extern lv_timer_t *indicator_animation_timer;

/**
 * Phase 2 — Factory function.
 * Allocates and returns a widget_t wired with the indicator vtable.
 * @param slot  0 = left indicator, 1 = right indicator.
 * @return      Heap-allocated widget_t *, caller must eventually call
 * w->destroy(w).
 */
widget_t *widget_indicator_create_instance(uint8_t slot);

/** Reset all indicator LVGL pointers (call before re-creating layout). */
void widget_indicator_reset(void);

/** Return the slot (0=left, 1=right) from an indicator widget's type_data. */
uint8_t widget_indicator_get_slot(const widget_t *w);

/** Return true if this indicator widget is bound to a signal. */
bool widget_indicator_has_signal(const widget_t *w);

#ifdef __cplusplus
}
#endif
