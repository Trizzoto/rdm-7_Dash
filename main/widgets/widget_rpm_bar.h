#pragma once
#include "lvgl.h"
#include "widget_types.h"
#include "widget_night_helpers.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ── Night-mode overrides for rpm_bar ──────────────────────────────────── */
typedef struct {
	NIGHT_FIELD_COLOR(bar_color)
	NIGHT_FIELD_COLOR(limiter_color)
} rpm_bar_night_overrides_t;

/* ── Per-instance state for RPM bar widget ─────────────────────────────── */
typedef struct {
	int32_t  gauge_max;
	int32_t  redline;
	lv_color_t bar_color;
	/* Limiter effect: applied when RPM >= limiter_value.
	 *   0 = None       — no visual change at the limiter threshold
	 *   1 = Bar Flash  — bar background toggles between bar_color and limiter_color
	 *                    every flash_speed_ms milliseconds
	 *   2 = Bar Solid  — bar background goes solid limiter_color (no flashing) */
	uint8_t  limiter_effect;
	int32_t  limiter_value;
	lv_color_t limiter_color;
	uint16_t flash_speed_ms;   /* default 200, range 50..1000 — flash period for effect=1 */
	char     signal_name[32];
	int16_t  signal_index;
	/* Night-mode appearance overrides (only applied when night_mode active) */
	rpm_bar_night_overrides_t night;
} rpm_bar_data_t;

/** Create the RPM widget container with bar gauge, redline, tick marks, Panel9.
 *  Returns the container lv_obj_t so the caller can set w->root. */
lv_obj_t *widget_rpm_bar_create(lv_obj_t *parent);

/** Immediate RPM UI update (called on LVGL thread). */
void update_rpm_ui_immediate(const char *rpm_str, int rpm_value);

/** Async RPM UI update (lv_async_call compatible). */
void update_rpm_ui(void *param);

/** Update the RPM bar gauge value + limiter/background effects. */
void set_rpm_value(int rpm);

/** Reposition the redline zone overlay after gauge_max or redline change. */
void update_redline_position(void);

/** Rebuild all RPM tick marks for the current rpm_gauge_max. */
void update_rpm_lines(lv_obj_t *parent);

/** Create the full RPM bar (base panels + bar widget + redline zone). */
void create_rpm_bar_gauge(lv_obj_t *parent_screen);

/** Clear stale static pointers (tick mark lines and labels) on layout reload. */
void widget_rpm_bar_clear_stale_pointers(void);

/** Timer callback for deferred RPM colour update. */
void check_rpm_color_update(lv_timer_t *timer);

/** RPM-specific config dropdown/roller callbacks (registered by config_modal).
 */
void rpm_gauge_roller_event_cb(lv_event_t *e);
void rpm_redline_roller_event_cb(lv_event_t *e);
void rpm_ecu_dropdown_event_cb(lv_event_t *e);
void rpm_color_dropdown_event_cb(lv_event_t *e);
void rpm_limiter_effect_dropdown_event_cb(lv_event_t *e);
void rpm_limiter_roller_event_cb(lv_event_t *e);
void rpm_limiter_color_dropdown_event_cb(lv_event_t *e);
void rpm_flash_speed_dropdown_event_cb(lv_event_t *e);
/** Color-wheel popup creators. */
void create_rpm_color_wheel_popup(void);
void create_limiter_color_wheel_popup(void);

/** Returns pointer to last_rpm_can_received for dispatcher timeout tracking. */
uint64_t *widget_rpm_bar_get_last_can_time(void);

/**
 * Phase 2 — Factory function.
 * Allocates and returns a widget_t wired with the rpm_bar vtable.
 * @return Heap-allocated widget_t *, caller must eventually call w->destroy(w).
 */
widget_t *widget_rpm_bar_create_instance(void);

#ifdef __cplusplus
}
#endif
