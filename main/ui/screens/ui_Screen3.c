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
#include "esp_attr.h"
#include "ui/callbacks/ui_callbacks.h"
#include "ui/dashboard.h"
#include "ui/menu/edit_mode.h"
#include "ui/menu/menu_screen.h"
#include "ui/menu/main_menu.h"
#include "ui/layout_thumbs.h"
#include "ui/kit/ui_kit.h"
#include "storage/data_logger.h"
#include "system/rdm_lv_async.h"
#include "ui/screens/splash_screen.h"
#include "ui/screens/first_run_wizard.h"
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
/* Cached count of switchable layouts. Computed ONCE per screen build (in
 * ui_Screen3_screen_init, i.e. on every layout reload) — NOT on every tap.
 * layout_switcher_count() reads NVS + stat()s each layout file on LittleFS;
 * calling it from the per-tap chrome reveal blocked the LVGL render task long
 * enough to hitch the live gauges (the "glitch on tap"). The reveal now reads
 * this cache instead. */
static int s_layout_count = 0;
static int s_layout_index = -1;   /* where the active layout sits in that cycle */
static char s_active_layout[LAYOUT_MAX_NAME] = "";   /* cached with the cycle */

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


/* ═══════════════════════════════════════════════════════════════════════════
 *  The dock (ADR-0075)
 *  ───────────────────
 *  One bar along the bottom that a tap on the dashboard brings up for
 *  CHROME_AUTO_HIDE_MS: switch layout, set brightness, dim, start or stop
 *  recording, open Setup. It replaced a Menu pill plus two arrow buttons in
 *  the top corner, which opened a small card with two dropdowns.
 *
 *  Opaque on purpose: LVGL stops redrawing at the topmost opaque object, so
 *  gauges updating underneath don't re-composite through it.
 * ═══════════════════════════════════════════════════════════════════════════ */

static lv_obj_t   *s_dock        = NULL;
static lv_obj_t   *s_dock_name   = NULL;
static lv_obj_t   *s_drawer      = NULL;   /* the layout drawer, when open */
static lv_obj_t   *s_dock_slider = NULL;
static lv_obj_t   *s_dock_dim    = NULL;
static lv_obj_t   *s_dock_rec    = NULL;
static lv_timer_t *s_dock_tick   = NULL;

static void _dock_refresh(void) {
	if (!s_dock || !lv_obj_is_valid(s_dock)) return;
	if (s_dock_slider) lv_slider_set_value(s_dock_slider, current_brightness, LV_ANIM_OFF);
	if (s_dock_dim) uk_btn_set_kind(s_dock_dim, display_is_dimmed() ? UK_BTN_ON : UK_BTN_NEUTRAL);
	if (s_dock_rec) {
		if (data_logger_is_active()) {
			uint32_t s = data_logger_get_elapsed_ms() / 1000;
			char t[12];
			if (s >= 3600) snprintf(t, sizeof(t), "%u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60));
			else snprintf(t, sizeof(t), "%02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
			uk_btn_set_text(s_dock_rec, t);
			uk_btn_set_kind(s_dock_rec, UK_BTN_ON);
		} else {
			uk_btn_set_text(s_dock_rec, "Rec");
			uk_btn_set_kind(s_dock_rec, UK_BTN_NEUTRAL);
		}
	}
}

static void _dock_tick_cb(lv_timer_t *t) {
	(void)t;
	_dock_refresh();
}

static void _drawer_close(void);

static void chrome_hide(void) {
	_drawer_close();
	if (s_dock && lv_obj_is_valid(s_dock))
		lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
	if (s_dock_tick) {
		lv_timer_del(s_dock_tick);
		s_dock_tick = NULL;
	}
	/* Hide the Edit Mode pill in lockstep — no-op when armed (pinned). */
	edit_mode_hide_pill();
	if (menu_button_hide_timer) {
		lv_timer_del(menu_button_hide_timer);
		menu_button_hide_timer = NULL;
	}
}

static void menu_button_hide_timer_cb(lv_timer_t *timer) {
	(void)timer;
	chrome_hide();
}

/* (Re)start the auto-hide countdown. Every touch on the dock calls this, so
 * it never vanishes under a finger that is still using it. */
static void _chrome_rearm(void) {
	if (menu_button_hide_timer)
		lv_timer_del(menu_button_hide_timer);
	menu_button_hide_timer = NULL;
	/* The drawer stays until it is closed: someone choosing a layout isn't
	 * idle, and a sheet vanishing mid-scroll would be worse than a dock. */
	if (s_drawer) return;
	menu_button_hide_timer =
		lv_timer_create(menu_button_hide_timer_cb, CHROME_AUTO_HIDE_MS, NULL);
	lv_timer_set_repeat_count(menu_button_hide_timer, 1);
}

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

/* Reveal the dock and (re)arm the auto-hide timer. Deferred via lv_async_call
 * from the tap handler so it runs after touch-event dispatch, not mid-event.
 * Idempotent: a re-tap while it is up just resets the countdown. */
static void _chrome_show_cb(void *arg) {
	(void)arg;
	if (edit_mode_is_armed()) return;

	_dock_refresh();
	_chrome_fade_in(s_dock);
	if (!s_dock_tick) s_dock_tick = lv_timer_create(_dock_tick_cb, 1000, NULL);

	edit_mode_show_pill();
	_chrome_rearm();
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
	if (s_drawer) {
		_drawer_close();
		lv_async_call(_chrome_show_cb, NULL);
		return;
	}
	/* The glass shows the dashboard alone right now (the dock appears after
	 * this): the moment to picture it for the Layouts page. Skipped when
	 * anything is drawn over it. */
	if (lv_scr_act() == ui_Screen3 && s_dock && lv_obj_has_flag(s_dock, LV_OBJ_FLAG_HIDDEN) &&
	    lv_obj_get_child_cnt(lv_layer_top()) == 0 && !first_run_wizard_is_open())
		layout_thumbs_capture_if_due(s_active_layout);
	/* Defer the reveal to after touch-event dispatch. */
	lv_async_call(_chrome_show_cb, NULL);
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

/* Set when a layout is picked from a menu: the dashboard it replaces is not the
 * active screen then, so the load animation's auto-delete won't free it. */
static lv_obj_t *s_stale_dash = NULL;

static void _deferred_layout_reload(void *arg) {
	(void)arg;
	lv_obj_t *stale = s_stale_dash;
	s_stale_dash = NULL;
	ui_Screen3_screen_init();
	if (stale && stale != ui_Screen3 && stale != lv_scr_act() && lv_obj_is_valid(stale))
		rdm_obj_del_async(stale);
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
	_chrome_rearm();
	_layout_arrow_step(-1);
}
static void _layout_next_clicked_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
	_chrome_rearm();
	_layout_arrow_step(+1);
}

static void _dock_setup_cb(lv_event_t *e) {
	(void)e;
	chrome_hide();
	main_menu_open(ui_Screen3);
}

static void _dock_dim_cb(lv_event_t *e) {
	(void)e;
	display_toggle_dim();
	_dock_refresh();
	_chrome_rearm();
}

static void _dock_rec_cb(lv_event_t *e) {
	(void)e;
	if (data_logger_is_active()) data_logger_stop();
	else data_logger_start();
	_dock_refresh();
	_chrome_rearm();
}

static void _dock_slider_cb(lv_event_t *e) {
	lv_obj_t *s = lv_event_get_target(e);
	set_display_brightness(lv_slider_get_value(s));
	_chrome_rearm();
}

static void _dock_deleted_cb(lv_event_t *e) {
	if (lv_event_get_target(e) != s_dock) return;
	if (s_dock_tick) {
		lv_timer_del(s_dock_tick);
		s_dock_tick = NULL;
	}
	s_dock = s_dock_name = s_dock_slider = s_dock_dim = s_dock_rec = NULL;
}

/* ── Layout drawer ────────────────────────────────────────────────────────
 * The name pill opens a sheet along the bottom with every layout as a picture
 * card (the Layouts page's cards), scrolled so the one in use is in view. Tap
 * one to drive with it; tap the dashboard above or the X to put it away. */

/* PSRAM: internal RAM is kept for WiFi at boot (ADR-0076). */
static EXT_RAM_BSS_ATTR char s_drawer_names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
static int  s_drawer_count = 0;

static void _drawer_deleted_cb(lv_event_t *e) {
	if (lv_event_get_target(e) == s_drawer) s_drawer = NULL;
}

/* Deferred: it is usually called from inside the drawer's own click. */
static void _drawer_close(void) {
	if (s_drawer && lv_obj_is_valid(s_drawer)) rdm_obj_del_async(s_drawer);
	s_drawer = NULL;
}

static void _drawer_x_cb(lv_event_t *e) {
	(void)e;
	_drawer_close();
	lv_async_call(_chrome_show_cb, NULL);
}

static void _drawer_pick_cb(lv_event_t *e) {
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	if (idx < 0 || idx >= s_drawer_count) return;
	_drawer_close();
	if (strcmp(s_drawer_names[idx], s_active_layout) == 0) {
		lv_async_call(_chrome_show_cb, NULL);   /* already on it: back to the dock */
		return;
	}
	ui_Screen3_switch_layout(s_drawer_names[idx]);
}

static void _drawer_open_cb(lv_event_t *e) {
	(void)e;
	if (s_drawer || !ui_Screen3) return;
	if (menu_button_hide_timer) {
		lv_timer_del(menu_button_hide_timer);
		menu_button_hide_timer = NULL;
	}
	if (s_dock && lv_obj_is_valid(s_dock)) lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);

	const lv_coord_t H = 276;
	s_drawer = lv_obj_create(ui_Screen3);
	lv_obj_remove_style_all(s_drawer);
	/* Taller than it shows, pushed down, so only the top corners are round. */
	lv_obj_set_size(s_drawer, LV_HOR_RES, H + 16);
	lv_obj_align(s_drawer, LV_ALIGN_BOTTOM_MID, 0, 16);
	lv_obj_set_style_bg_color(s_drawer, THEME_COLOR_SURFACE, 0);
	lv_obj_set_style_bg_opa(s_drawer, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(s_drawer, THEME_COLOR_BORDER_MED, 0);
	lv_obj_set_style_border_width(s_drawer, 1, 0);
	lv_obj_set_style_radius(s_drawer, 16, 0);
	lv_obj_set_style_pad_top(s_drawer, 12, 0);
	lv_obj_set_style_pad_hor(s_drawer, 12, 0);
	lv_obj_set_style_pad_bottom(s_drawer, 16 + 12, 0);
	lv_obj_set_style_pad_row(s_drawer, 10, 0);
	lv_obj_set_flex_flow(s_drawer, LV_FLEX_FLOW_COLUMN);
	lv_obj_add_flag(s_drawer, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_clear_flag(s_drawer, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(s_drawer, _drawer_deleted_cb, LV_EVENT_DELETE, NULL);

	char names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
	int n = layout_manager_list(names, LAYOUT_MAX_COUNT);
	s_drawer_count = 0;
	for (int i = 0; i < n; i++) {
		if (names[i][0] == '_') continue;
		snprintf(s_drawer_names[s_drawer_count++], LAYOUT_MAX_NAME, "%s", names[i]);
	}

	lv_obj_t *head = lv_obj_create(s_drawer);
	lv_obj_remove_style_all(head);
	lv_obj_set_size(head, lv_pct(100), 36);
	lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(head, 12, 0);
	lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	uk_label(head, "Layouts", UK_FONT_HEAD, UK_TONE_TEXT);
	char saved[16];
	snprintf(saved, sizeof(saved), "%d saved", s_drawer_count);
	lv_obj_t *sv = uk_label(head, saved, UK_FONT_SMALL, UK_TONE_MUTED);
	lv_obj_set_flex_grow(sv, 1);
	lv_obj_t *x = uk_btn(head, UK_ICON_CLOSE, NULL, UK_BTN_NEUTRAL, _drawer_x_cb, NULL);
	lv_obj_set_size(x, 40, 36);
	lv_obj_set_style_pad_hor(x, 0, 0);
	lv_obj_set_ext_click_area(x, 10);

	lv_obj_t *row = lv_obj_create(s_drawer);
	lv_obj_remove_style_all(row);
	lv_obj_set_width(row, lv_pct(100));
	lv_obj_set_flex_grow(row, 1);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(row, 10, 0);
	lv_obj_set_scroll_dir(row, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
	lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *in_use = NULL;
	for (int i = 0; i < s_drawer_count; i++) {
		bool on = strcmp(s_drawer_names[i], s_active_layout) == 0;
		lv_obj_t *card = main_menu_layout_card(row, s_drawer_names[i], on,
		                                       _drawer_pick_cb, (void *)(intptr_t)i);
		if (on) in_use = card;
	}
	if (in_use) {
		lv_obj_update_layout(s_drawer);
		lv_obj_scroll_to_view(in_use, LV_ANIM_OFF);
	}

	lv_obj_set_style_opa(s_drawer, LV_OPA_TRANSP, 0);
	lv_obj_fade_in(s_drawer, 150, 0);
}

/* A dock segment: the rounded card-coloured group the controls sit in. */
static lv_obj_t *_dock_seg(lv_obj_t *dock, lv_coord_t grow) {
	lv_obj_t *seg = lv_obj_create(dock);
	lv_obj_remove_style_all(seg);
	lv_obj_set_style_bg_color(seg, THEME_COLOR_PANEL, 0);
	lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(seg, 10, 0);
	lv_obj_set_height(seg, lv_pct(100));
	lv_obj_set_flex_grow(seg, grow);
	lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	return seg;
}

/* An icon-over-label dock button. */
static lv_obj_t *_dock_btn(lv_obj_t *dock, uk_icon_t icon, const char *text, lv_event_cb_t cb) {
	lv_obj_t *b = uk_btn(dock, icon, text, UK_BTN_NEUTRAL, cb, NULL);
	lv_obj_set_size(b, 76, lv_pct(100));
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_hor(b, 0, 0);
	lv_obj_set_style_pad_row(b, 5, 0);
	lv_obj_set_style_radius(b, 10, 0);
	lv_obj_t *l = uk_btn_label(b);
	lv_obj_set_style_text_font(l, uk_font(UK_FONT_SMALL), 0);
	return b;
}

static void _dock_create(lv_obj_t *scr) {
	uk_init();
	s_dock = lv_obj_create(scr);
	lv_obj_remove_style_all(s_dock);
	lv_obj_set_size(s_dock, LV_HOR_RES - 20, 84);
	lv_obj_align(s_dock, LV_ALIGN_BOTTOM_MID, 0, -10);
	lv_obj_set_style_bg_color(s_dock, THEME_COLOR_INPUT_BG, 0);
	lv_obj_set_style_bg_opa(s_dock, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(s_dock, THEME_COLOR_BORDER_MED, 0);
	lv_obj_set_style_border_width(s_dock, 1, 0);
	lv_obj_set_style_radius(s_dock, 16, 0);
	lv_obj_set_style_pad_all(s_dock, 8, 0);
	lv_obj_set_style_pad_column(s_dock, 8, 0);
	lv_obj_set_flex_flow(s_dock, LV_FLEX_FLOW_ROW);
	lv_obj_clear_flag(s_dock, LV_OBJ_FLAG_SCROLLABLE);
	/* Clickable so a tap on the dock's own padding doesn't fall through to
	 * the dashboard (and re-trigger the reveal). */
	lv_obj_add_flag(s_dock, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(s_dock, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(s_dock, _dock_deleted_cb, LV_EVENT_DELETE, NULL);

	/* ── Layout: [◀] [NAME / 3 of 9] [▶] — three pills. The name pill is
	 * as wide as the name needs and opens the layout drawer; the arrows step
	 * through the cycle without opening anything. ── */
	lv_obj_t *lay = lv_obj_create(s_dock);
	lv_obj_remove_style_all(lay);
	lv_obj_set_size(lay, LV_SIZE_CONTENT, lv_pct(100));
	lv_obj_set_flex_flow(lay, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(lay, 6, 0);
	lv_obj_clear_flag(lay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

	ui_Layout_Prev_Button = uk_btn(lay, UK_ICON_LEFT, NULL, UK_BTN_NEUTRAL, _layout_prev_clicked_cb, NULL);
	lv_obj_set_size(ui_Layout_Prev_Button, 50, lv_pct(100));
	lv_obj_set_style_radius(ui_Layout_Prev_Button, 10, 0);

	lv_obj_t *pill = lv_obj_create(lay);
	lv_obj_remove_style_all(pill);
	lv_obj_set_size(pill, LV_SIZE_CONTENT, lv_pct(100));
	lv_obj_set_style_min_width(pill, 132, 0);
	lv_obj_set_style_bg_color(pill, THEME_COLOR_SECTION_BG, 0);
	lv_obj_set_style_bg_color(pill, THEME_COLOR_BTN_GRAY, LV_STATE_PRESSED);
	lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(pill, 10, 0);
	lv_obj_set_style_pad_hor(pill, 18, 0);
	lv_obj_set_style_pad_row(pill, 0, 0);
	lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(pill, _drawer_open_cb, LV_EVENT_CLICKED, NULL);

	char active[LAYOUT_MAX_NAME] = {0};
	layout_manager_get_active(active, sizeof(active));
	/* Layout names are file names: show "track_night" as "TRACK NIGHT". */
	char shown[LAYOUT_MAX_NAME];
	snprintf(shown, sizeof(shown), "%s", active[0] ? active : "Layout");
	for (char *c = shown; *c; c++) if (*c == '_') *c = ' ';
	s_dock_name = uk_label(pill, shown, UK_FONT_TITLE, UK_TONE_TEXT);
	/* Sized to the name: the big face when it fits, a size down when it
	 * doesn't, and only then cut short — on one line, which LONG_DOT only
	 * keeps when the label has a fixed height as well as a width. */
	{
		/* 180 keeps the widest dock inside the screen: arrows 100 + pill 216 +
		 * brightness 150 + three buttons 228 + gaps. */
		const lv_coord_t MAX_W = 180;
		const char *caps = lv_label_get_text(s_dock_name);
		lv_point_t sz;
		lv_txt_get_size(&sz, caps, uk_font(UK_FONT_TITLE), 1, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (sz.x > MAX_W) {
			lv_obj_set_style_text_font(s_dock_name, uk_font(UK_FONT_HEAD), 0);
			lv_txt_get_size(&sz, caps, uk_font(UK_FONT_HEAD), 1, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
			if (sz.x > MAX_W) {
				lv_obj_set_size(s_dock_name, MAX_W, lv_font_get_line_height(uk_font(UK_FONT_HEAD)));
				lv_label_set_long_mode(s_dock_name, LV_LABEL_LONG_DOT);
			}
		}
	}

	/* Where this layout sits: "3 of 9", the number a size up. */
	lv_obj_t *count = lv_obj_create(pill);
	lv_obj_remove_style_all(count);
	lv_obj_set_size(count, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(count, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(count, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(count, 5, 0);
	lv_obj_clear_flag(count, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	char num[8];
	if (s_layout_index >= 0) snprintf(num, sizeof(num), "%d", s_layout_index + 1);
	else snprintf(num, sizeof(num), "-");
	uk_label(count, num, UK_FONT_LABEL, UK_TONE_TEXT);
	char of[16];
	snprintf(of, sizeof(of), "of %d", s_layout_count);
	lv_obj_t *of_lbl = uk_label(count, of, UK_FONT_SMALL, UK_TONE_MUTED);
	lv_obj_set_style_pad_bottom(of_lbl, 1, 0);

	ui_Layout_Next_Button = uk_btn(lay, UK_ICON_RIGHT, NULL, UK_BTN_NEUTRAL, _layout_next_clicked_cb, NULL);
	lv_obj_set_size(ui_Layout_Next_Button, 50, lv_pct(100));
	lv_obj_set_style_radius(ui_Layout_Next_Button, 10, 0);
	if (s_layout_count < 2) {
		/* One layout: nothing to step to. The name still opens the drawer. */
		lv_obj_add_state(ui_Layout_Prev_Button, LV_STATE_DISABLED);
		lv_obj_add_state(ui_Layout_Next_Button, LV_STATE_DISABLED);
		lv_obj_add_flag(count, LV_OBJ_FLAG_HIDDEN);
	}

	/* ── Brightness ── */
	lv_obj_t *bri = _dock_seg(s_dock, 1);
	lv_obj_set_width(bri, 0);   /* all of the room the name pill leaves */
	lv_obj_set_style_min_width(bri, 150, 0);
	lv_obj_set_style_pad_hor(bri, 14, 0);
	lv_obj_set_style_pad_column(bri, 14, 0);
	uk_icon(bri, UK_ICON_SUN, UK_ICON_MD, UK_TONE_MUTED);
	s_dock_slider = lv_slider_create(bri);
	lv_obj_set_height(s_dock_slider, 6);
	lv_obj_set_flex_grow(s_dock_slider, 1);
	lv_slider_set_range(s_dock_slider, 5, 100);
	uk_style_slider(s_dock_slider);
	lv_obj_set_ext_click_area(s_dock_slider, 24);
	lv_obj_add_event_cb(s_dock_slider, _dock_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

	/* ── Dim / Record / Setup ── */
	s_dock_dim = _dock_btn(s_dock, UK_ICON_MOON, "Dim", _dock_dim_cb);
	s_dock_rec = _dock_btn(s_dock, UK_ICON_REC, "Rec", _dock_rec_cb);
	_dock_btn(s_dock, UK_ICON_GEAR, "Setup", _dock_setup_cb);

	/* Edit mode and older callers hide "the menu button" to clear the chrome;
	 * the dock is that button now. */
	ui_Menu_Button = s_dock;
	_dock_refresh();
}

void ui_Screen3_rebuild(void) {
	if (s_layout_switching) return;
	s_layout_switching = true;
	s_stale_dash = ui_Screen3;
	lv_async_call(_deferred_layout_reload, NULL);
}

void ui_Screen3_switch_layout(const char *name) {
	if (!name || !name[0] || s_layout_switching) return;
	s_layout_switching = true;
	s_stale_dash = ui_Screen3;
	layout_manager_set_active(name);
	lv_async_call(_deferred_layout_reload, NULL);
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

    /* The setup wizard sits over the dashboard on this same screen, and its
     * CAN step already says "No CAN traffic detected". The badge foregrounds
     * itself on every show, so it landed ON TOP of the wizard card, covering
     * the step's own help text. Stay out of the way while the wizard is up;
     * the silence clock keeps running, so it reappears as soon as it closes. */
    if (first_run_wizard_is_open()) {
        if (!lv_obj_has_flag(s_bus_silent_badge, LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(s_bus_silent_badge, LV_OBJ_FLAG_HIDDEN);
        return;
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
	lv_obj_set_style_bg_color(ui_Screen3, WIDGET_COLOR_BG,
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

	/* Cache the switchable-layout cycle once, here — the dock reads the cache
	 * so a tap never touches the filesystem (see s_layout_count). */
	{
		char active[LAYOUT_MAX_NAME] = {0};
		layout_manager_get_active(active, sizeof(active));
		s_layout_index = layout_switcher_position(active, &s_layout_count);
		snprintf(s_active_layout, sizeof(s_active_layout), "%s", active);
		layout_thumbs_mark_loaded(active);
	}

	_dock_create(ui_Screen3);

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
	lv_obj_set_style_bg_color(ui_Screen3, WIDGET_COLOR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Screen3, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	/* Tap-on-background -> reveal chrome (see notes on screen3_touch_event_cb). */
	lv_obj_add_event_cb(ui_Screen3, screen3_touch_event_cb,
						LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_add_event_cb(ui_Screen3, edit_mode_screen_pressed_cb,
						LV_EVENT_PRESSED, NULL);

	dashboard_apply_layout_json(ui_Screen3, root);
}
