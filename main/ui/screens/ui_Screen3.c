/*
 * ui_Screen3.c — Dashboard coordinator.
 *
 * Owns LVGL object arrays and UI-level state shared across modules.
 * All widget rendering and signal dispatch are handled by the widget
 * system and signal registry.
 */

#include "ui/screens/ui_Screen3.h"
#include "can/can_manager.h"
#include "device_id.h"
#include "device_settings.h"
#include "system/rdm_lv_async.h"
#include "esp_log.h"
#include "lvgl.h"
#include "ui/callbacks/ui_callbacks.h"
#include "ui/dashboard.h"
#include "ui/menu/edit_mode.h"
#include "ui/menu/menu_screen.h"
#include "ui/screens/splash_screen.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "layout/layout_manager.h"
#include "layout/layout_switcher.h"
#include "storage/config_store.h"
#include "widgets/widget_bar.h"
#include "widgets/widget_indicator.h"
#include "widgets/widget_panel.h"
#include "widgets/widget_rpm_bar.h"
#include "widgets/signal_sim.h"
#include "widgets/widget_warning.h"
#include <stdio.h>
#include <string.h>

/* ── Global LVGL object arrays ──────────────────────────────────────────── */
lv_obj_t *ui_Label[13] = {NULL};
lv_obj_t *ui_Value[13] = {NULL};
lv_obj_t *ui_Box[8] = {NULL};
lv_obj_t *ui_CustomText[8] = {NULL};
lv_obj_t *config_bars[13] = {NULL};
lv_obj_t *ui_MenuScreen = NULL;
lv_obj_t *keyboard = NULL;
lv_obj_t *rpm_bar_gauge = NULL;
lv_obj_t *rpm_redline_zone = NULL;
lv_timer_t *menu_button_hide_timer = NULL;
/* Dash switcher arrows — declared in ui.h, defined here so they're
 * accessible from the show/hide path and click handlers. Created in
 * the same block as ui_Menu_Button at the bottom of ui_Screen3_screen_init. */
lv_obj_t *ui_Layout_Prev_Button = NULL;
lv_obj_t *ui_Layout_Next_Button = NULL;

int rpm_gauge_max = 7000;
int rpm_redline_value = 6000;
uint8_t current_value_id;

/* ── Coordinator-local state ────────────────────────────────────────────── */
static lv_obj_t *ui_Setup_Menu_Screen = NULL;

/* Cached count of switchable layouts. Computed ONCE per screen build (in
 * ui_Screen3_screen_init, i.e. on every layout reload) — NOT on every tap.
 * layout_switcher_count() reads NVS + stat()s each layout file on LittleFS;
 * calling it from the per-tap chrome reveal blocked the LVGL render task long
 * enough to hitch the live gauges (the "glitch on tap"). The reveal now reads
 * this cache instead. */
static int s_layout_count = 0;

/* How long the chrome (Menu / arrows / Edit pill) stays visible after a
 * background tap. Bumped from 6 s -> 10 s so the user has time to actually
 * find what they need without the menu pulling a vanishing act mid-reach. */
#define CHROME_AUTO_HIDE_MS 10000


/* ═══════════════════════════════════════════════════════════════════════════
 *  Coordinator-level event callbacks
 * ═══════════════════════════════════════════════════════════════════════════
 */

void keyboard_ready_event_cb(lv_event_t *e) {
	lv_obj_t *kb = lv_event_get_target(e);
	lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
}


static void menu_button_hide_timer_cb(lv_timer_t *timer) {
	if (ui_Menu_Button && lv_obj_is_valid(ui_Menu_Button))
		lv_obj_add_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN);
	if (ui_Layout_Prev_Button && lv_obj_is_valid(ui_Layout_Prev_Button))
		lv_obj_add_flag(ui_Layout_Prev_Button, LV_OBJ_FLAG_HIDDEN);
	if (ui_Layout_Next_Button && lv_obj_is_valid(ui_Layout_Next_Button))
		lv_obj_add_flag(ui_Layout_Next_Button, LV_OBJ_FLAG_HIDDEN);
	/* Hide the Edit Mode pill in lockstep — no-op when armed (pinned). */
	edit_mode_hide_pill();
	if (menu_button_hide_timer) {
		lv_timer_del(menu_button_hide_timer);
		menu_button_hide_timer = NULL;
	}
}

/* Reveal all chrome (Menu / layout arrows / Edit pill) together and (re)arm
 * the auto-hide timer. Deferred via lv_async_call from the tap handler so it
 * runs after touch-event dispatch, not mid-event. Idempotent: a re-tap while
 * the chrome is already up just resets the hide countdown.
 *
 * (This used to be a 4-step cascade — one object per refresh tick — to dodge a
 * multi-rect RGB-LCD tear. With the vsync-gated flush now in place that
 * workaround is unnecessary, and the stagger itself read as a glitch.) */
/* Fade a chrome object in from transparent instead of a hard pop. The dash's
 * flush isn't vsync-synced, so a sharp reveal over the live gauges shows a
 * tear stripe; spreading the repaint across a ~150 ms fade hides it. No-op if
 * already shown so a re-tap doesn't re-trigger the fade. */
static void _chrome_fade_in(lv_obj_t *o) {
	if (!o || !lv_obj_is_valid(o)) return;
	if (!lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return;
	lv_obj_set_style_opa(o, LV_OPA_TRANSP, 0);
	lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
	lv_obj_fade_in(o, 150, 0);
}

static void _chrome_show_cb(void *arg) {
	(void)arg;
	if (edit_mode_is_armed()) return;

	_chrome_fade_in(ui_Menu_Button);

	if (s_layout_count >= 2) {   /* cached — no filesystem on the tap path */
		_chrome_fade_in(ui_Layout_Next_Button);
		_chrome_fade_in(ui_Layout_Prev_Button);
	}

	edit_mode_show_pill();

	if (menu_button_hide_timer)
		lv_timer_del(menu_button_hide_timer);
	menu_button_hide_timer =
		lv_timer_create(menu_button_hide_timer_cb, CHROME_AUTO_HIDE_MS, NULL);
	lv_timer_set_repeat_count(menu_button_hide_timer, 1);
}

/* Dashboard-background tap handler.
 *
 * Listens for LV_EVENT_SHORT_CLICKED on ui_Screen3 — LVGL's built-in
 * "quick tap without drag" event. This replaces the old PRESSED/RELEASED
 * dance that measured duration manually against lv_tick_get(): that
 * approach dropped events whenever the GT911 controller emitted a
 * release at slightly different coordinates than the press, or whenever
 * an unrelated frame jitter pushed the measured duration past 300 ms.
 *
 * SHORT_CLICKED already enforces "released within click-time window
 * (~400 ms by default) AND drag distance under scroll threshold", which
 * matches the human intent for a tap. Long-presses on widgets continue
 * to fire LV_EVENT_LONG_PRESSED on the widget itself (different event
 * path) — preset-change long-presses are unaffected.
 *
 * Widgets are LV_OBJ_FLAG_CLICKABLE, so events that land on a widget
 * are absorbed and never reach the screen. This handler only fires on
 * empty-background taps — exactly what we want. */
void screen3_touch_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
	if (edit_mode_is_armed()) return;
	/* Defer the reveal to after touch-event dispatch. */
	lv_async_call(_chrome_show_cb, NULL);
}

static void setup_menu_close_btn_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	lv_obj_t *scr = ui_Setup_Menu_Screen;
	if (ui_Screen3 && lv_obj_is_valid(ui_Screen3)) {
		lv_scr_load(ui_Screen3);
		if (scr && lv_obj_is_valid(scr)) {
			rdm_obj_del_async(scr);
			ui_Setup_Menu_Screen = NULL;
		}
	}
}

static void menu_device_settings_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	/* Close menu screen, then open device settings */
	lv_obj_t *scr = ui_Setup_Menu_Screen;
	device_settings_with_return_screen(ui_Screen3);
	if (scr && lv_obj_is_valid(scr)) {
		/* Canonical crash-safe deferred delete: raw lv_obj_del_async can't be
		 * cancelled, so if anything else frees this screen first (layout reload,
		 * wizard teardown) the queued delete double-frees. Mirrors line 171. */
		rdm_obj_del_async(scr);
		ui_Setup_Menu_Screen = NULL;
	}
}

/* ── Deferred layout reload (called via lv_async_call after menu closes) ── */

/* True while a layout cross-fade is in flight. Blocks a second switch from
 * starting an overlapping screen-load animation (which could delete a screen
 * the first transition is still using). Cleared by a one-shot timer just after
 * the fade completes. */
static bool s_layout_switching = false;
static void _layout_switch_done_cb(lv_timer_t *t) {
	s_layout_switching = false;
	lv_timer_del(t);
}

static void _deferred_layout_reload(void *arg) {
	(void)arg;
	ui_Screen3_screen_init();
	if (ui_Screen3) {
		/* Cross-fade to the new layout instead of an instant swap. lv_scr_load
		 * renders the whole new screen in ONE flush, which tears hard on the
		 * dash's un-vsync-synced display (this is the "glitch on layout change"
		 * — the only full-screen redraw in normal use). The ~260 ms fade spreads
		 * that redraw across frames so the swap reads as a smooth transition.
		 * auto_del=true frees the outgoing screen when the animation finishes. */
		lv_scr_load_anim(ui_Screen3, LV_SCR_LOAD_ANIM_FADE_ON, 260, 0, true);
	}
	lv_timer_t *done = lv_timer_create(_layout_switch_done_cb, 320, NULL);
	lv_timer_set_repeat_count(done, 1);
}

/* ── Layout dropdown change callback ── */
static void _menu_layout_changed_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
	lv_obj_t *dd = lv_event_get_target(e);
	char name[LAYOUT_MAX_NAME];
	lv_dropdown_get_selected_str(dd, name, sizeof(name));

	layout_manager_set_active(name);
	ui_Setup_Menu_Screen = NULL;

	/* Defer full screen reload — _deferred_layout_reload will delete
	 * the current screen (menu) and create a fresh dashboard. */
	lv_async_call(_deferred_layout_reload, NULL);
}

/* ── Toast label (auto-delete after delay) ── */
static void _toast_timer_cb(lv_timer_t *t) {
	lv_obj_t *lbl = (lv_obj_t *)t->user_data;
	if (lbl && lv_obj_is_valid(lbl))
		lv_obj_del(lbl);
}

/* ── Splash dropdown change callback ── */
static void _menu_splash_changed_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
	lv_obj_t *dd = lv_event_get_target(e);

	const char *msg;
	if (lv_dropdown_get_selected(dd) == 0) {
		/* Option 0 = "Disable Splash" — boot straight to the dashboard. */
		config_store_save_splash_enabled(false);
		msg = LV_SYMBOL_OK " Splash disabled";
	} else {
		char name[LAYOUT_MAX_NAME];
		lv_dropdown_get_selected_str(dd, name, sizeof(name));
		config_store_save_splash_enabled(true);
		layout_manager_set_active_splash(name);
		msg = LV_SYMBOL_OK " Splash updated";
	}

	/* Show brief toast confirmation */
	lv_obj_t *scr = lv_scr_act();
	lv_obj_t *toast = lv_label_create(scr);
	lv_label_set_text(toast, msg);
	lv_obj_set_style_text_color(toast, THEME_COLOR_ACCENT_TEAL, 0);
	lv_obj_set_style_text_font(toast, THEME_FONT_SMALL, 0);
	lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -12);
	lv_timer_t *tt = lv_timer_create(_toast_timer_cb, 2000, toast);
	lv_timer_set_repeat_count(tt, 1);
}

/** Style a dropdown to match the device-settings dark input look. */
static void _style_dropdown(lv_obj_t *dd) {
	lv_obj_set_style_bg_color(dd, THEME_COLOR_INPUT_BG, 0);
	lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(dd, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(dd, THEME_FONT_SMALL, 0);
	lv_obj_set_style_border_color(dd, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(dd, 1, 0);
	lv_obj_set_style_radius(dd, THEME_RADIUS_NORMAL, 0);
	lv_obj_set_style_pad_all(dd, 4, 0);
	lv_obj_set_style_text_color(dd, THEME_COLOR_TEXT_MUTED, LV_PART_INDICATOR);
}

/* Arrow click → resolve next/prev pinned layout, swap to it. Uses the
 * existing _deferred_layout_reload so the screen rebuild path is shared
 * with the Layout dropdown in the menu. */
static void _layout_arrow_step(int direction) {
	if (s_layout_switching) return;   /* ignore taps while a fade is in flight */
	char active[LAYOUT_MAX_NAME] = {0};
	layout_manager_get_active(active, sizeof(active));
	char next[LAYOUT_MAX_NAME];
	bool ok = (direction > 0)
		? layout_switcher_get_next(active, next, sizeof(next))
		: layout_switcher_get_prev(active, next, sizeof(next));
	if (!ok) return;
	if (strcmp(next, active) == 0) return;
	s_layout_switching = true;
	layout_manager_set_active(next);
	lv_async_call(_deferred_layout_reload, NULL);
}
static void _layout_prev_clicked_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
	_layout_arrow_step(-1);
}
static void _layout_next_clicked_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
	_layout_arrow_step(+1);
}

static void menu_button_clicked_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	if (ui_Menu_Button && lv_obj_is_valid(ui_Menu_Button))
		lv_obj_add_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN);
	/* Tear down the switcher arrows alongside so they don't linger when
	 * the user enters the menu. */
	if (ui_Layout_Prev_Button && lv_obj_is_valid(ui_Layout_Prev_Button))
		lv_obj_add_flag(ui_Layout_Prev_Button, LV_OBJ_FLAG_HIDDEN);
	if (ui_Layout_Next_Button && lv_obj_is_valid(ui_Layout_Next_Button))
		lv_obj_add_flag(ui_Layout_Next_Button, LV_OBJ_FLAG_HIDDEN);
	/* Hide the Edit Mode pill in lockstep — the pills appear together on
	 * a dashboard short-tap and should disappear together when the user
	 * commits to opening the menu. Otherwise the orange/grey pill remains
	 * floating after returning from the menu. No-op in armed mode (the
	 * Menu button is hidden anyway then, so we never get here). */
	edit_mode_hide_pill();
	if (menu_button_hide_timer) {
		lv_timer_del(menu_button_hide_timer);
		menu_button_hide_timer = NULL;
	}

	/* ── Full-screen backdrop ── */
	ui_Setup_Menu_Screen = lv_obj_create(NULL);
	lv_obj_clear_flag(ui_Setup_Menu_Screen, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(ui_Setup_Menu_Screen, THEME_COLOR_BG, 0);
	lv_obj_set_style_bg_opa(ui_Setup_Menu_Screen, LV_OPA_COVER, 0);

	/* ── Main container (matches device_settings style) ── */
	lv_obj_t *container = lv_obj_create(ui_Setup_Menu_Screen);
	lv_obj_set_size(container, 340, 380);
	lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_color(container, THEME_COLOR_SURFACE, 0);
	lv_obj_set_style_bg_opa(container, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(container, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(container, 1, 0);
	lv_obj_set_style_radius(container, THEME_RADIUS_NORMAL, 0);
	lv_obj_set_style_pad_all(container, 0, 0);
	lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

	/* ── Header bar ── */
	lv_obj_t *header = lv_obj_create(container);
	lv_obj_set_size(header, lv_pct(100), 44);
	lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(header, THEME_COLOR_SURFACE, 0);
	lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(header, 0, 0);
	lv_obj_set_style_border_width(header, 1, 0);
	lv_obj_set_style_border_color(header, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *title = lv_label_create(header);
	lv_label_set_text(title, "Menu");
	lv_obj_align(title, LV_ALIGN_LEFT_MID, 15, 0);
	lv_obj_set_style_text_font(title, THEME_FONT_MEDIUM, 0);
	lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

	/* Close button in header */
	lv_obj_t *close_btn = lv_btn_create(header);
	lv_obj_set_size(close_btn, 60, 28);
	lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -10, 0);
	lv_obj_set_style_bg_color(close_btn, THEME_COLOR_SECTION_BG, 0);
	lv_obj_set_style_bg_opa(close_btn, LV_OPA_80, LV_STATE_PRESSED);
	lv_obj_set_style_radius(close_btn, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_border_width(close_btn, 1, 0);
	lv_obj_set_style_border_color(close_btn, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_shadow_width(close_btn, 0, 0);
	lv_obj_t *close_lbl = lv_label_create(close_btn);
	lv_label_set_text(close_lbl, "Close");
	lv_obj_center(close_lbl);
	lv_obj_set_style_text_font(close_lbl, THEME_FONT_SMALL, 0);
	lv_obj_set_style_text_color(close_lbl, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_add_event_cb(close_btn, setup_menu_close_btn_cb,
						LV_EVENT_CLICKED, NULL);

	/* ── Content area ── */
	lv_obj_t *content = lv_obj_create(container);
	lv_obj_set_size(content, lv_pct(100), 328);
	lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 48);
	lv_obj_set_style_bg_opa(content, 0, 0);
	lv_obj_set_style_border_width(content, 0, 0);
	lv_obj_set_style_pad_all(content, 14, 0);
	lv_obj_set_style_pad_row(content, 8, 0);
	lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);

	/* ── Layout section card ── */
	lv_obj_t *layout_card = lv_obj_create(content);
	lv_obj_set_size(layout_card, 304, 100);
	lv_obj_align(layout_card, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(layout_card, THEME_COLOR_SECTION_BG, 0);
	lv_obj_set_style_bg_opa(layout_card, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(layout_card, THEME_RADIUS_NORMAL, 0);
	lv_obj_set_style_border_color(layout_card, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(layout_card, 1, 0);
	lv_obj_set_style_pad_all(layout_card, 12, 0);
	lv_obj_clear_flag(layout_card, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *layout_title = lv_label_create(layout_card);
	lv_label_set_text(layout_title, "LAYOUT");
	lv_obj_align(layout_title, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_set_style_text_font(layout_title, THEME_FONT_TINY, 0);
	lv_obj_set_style_text_color(layout_title, THEME_COLOR_ACCENT_BLUE, 0);
	lv_obj_set_style_text_letter_space(layout_title, 1, 0);

	lv_obj_t *layout_sublbl = lv_label_create(layout_card);
	lv_label_set_text(layout_sublbl, "Active dashboard");
	lv_obj_align(layout_sublbl, LV_ALIGN_TOP_LEFT, 0, 18);
	lv_obj_set_style_text_font(layout_sublbl, THEME_FONT_SMALL, 0);
	lv_obj_set_style_text_color(layout_sublbl, THEME_COLOR_TEXT_MUTED, 0);

	lv_obj_t *layout_dd = lv_dropdown_create(layout_card);
	lv_obj_set_size(layout_dd, 278, 32);
	lv_obj_align(layout_dd, LV_ALIGN_TOP_LEFT, 0, 40);
	_style_dropdown(layout_dd);

	/* Populate layout dropdown */
	char names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
	int count = layout_manager_list(names, LAYOUT_MAX_COUNT);
	char active[LAYOUT_MAX_NAME];
	layout_manager_get_active(active, sizeof(active));

	char options[640] = "";
	int sel_idx = 0;
	int opt_count = 0;
	size_t pos = 0;
	for (int i = 0; i < count; i++) {
		if (names[i][0] == '_') continue;
		size_t nlen = strlen(names[i]);
		if (pos + nlen + 2 > sizeof(options)) break;
		if (opt_count > 0) options[pos++] = '\n';
		memcpy(&options[pos], names[i], nlen);
		pos += nlen;
		options[pos] = '\0';
		if (strcmp(names[i], active) == 0) sel_idx = opt_count;
		opt_count++;
	}
	lv_dropdown_set_options(layout_dd, options);
	lv_dropdown_set_selected(layout_dd, sel_idx);
	lv_obj_add_event_cb(layout_dd, _menu_layout_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	/* ── Splash section card ── */
	lv_obj_t *splash_card = lv_obj_create(content);
	lv_obj_set_size(splash_card, 304, 100);
	lv_obj_align(splash_card, LV_ALIGN_TOP_MID, 0, 108);
	lv_obj_set_style_bg_color(splash_card, THEME_COLOR_SECTION_BG, 0);
	lv_obj_set_style_bg_opa(splash_card, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(splash_card, THEME_RADIUS_NORMAL, 0);
	lv_obj_set_style_border_color(splash_card, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(splash_card, 1, 0);
	lv_obj_set_style_pad_all(splash_card, 12, 0);
	lv_obj_clear_flag(splash_card, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *splash_title = lv_label_create(splash_card);
	lv_label_set_text(splash_title, "SPLASH SCREEN");
	lv_obj_align(splash_title, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_set_style_text_font(splash_title, THEME_FONT_TINY, 0);
	lv_obj_set_style_text_color(splash_title, THEME_COLOR_ACCENT_TEAL, 0);
	lv_obj_set_style_text_letter_space(splash_title, 1, 0);

	lv_obj_t *splash_sublbl = lv_label_create(splash_card);
	lv_label_set_text(splash_sublbl, "Shown on boot");
	lv_obj_align(splash_sublbl, LV_ALIGN_TOP_LEFT, 0, 18);
	lv_obj_set_style_text_font(splash_sublbl, THEME_FONT_SMALL, 0);
	lv_obj_set_style_text_color(splash_sublbl, THEME_COLOR_TEXT_MUTED, 0);

	lv_obj_t *splash_dd = lv_dropdown_create(splash_card);
	lv_obj_set_size(splash_dd, 278, 32);
	lv_obj_align(splash_dd, LV_ALIGN_TOP_LEFT, 0, 40);
	_style_dropdown(splash_dd);

	/* Populate splash dropdown */
	char splash_names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
	int splash_count = layout_manager_list_splash(splash_names,
												  LAYOUT_MAX_COUNT);
	char active_splash[LAYOUT_MAX_NAME];
	layout_manager_get_active_splash(active_splash, sizeof(active_splash));

	bool splash_on = true;
	config_store_load_splash_enabled(&splash_on);

	/* Option 0 is always "Disable Splash"; the saved splash layouts follow. */
	char splash_opts[640] = "Disable Splash";
	size_t spos = strlen(splash_opts);
	int splash_sel = 0;            /* default selection = Disable Splash */
	int splash_opt_count = 1;      /* index 0 taken by Disable Splash     */
	for (int i = 0; i < splash_count; i++) {
		size_t slen = strlen(splash_names[i]);
		if (spos + slen + 2 > sizeof(splash_opts)) break;
		splash_opts[spos++] = '\n';
		memcpy(&splash_opts[spos], splash_names[i], slen);
		spos += slen;
		splash_opts[spos] = '\0';
		if (splash_on && strcmp(splash_names[i], active_splash) == 0)
			splash_sel = splash_opt_count;
		splash_opt_count++;
	}
	/* Splash enabled but the active name wasn't in the list → fall back to the
	 * first real splash rather than leaving "Disable Splash" selected. */
	if (splash_on && splash_sel == 0 && splash_opt_count > 1)
		splash_sel = 1;

	lv_dropdown_set_options(splash_dd, splash_opts);
	lv_dropdown_set_selected(splash_dd, splash_sel);
	lv_obj_add_event_cb(splash_dd, _menu_splash_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	/* ── Bottom button row ── */
	lv_obj_t *btn_row = lv_obj_create(content);
	lv_obj_set_size(btn_row, 304, 48);
	lv_obj_align(btn_row, LV_ALIGN_TOP_MID, 0, 218);
	lv_obj_set_style_bg_opa(btn_row, 0, 0);
	lv_obj_set_style_border_width(btn_row, 0, 0);
	lv_obj_set_style_pad_all(btn_row, 0, 0);
	lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

	/* Device Settings button — blue accent */
	lv_obj_t *ds = lv_btn_create(btn_row);
	lv_obj_set_size(ds, 304, 40);
	lv_obj_align(ds, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_color(ds, THEME_COLOR_ACCENT_BLUE, 0);
	lv_obj_set_style_bg_color(ds, THEME_COLOR_ACCENT_BLUE_PRESSED,
							  LV_STATE_PRESSED);
	lv_obj_set_style_radius(ds, THEME_RADIUS_NORMAL, 0);
	lv_obj_set_style_shadow_width(ds, 0, 0);
	lv_obj_t *ds_lbl = lv_label_create(ds);
	lv_label_set_text(ds_lbl, LV_SYMBOL_SETTINGS "  Device Settings");
	lv_obj_center(ds_lbl);
	lv_obj_set_style_text_font(ds_lbl, THEME_FONT_SMALL, 0);
	lv_obj_set_style_text_color(ds_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
	lv_obj_add_event_cb(ds, menu_device_settings_cb, LV_EVENT_CLICKED, NULL);

	lv_scr_load(ui_Setup_Menu_Screen);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  BUS SILENT Overlay
 *  ─────────────────
 *  Small top-right badge that appears when no CAN frames have arrived for
 *  more than CAN_SILENT_MS. Hidden when traffic is flowing. Polled once per
 *  second by an LVGL timer — tiny overhead, and we compare against the
 *  cumulative frame counter exposed by can_manager.
 * ═══════════════════════════════════════════════════════════════════════════ */
#define CAN_SILENT_MS 5000

static lv_obj_t *s_bus_silent_badge = NULL;
static lv_timer_t *s_bus_silent_timer = NULL;
static uint32_t s_last_rx_count = 0;
static uint32_t s_last_rx_change_ms = 0;

static void _bus_silent_tick_cb(lv_timer_t *t) {
    (void)t;
    if (!s_bus_silent_badge || !lv_obj_is_valid(s_bus_silent_badge)) return;

    uint32_t now_ms = lv_tick_get();

    /* While the signal simulator is active, hide the NO-CAN badge and keep
     * the silence timer seeded forward. Rationale: demos in a parked car
     * should look clean, and the simulator is producing visible motion on
     * the gauges anyway — a flashing "NO CAN BUS" warning alongside fake
     * data is noise. When sim stops we fall back to the real 5s silence
     * heuristic, starting from "now" so the badge doesn't snap on
     * instantly from the pre-sim timestamp. */
    if (signal_sim_is_active()) {
        s_last_rx_count = can_get_rx_frame_count();
        s_last_rx_change_ms = now_ms;
        if (!lv_obj_has_flag(s_bus_silent_badge, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_bus_silent_badge, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    uint32_t cnt = can_get_rx_frame_count();
    if (cnt != s_last_rx_count) {
        s_last_rx_count = cnt;
        s_last_rx_change_ms = now_ms;
    }
    /* On first tick, seed timestamp so we don't immediately show silence */
    if (s_last_rx_change_ms == 0) s_last_rx_change_ms = now_ms;

    uint32_t silent_for = now_ms - s_last_rx_change_ms;
    bool should_show = (silent_for > CAN_SILENT_MS);

    bool currently_visible = !lv_obj_has_flag(s_bus_silent_badge, LV_OBJ_FLAG_HIDDEN);
    if (should_show && !currently_visible) {
        lv_obj_clear_flag(s_bus_silent_badge, LV_OBJ_FLAG_HIDDEN);
        /* Foreground every time it transitions visible — guards against any
         * widget added after this point ending up on top of the warning. */
        lv_obj_move_foreground(s_bus_silent_badge);
    } else if (!should_show && currently_visible) {
        lv_obj_add_flag(s_bus_silent_badge, LV_OBJ_FLAG_HIDDEN);
    }
}

static void _ui_screen3_init_bus_silent_overlay(void) {
    /* Centre badge — positioned over the dashboard but non-interactive */
    s_bus_silent_badge = lv_obj_create(ui_Screen3);
    lv_obj_remove_style_all(s_bus_silent_badge);
    lv_obj_set_size(s_bus_silent_badge, 160, 32);
    lv_obj_align(s_bus_silent_badge, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_bus_silent_badge, lv_color_hex(0x7F1D1D), 0);
    lv_obj_set_style_bg_opa(s_bus_silent_badge, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_bus_silent_badge, lv_color_hex(0xF87171), 0);
    lv_obj_set_style_border_width(s_bus_silent_badge, 1, 0);
    lv_obj_set_style_radius(s_bus_silent_badge, 6, 0);
    lv_obj_add_flag(s_bus_silent_badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_bus_silent_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_bus_silent_badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(s_bus_silent_badge);
    lv_label_set_text(lbl, LV_SYMBOL_WARNING "  NO CAN BUS");
    lv_obj_center(lbl);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFECACA), 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);

    /* Poll once per second — seeded so initial "no data yet" doesn't trigger */
    s_last_rx_count = can_get_rx_frame_count();
    s_last_rx_change_ms = lv_tick_get();
    if (s_bus_silent_timer) { lv_timer_del(s_bus_silent_timer); s_bus_silent_timer = NULL; }
    s_bus_silent_timer = lv_timer_create(_bus_silent_tick_cb, 1000, NULL);
}

void ui_Screen3_refresh_overlays(void) {
    /* Bring the BUS SILENT badge to the front of its parent so any widgets
     * added by a layout reapply don't draw over it. The badge starts
     * hidden — foregrounding while hidden is harmless. */
    if (s_bus_silent_badge && lv_obj_is_valid(s_bus_silent_badge))
        lv_obj_move_foreground(s_bus_silent_badge);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Screen initialisation
 * ═══════════════════════════════════════════════════════════════════════════
 */
void ui_Screen3_screen_init(void) {
	init_styles();
	init_common_style();

	ui_Screen3 = lv_obj_create(NULL);
	lv_obj_clear_flag(ui_Screen3, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(ui_Screen3, THEME_COLOR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Screen3, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	/* Tap-on-background -> reveal chrome via LVGL's built-in short-click
	 * detection. Drag-tolerant, debounced, fires only on real taps. */
	lv_obj_add_event_cb(ui_Screen3, screen3_touch_event_cb,
						LV_EVENT_SHORT_CLICKED, NULL);
	/* Edit Mode: empty-area press deselects whatever widget is currently
	 * selected. Widgets intercept presses (CLICKABLE), so this only fires on
	 * the dashboard background. No-op in live mode. */
	lv_obj_add_event_cb(ui_Screen3, edit_mode_screen_pressed_cb,
						LV_EVENT_PRESSED, NULL);

	static bool timers_created = false;
	if (!timers_created) {
		lv_timer_create(check_rpm_color_update, 500, NULL);
		lv_timer_create(check_warning_timeouts, 50, NULL);
		indicator_animation_timer =
			lv_timer_create(indicator_animation_timer_cb, 350, NULL);
		lv_timer_pause(indicator_animation_timer);
		timers_created = true;
	}

	/* Clear stale object pointers before re-creation */
	for (int i = 0; i < 13; i++) {
		ui_Label[i] = ui_Value[i] = NULL;
		if (i < 8) {
			ui_Box[i] = ui_CustomText[i] = NULL;
		}
	}
	rpm_bar_gauge = NULL;
	ui_RPM_Value = NULL;
	ui_RPM_Label = NULL;
	ui_Panel9 = NULL;
	ui_Bar_1 = NULL;
	ui_Bar_2 = NULL;
	/* Clear stale static pointers inside widget_rpm_bar module */
	widget_rpm_bar_clear_stale_pointers();

	/* Initialise widget layer via layout manager (loads from LittleFS JSON,
	 * falls back to direct widget_X_create() if the file is unavailable). */
	dashboard_init(ui_Screen3);

	/* Cache the switchable-layout count once, here — the chrome reveal reads
	 * the cache so a tap never touches the filesystem (see s_layout_count). */
	s_layout_count = layout_switcher_count();

	/* Menu button — floating top-right pill, blue accent with settings
	 * icon. Replaces the former centre-dash glassmorphism style; the
	 * top-right corner keeps it out of the driver's sight-line while
	 * the icon + label read clearly against any layout behind it. */
	ui_Menu_Button = lv_btn_create(ui_Screen3);
	lv_obj_set_size(ui_Menu_Button, 100, 36);
	lv_obj_align(ui_Menu_Button, LV_ALIGN_TOP_RIGHT, -12, 12);
	/* Expand the touch target 20 px beyond the visual bounds. The GT911 jitters
	 * the contact point between press and release (see notes on
	 * screen3_touch_event_cb); on a small corner target that drift can push the
	 * finger off the button mid-press, firing LV_EVENT_PRESS_LOST and clearing
	 * the active object — so neither CLICKED nor SHORT_CLICKED fires on release
	 * and the tap is silently dropped ("the menu sometimes didn't open"). The
	 * ext click area keeps the press tracked through that jitter. No steal risk:
	 * it grows toward the screen corner, and the only neighbour (Layout_Next) is
	 * created later so it sits on top and wins the seam hit-test. Hidden objects
	 * aren't hit-tested, so this is inert while the chrome is hidden — widget
	 * long-press underneath is unaffected. */
	lv_obj_set_ext_click_area(ui_Menu_Button, 20);
	lv_obj_add_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_bg_color(ui_Menu_Button, THEME_COLOR_ACCENT_BLUE, 0);
	lv_obj_set_style_bg_color(ui_Menu_Button, THEME_COLOR_ACCENT_BLUE_PRESSED,
							  LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(ui_Menu_Button, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(ui_Menu_Button, 0, 0);
	lv_obj_set_style_radius(ui_Menu_Button, THEME_RADIUS_NORMAL, 0);
	/* No drop shadow: its semi-transparent halo sits over the live widgets
	 * (RPM bar) and re-composites every time they update — that shimmer is
	 * the "small glitch" on reveal. Flat reads cleaner anyway. */
	lv_obj_set_style_shadow_width(ui_Menu_Button, 0, 0);
	lv_obj_t *ml = lv_label_create(ui_Menu_Button);
	lv_label_set_text(ml, LV_SYMBOL_SETTINGS "  Menu");
	lv_obj_set_style_text_color(ml, THEME_COLOR_TEXT_ON_ACCENT, 0);
	lv_obj_set_style_text_font(ml, THEME_FONT_SMALL, 0);
	lv_obj_center(ml);
	lv_obj_add_event_cb(ui_Menu_Button, menu_button_clicked_cb,
						LV_EVENT_CLICKED, NULL);

	/* Layout switcher arrows — flank the Menu button when the user taps
	 * the dash. Same blue accent so they read as a related cluster, but
	 * narrower (44 px) since they're symbol-only. Both hidden by default
	 * and tied to the same show/hide timer as the Menu button — the
	 * screen3_touch_event_cb only unhides them when the switcher cycle
	 * has ≥2 entries (otherwise they wouldn't do anything useful). */
	const int arrow_w = 44, arrow_h = 36;
	/* Geometry: Menu_Button is 100 wide aligned top-right at -12. The
	 * arrows sit to its LEFT (so the menu pill stays in the corner).
	 * Prev = leftmost, Next = between Prev and Menu. 6px gaps. */
	ui_Layout_Next_Button = lv_btn_create(ui_Screen3);
	lv_obj_set_size(ui_Layout_Next_Button, arrow_w, arrow_h);
	lv_obj_align(ui_Layout_Next_Button, LV_ALIGN_TOP_RIGHT,
				 -(12 + 100 + 6), 12);
	lv_obj_add_flag(ui_Layout_Next_Button, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_bg_color(ui_Layout_Next_Button, THEME_COLOR_ACCENT_BLUE, 0);
	lv_obj_set_style_bg_color(ui_Layout_Next_Button,
							  THEME_COLOR_ACCENT_BLUE_PRESSED, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(ui_Layout_Next_Button, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(ui_Layout_Next_Button, 0, 0);
	lv_obj_set_style_radius(ui_Layout_Next_Button, THEME_RADIUS_NORMAL, 0);
	lv_obj_set_style_shadow_width(ui_Layout_Next_Button, 0, 0);
	lv_obj_t *nl = lv_label_create(ui_Layout_Next_Button);
	lv_label_set_text(nl, LV_SYMBOL_RIGHT);
	lv_obj_set_style_text_color(nl, THEME_COLOR_TEXT_ON_ACCENT, 0);
	lv_obj_set_style_text_font(nl, THEME_FONT_SMALL, 0);
	lv_obj_center(nl);
	lv_obj_add_event_cb(ui_Layout_Next_Button, _layout_next_clicked_cb,
						LV_EVENT_CLICKED, NULL);

	ui_Layout_Prev_Button = lv_btn_create(ui_Screen3);
	lv_obj_set_size(ui_Layout_Prev_Button, arrow_w, arrow_h);
	lv_obj_align(ui_Layout_Prev_Button, LV_ALIGN_TOP_RIGHT,
				 -(12 + 100 + 6 + arrow_w + 6), 12);
	lv_obj_add_flag(ui_Layout_Prev_Button, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_bg_color(ui_Layout_Prev_Button, THEME_COLOR_ACCENT_BLUE, 0);
	lv_obj_set_style_bg_color(ui_Layout_Prev_Button,
							  THEME_COLOR_ACCENT_BLUE_PRESSED, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(ui_Layout_Prev_Button, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(ui_Layout_Prev_Button, 0, 0);
	lv_obj_set_style_radius(ui_Layout_Prev_Button, THEME_RADIUS_NORMAL, 0);
	lv_obj_set_style_shadow_width(ui_Layout_Prev_Button, 0, 0);
	lv_obj_t *pl = lv_label_create(ui_Layout_Prev_Button);
	lv_label_set_text(pl, LV_SYMBOL_LEFT);
	lv_obj_set_style_text_color(pl, THEME_COLOR_TEXT_ON_ACCENT, 0);
	lv_obj_set_style_text_font(pl, THEME_FONT_SMALL, 0);
	lv_obj_center(pl);
	lv_obj_add_event_cb(ui_Layout_Prev_Button, _layout_prev_clicked_cb,
						LV_EVENT_CLICKED, NULL);

	/* Edit Mode pill — DISABLED for now. On-device edit mode is incomplete and
	 * deferred to a later update, so the entry button is not created. Skipping
	 * creation leaves s_pill NULL; edit_mode_show_pill()/hide_pill() both guard
	 * on that, so the reveal-on-tap path is a safe no-op. Restore this call to
	 * bring the button back. */
	/* edit_mode_create_pill(ui_Screen3); */

	/* BUS SILENT overlay — shows when no CAN frames have been received for
	   >CAN_SILENT_MS milliseconds. Small top-right badge (doesn't block widgets),
	   tinted red for visibility. Invisible when CAN traffic is flowing. */
	_ui_screen3_init_bus_silent_overlay();
}

void ui_Screen3_preview_layout(cJSON *root) {
	/* Build new layout on a fresh offscreen object so the active screen
	 * stays visible until the swap in _deferred_preview_apply.
	 * The caller is responsible for lv_scr_load + deleting the old screen. */

	/* Clear stale widget pointers BEFORE creating new objects */
	widget_rpm_bar_clear_stale_pointers();
	for (int i = 0; i < 13; i++) {
		ui_Label[i] = ui_Value[i] = NULL;
		if (i < 8) {
			ui_Box[i] = ui_CustomText[i] = NULL;
		}
	}
	rpm_bar_gauge = NULL;
	rpm_redline_zone = NULL;
	ui_RPM_Value = NULL;
	ui_RPM_Label = NULL;
	ui_Panel9 = NULL;
	ui_Bar_1 = NULL;
	ui_Bar_2 = NULL;

	/* Create a new screen (offscreen — not yet loaded) */
	ui_Screen3 = lv_obj_create(NULL);
	lv_obj_clear_flag(ui_Screen3, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(ui_Screen3, THEME_COLOR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Screen3, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	/* Tap-on-background -> reveal chrome (see notes on screen3_touch_event_cb). */
	lv_obj_add_event_cb(ui_Screen3, screen3_touch_event_cb,
						LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_add_event_cb(ui_Screen3, edit_mode_screen_pressed_cb,
						LV_EVENT_PRESSED, NULL);

	dashboard_apply_layout_json(ui_Screen3, root);
}
