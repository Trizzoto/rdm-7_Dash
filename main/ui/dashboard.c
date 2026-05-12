#include "ui/dashboard.h"
#include "layout/layout_manager.h"
#include "widgets/font_manager.h"
#include "widgets/signal.h"
#include "widgets/signal_internal.h"
#include "widgets/widget_registry.h"
#include "system/night_mode.h"
#include "system/remote_touch.h"

/* Existing widget create functions — used as fallback */
#include "widgets/widget_bar.h"
#include "widgets/widget_indicator.h"
#include "widgets/widget_panel.h"
#include "widgets/widget_rpm_bar.h"
#include "widgets/widget_warning.h"

#include "ui/menu/edit_mode.h"
#include "ui/menu/menu_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/settings/device_settings.h"
#include "can/can_manager.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "dashboard";

/* ── Internal widget registry snapshot ───────────────────────────────────── */

/* Maximum widgets the dashboard tracks (5 types × worst-case instances):
 *   panel×8, rpm_bar×1, bar×2, indicator×2, warning×8, text×N, meter×N */
#define DASHBOARD_MAX_WIDGETS 32

static widget_t *s_widgets[DASHBOARD_MAX_WIDGETS];
static uint8_t s_widget_count = 0;

/* ── Signal timeout timer ─────────────────────────────────────────────────── */

static lv_timer_t *s_signal_timeout_timer = NULL;

static void _signal_timeout_cb(lv_timer_t *timer) {
	(void)timer;
	uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
	signal_check_timeouts(now_ms);
}

/* ── Accessors ───────────────────────────────────────────────────────────── */

widget_t **dashboard_get_widgets(void) { return s_widgets; }
uint8_t dashboard_get_widget_count(void) { return s_widget_count; }

/* ── Fallback: directly call the existing widget create functions ─────────
 *
 * Used when layout_manager_load() fails (e.g. corrupted file, FS error).
 * This guarantees the screen always renders correctly.
 * ────────────────────────────────────────────────────────────────────────── */
static void _fallback_create_all(lv_obj_t *parent) {
	ESP_LOGW(TAG, "Using fallback direct widget creation");
	widget_panel_create(parent);
	widget_rpm_bar_create(parent);
	widget_bar_create(parent);
	widget_indicator_create(parent);
	widget_warning_create(parent);
	/* s_widget_count stays 0 — no widget_t handles in this path */
}

/* ── Long-press callback: opens config modal for the tapped widget ──────── */

static void _widget_long_press_cb(lv_event_t *e) {
	/* Long-press is a no-op in live mode — only inspects while Edit Mode is
	 * armed. Prevents accidental opens during normal driving (Button widgets
	 * legitimately held for >500 ms, etc.). */
	if (!edit_mode_is_armed()) return;
	widget_t *w = (widget_t *)lv_event_get_user_data(e);
	if (!w) return;
	load_menu_screen_for_widget(w);
}

/** Register touch events on all widgets so the MENU button always appears
 *  on short tap, and long-press opens the config modal once Edit Mode is
 *  armed.  Without the CLICKABLE flag, toggle/button widgets would consume
 *  events before the screen-wide short-tap handler ever sees them. */
static void _register_widget_long_press(void) {
	for (uint8_t i = 0; i < s_widget_count; i++) {
		widget_t *w = s_widgets[i];
		if (!w || !w->root) continue;

		/* Images and shape panels are decoration — they intentionally pass
		 * touch events through to whatever real widget they cover. They get
		 * their CLICKABLE flag flipped on only while Edit Mode is armed
		 * (handled by edit_mode + dashboard listener — see Phase 2). For now,
		 * they remain unclickable and cannot be inspected. */
		if (w->type == WIDGET_IMAGE || w->type == WIDGET_SHAPE_PANEL ||
		    w->type == WIDGET_LINE) {
			lv_obj_clear_flag(w->root, LV_OBJ_FLAG_CLICKABLE);
			continue;
		}

		lv_obj_add_flag(w->root, LV_OBJ_FLAG_CLICKABLE);

		/* Short-tap → reveal toolbar pills (Menu / Edit Mode) */
		lv_obj_add_event_cb(w->root, screen3_touch_event_cb,
							LV_EVENT_PRESSED, NULL);
		lv_obj_add_event_cb(w->root, screen3_touch_event_cb,
							LV_EVENT_RELEASED, NULL);

		/* Long-press → open config modal. Attached to every clickable widget;
		 * the callback bails when Edit Mode is not armed. */
		lv_obj_add_event_cb(w->root, _widget_long_press_cb,
							LV_EVENT_LONG_PRESSED, w);

		/* Edit Mode select + drag handlers. All three bail in live mode so
		 * they're nearly free; only become active when armed. */
		lv_obj_add_event_cb(w->root, edit_mode_widget_pressed_cb,
							LV_EVENT_PRESSED, w);
		lv_obj_add_event_cb(w->root, edit_mode_widget_pressing_cb,
							LV_EVENT_PRESSING, w);
		lv_obj_add_event_cb(w->root, edit_mode_widget_released_cb,
							LV_EVENT_RELEASED, w);
	}
}

/* ── Layout-level night-mode CAN trigger ────────────────────────────────────
 * If the loaded layout has a `night_mode.signal_name` binding, we subscribe
 * to that signal here. The callback compares live value vs `active_when`
 * threshold and forwards to night_mode_set_active(). Subscription survives
 * for the life of the layout — torn down by signal_registry_reset on next
 * layout load, plus the night_mode subscriber list is cleared by dashboard_init. */

static char    s_night_trig_name[33] = "";
static int16_t s_night_trig_idx      = -1;
static float   s_night_trig_threshold = 1.0f;

static void _night_trigger_signal_cb(float value, bool is_stale, void *user_data) {
	(void)user_data;
	if (is_stale) return;
	night_mode_set_active(value >= s_night_trig_threshold);
}

static void _setup_night_trigger(void) {
	s_night_trig_name[0] = '\0';
	s_night_trig_idx     = -1;
	s_night_trig_threshold = 1.0f;

	if (!layout_manager_get_night_trigger(s_night_trig_name,
	                                      sizeof(s_night_trig_name),
	                                      &s_night_trig_threshold)) {
		return;
	}
	s_night_trig_idx = signal_find_by_name(s_night_trig_name);
	if (s_night_trig_idx < 0) {
		ESP_LOGW("dashboard", "night-mode trigger signal '%s' not found in registry — skipped",
		         s_night_trig_name);
		return;
	}
	signal_subscribe((uint16_t)s_night_trig_idx, _night_trigger_signal_cb, NULL);
	ESP_LOGI("dashboard", "Night-mode trigger subscribed to '%s' (active_when >= %.3f)",
	         s_night_trig_name, (double)s_night_trig_threshold);
}

/* ════════════════════════════════════════════════════════════════════════════
 *  dashboard_init
 * ════════════════════════════════════════════════════════════════════════════
 */
void dashboard_init(lv_obj_t *parent) {
	/* Exit Edit Mode on every reload — armed state never survives a layout
	 * swap (the parent screen is about to be deleted, taking the pill and
	 * banner with it). Idempotent; safe at boot when not armed. */
	edit_mode_exit();

	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));

	font_manager_reset_instances();
	font_manager_init();
	signal_registry_init();
	signal_peaks_start_autosave();
	widget_registry_reset();
	widget_warning_reset();
	widget_indicator_reset();
	/* Drop any night-mode subscribers from the previous layout — the widgets
	 * they pointed at have been destroyed; new layout will subscribe fresh. */
	night_mode_clear_subscribers();

	/* Create (or keep) signal timeout timer — checks every 500 ms */
	if (!s_signal_timeout_timer) {
		s_signal_timeout_timer =
			lv_timer_create(_signal_timeout_cb, 500, NULL);
	}

	esp_err_t err = layout_manager_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "layout_manager_init failed (%s) — using fallback",
				 esp_err_to_name(err));
		_fallback_create_all(parent);
		return;
	}

	char layout_name[LAYOUT_MAX_NAME];
	layout_manager_get_active(layout_name, sizeof(layout_name));
	ESP_LOGI(TAG, "Loading layout '%s'", layout_name);

	err = layout_manager_load(layout_name, parent);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "layout_manager_load('%s') failed (%s)",
				 layout_name, esp_err_to_name(err));
		/* Try loading "default" before using hardcoded fallback */
		if (strcmp(layout_name, "default") != 0) {
			ESP_LOGI(TAG, "Attempting to load 'default' layout as fallback");
			signal_registry_reset();
			widget_registry_reset();
			err = layout_manager_load("default", parent);
			if (err == ESP_OK) {
				layout_manager_set_active("default");
				goto loaded;
			}
			ESP_LOGW(TAG, "'default' also failed — using hardcoded fallback");
		}
		_fallback_create_all(parent);
		return;
	}
loaded:

	widget_registry_snapshot(s_widgets, DASHBOARD_MAX_WIDGETS, &s_widget_count);

	/* Register long-press config on signal-bound widgets */
	_register_widget_long_press();

	/* Stop any existing internal signal timer before (re-)starting */
	signal_internal_stop();
	signal_internal_start();

	/* Subscribe brightness dimmer to its configured signal */
	dimmer_subscribe();

	/* Set up the layout-level night-mode CAN trigger (if any). Must run
	 * AFTER signals are loaded and widget night_mode subscriptions are in
	 * place — that way the first trigger callback flips the state and
	 * re-renders all widgets correctly. */
	_setup_night_trigger();

	/* Lazy-init the virtual touch indev for web-based CONTROL. We defer this
	 * until here (rather than registering at boot in app_main) because
	 * calling lv_indev_drv_register() BEFORE the dashboard widgets exist
	 * somehow corrupts LVGL state — the first lv_obj_create() of any
	 * panel widget then infinite-loops inside lv_obj_get_screen(). By
	 * registering here, widgets are already built and LVGL is stable.
	 * Idempotent — subsequent calls are a no-op. */
	remote_touch_init(lv_disp_get_default());

	/* Rebuild the TWAI hardware acceptance filter from the just-loaded signal
	 * registry. Without this, runtime ECU changes (first-run wizard, Device
	 * Settings ECU picker, web layout save) wouldn't take effect on the bus
	 * until a power reboot — the filter would still hold the previous layout's
	 * CAN IDs and silently drop the new ECU's frames. Safe at boot too: the
	 * very first call transitions from boot-time ACCEPT_ALL to a layout-driven
	 * narrow filter. ~150 ms TWAI restart, called once per layout reload. */
	reconfigure_can_filter();
}

void dashboard_apply_layout_json(lv_obj_t *parent, cJSON *root) {
	if (!root || !parent)
		return;

	/* Exit Edit Mode on layout swap — see dashboard_init for rationale. */
	edit_mode_exit();

	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));
	font_manager_reset_instances();
	widget_registry_reset();
	widget_warning_reset();
	widget_indicator_reset();
	night_mode_clear_subscribers();

	/* Ensure layout manager is init (FS mounted etc) before applying */
	layout_manager_init();

	esp_err_t err = layout_manager_apply_json(root, parent);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "dashboard_apply_layout_json failed (%s)",
				 esp_err_to_name(err));
		_fallback_create_all(parent);
	} else {
		widget_registry_snapshot(s_widgets, DASHBOARD_MAX_WIDGETS,
								 &s_widget_count);
		_register_widget_long_press();
		/* Re-bind layout-level night-mode CAN trigger for the new layout. */
		_setup_night_trigger();
	}

	/* New signal set means new CAN IDs to accept — rebuild the TWAI hardware
	 * filter so the bus actually delivers them. See dashboard_init for the
	 * full rationale. */
	reconfigure_can_filter();
}

esp_err_t dashboard_persist_layout(void) {
	char layout_name[LAYOUT_MAX_NAME];
	esp_err_t err = layout_manager_get_active(layout_name, sizeof(layout_name));
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "dashboard_persist_layout: get_active failed (%s)",
				 esp_err_to_name(err));
		return err;
	}

	err = layout_manager_save(layout_name, s_widgets, s_widget_count);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "dashboard_persist_layout: save '%s' failed (%s)",
				 layout_name, esp_err_to_name(err));
	} else {
		ESP_LOGI(TAG, "Layout '%s' persisted (%d widgets)", layout_name,
				 s_widget_count);
	}
	return err;
}
