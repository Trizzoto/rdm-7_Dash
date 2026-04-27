/* menu_screen.c — config modal host screen */
#include "menu_screen.h"
#include "../dashboard.h"
#include "../screens/ui_Screen3.h"
#include "../theme.h"
#include "../ui.h"
#include "config_modal.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Externs not already covered by the headers above */
extern void reconfigure_can_filter(void);

/* Global previewer references */
lv_obj_t *menu_rpm_value_label = NULL;
lv_obj_t *menu_panel_value_labels[8] = {NULL};
lv_obj_t *menu_panel_boxes[8] = {NULL};
lv_obj_t *menu_panel_labels[8] = {NULL};
lv_obj_t *menu_bar_objects[2] = {NULL};
lv_obj_t *menu_bar_labels[2] = {NULL};

/* ── private helpers ──────────────────────────────────────────── */

static void delete_old_screen_cb(lv_timer_t *t) {
	lv_obj_t *s = (lv_obj_t *)t->user_data;
	if (s) {
		lv_obj_del(s);
	}
}

static void clear_menu_refs(void) {
	for (int i = 0; i < 8; i++)
		menu_panel_value_labels[i] = menu_panel_boxes[i] =
			menu_panel_labels[i] = NULL;
	for (int i = 0; i < 2; i++)
		menu_bar_objects[i] = menu_bar_labels[i] = NULL;
}

static void do_screen_transition(lv_obj_t *old, lv_obj_t *btn) {
	ui_Screen3_screen_init();
	if (!ui_Screen3) {
		lv_obj_clear_state(btn, LV_STATE_DISABLED);
		return;
	}
	lv_scr_load_anim(ui_Screen3, LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
	lv_timer_t *dt = lv_timer_create(delete_old_screen_cb, 300, old);
	lv_timer_set_repeat_count(dt, 1);
	clear_menu_refs();
}

/* ── deferred transition context ─────────────────────────────── */

typedef struct {
	lv_obj_t *old_screen;
	lv_obj_t *btn;
	lv_obj_t *indicator;
} deferred_transition_t;

static void _deferred_save_transition_cb(lv_timer_t *t) {
	deferred_transition_t *ctx = (deferred_transition_t *)t->user_data;
	if (ctx->indicator && lv_obj_is_valid(ctx->indicator))
		lv_obj_del(ctx->indicator);
	do_screen_transition(ctx->old_screen, ctx->btn);
	free(ctx);
}

static void _deferred_cancel_transition_cb(lv_timer_t *t) {
	deferred_transition_t *ctx = (deferred_transition_t *)t->user_data;
	if (ctx->indicator && lv_obj_is_valid(ctx->indicator))
		lv_obj_del(ctx->indicator);
	do_screen_transition(ctx->old_screen, ctx->btn);
	free(ctx);
}

/* ── public API ───────────────────────────────────────────────── */

void close_menu_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	menu_rpm_value_label = NULL;

	lv_obj_t *old = lv_scr_act();
	lv_obj_t *btn = lv_event_get_target(e);
	lv_obj_add_state(btn, LV_STATE_DISABLED);

	lv_obj_t *ind = lv_label_create(old);
	lv_label_set_text(ind, "Saving...");
	lv_obj_align(ind, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_text_color(ind, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(ind, THEME_FONT_MEDIUM, 0);
	lv_refr_now(NULL);

	/* Persist widget config into JSON layout on LittleFS */
	dashboard_persist_layout();
	reconfigure_can_filter();

	/* Defer the screen transition so the LVGL task is not blocked */
	deferred_transition_t *ctx = malloc(sizeof(deferred_transition_t));
	if (ctx) {
		ctx->old_screen = old;
		ctx->btn = btn;
		ctx->indicator = ind;
		lv_timer_t *tm = lv_timer_create(_deferred_save_transition_cb, 50, ctx);
		lv_timer_set_repeat_count(tm, 1);
	} else {
		/* Fallback: transition immediately if malloc fails */
		lv_obj_del(ind);
		do_screen_transition(old, btn);
	}
}

void cancel_menu_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	menu_rpm_value_label = NULL;

	lv_obj_t *old = lv_scr_act();
	lv_obj_t *btn = lv_event_get_target(e);
	lv_obj_add_state(btn, LV_STATE_DISABLED);

	lv_obj_t *ind = lv_label_create(old);
	lv_label_set_text(ind, "Canceling...");
	lv_obj_align(ind, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_text_color(ind, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(ind, THEME_FONT_MEDIUM, 0);
	lv_refr_now(NULL);

	/* Defer the screen transition so the LVGL task is not blocked */
	deferred_transition_t *ctx = malloc(sizeof(deferred_transition_t));
	if (ctx) {
		ctx->old_screen = old;
		ctx->btn = btn;
		ctx->indicator = ind;
		lv_timer_t *tm = lv_timer_create(_deferred_cancel_transition_cb, 200, ctx);
		lv_timer_set_repeat_count(tm, 1);
	} else {
		lv_obj_del(ind);
		do_screen_transition(old, btn);
	}
}

void load_menu_screen_for_widget(widget_t *w) {
	if (!w) return;

	ui_MenuScreen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(ui_MenuScreen, THEME_COLOR_BG, 0);
	lv_obj_set_style_bg_opa(ui_MenuScreen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(ui_MenuScreen, LV_OBJ_FLAG_SCROLLABLE);

	/* All widget types now use the unified tabbed modal */
	config_modal_open_for_widget(ui_MenuScreen, w);
	lv_scr_load(ui_MenuScreen);
}


