#include "splash_screen.h"
#include "ui.h"
#include "../theme.h"
#include "ui_helpers.h"
#include "ui_Screen3.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "layout/layout_manager.h"
#include "widgets/font_manager.h"
#include "widgets/signal.h"
#include "widgets/widget_registry.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "splash";

#define SPLASH_MAX_WIDGETS 16

/* Forward declarations */
void ui_Screen3_screen_init(void);

/* ── State ────────────────────────────────────────────────────────────── */

static lv_obj_t *splash_screen = NULL;
static esp_timer_handle_t splash_timer = NULL;
static bool s_edit_mode = false;
static char s_active_splash_name[LAYOUT_MAX_NAME] = "Default";

/* Widget tracking for splash edit mode (parallels dashboard.c) */
static widget_t *s_widgets[SPLASH_MAX_WIDGETS];
static uint8_t s_widget_count = 0;

/* ── Helpers ──────────────────────────────────────────────────────────── */

/** Build the full layout name for the active splash (e.g. "_splash_Default"). */
static void _build_splash_layout_name(char *out, size_t len)
{
	snprintf(out, len, "_splash_%s", s_active_splash_name);
}

static bool _splash_file_exists(void)
{
	char name[LAYOUT_MAX_NAME];
	_build_splash_layout_name(name, sizeof(name));
	char path[80];
	snprintf(path, sizeof(path), "%s/%s.json", LFS_LAYOUT_DIR, name);
	struct stat st;
	return (stat(path, &st) == 0 && st.st_size > 0);
}

/** Create a black full-screen LVGL object. */
static lv_obj_t *_create_screen(void)
{
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, THEME_COLOR_BG,
	                          LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(scr, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	return scr;
}

/** Show the default RDM logo on the given screen. */
static void _show_default_logo(lv_obj_t *parent)
{
	lv_obj_t *logo = lv_img_create(parent);
	lv_img_set_src(logo, &ui_img_RDM_Light);
	lv_img_set_zoom(logo, 320); /* 125% */
	lv_obj_set_size(logo, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_center(logo);
}

/** Load splash widgets from _splash_<name>.json into the given parent.
 *  Returns true on success, false on failure (caller should show default). */
static bool _load_splash_layout(lv_obj_t *parent)
{
	if (!_splash_file_exists()) return false;

	char layout_name[LAYOUT_MAX_NAME];
	_build_splash_layout_name(layout_name, sizeof(layout_name));

	/* Reset registries for splash loading. Signal registry is harmless
	 * to reset here since splash is loaded before or instead of dashboard. */
	font_manager_reset_instances();
	font_manager_init();
	signal_registry_init();
	widget_registry_reset();

	esp_err_t err = layout_manager_load(layout_name, parent);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "Failed to load %s: %s", layout_name,
		         esp_err_to_name(err));
		return false;
	}

	widget_registry_snapshot(s_widgets, SPLASH_MAX_WIDGETS, &s_widget_count);
	ESP_LOGI(TAG, "Loaded custom splash '%s' (%d widgets)",
	         s_active_splash_name, s_widget_count);
	return true;
}

/* ── Boot splash ─────────────────────────────────────────────────────── */

/** Phase 2: build and show the dashboard (runs after black frame rendered). */
static void _splash_build_dashboard(lv_timer_t *t)
{
	(void)t;

	ui_Screen3_screen_init();

	/* Fade in from black — LVGL renders the dashboard off-screen first,
	 * then cross-fades over 300ms. auto_del=true cleans up the black screen. */
	lv_scr_load_anim(ui_Screen3, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, true);
}

/** Phase 1: show a clean black screen while dashboard builds (LVGL task). */
static void _splash_transition_cb(void *arg)
{
	(void)arg;

	/* Create a solid black screen */
	lv_obj_t *black = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(black, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(black, LV_OPA_COVER, 0);
	lv_obj_clear_flag(black, LV_OBJ_FLAG_SCROLLABLE);

	/* Fade splash to black over 200ms, auto-delete splash screen */
	splash_screen = NULL;  /* lv_scr_load_anim will delete it */
	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));
	lv_scr_load_anim(black, LV_SCR_LOAD_ANIM_FADE_ON, 200, 0, true);

	/* Build dashboard after fade-to-black completes + one extra frame
	 * for the black screen to fully render. */
	lv_timer_t *t = lv_timer_create(_splash_build_dashboard, 250, NULL);
	lv_timer_set_repeat_count(t, 1);
}

/** esp_timer callback — runs on the esp_timer task, NOT safe for LVGL calls. */
static void splash_timer_cb(void *arg)
{
	(void)arg;
	/* Defer all LVGL work to the LVGL task */
	lv_async_call(_splash_transition_cb, NULL);

	/* Clean up the esp_timer (safe from esp_timer callback context) */
	if (splash_timer) {
		esp_timer_delete(splash_timer);
		splash_timer = NULL;
	}
}

void show_splash_screen(void)
{
	splash_screen = _create_screen();

	/* Read active splash name from NVS */
	layout_manager_init(); /* idempotent — ensures LittleFS mounted */
	layout_manager_get_active_splash(s_active_splash_name,
	                                 sizeof(s_active_splash_name));
	ESP_LOGI(TAG, "Active splash: '%s'", s_active_splash_name);

	/* Try to load custom splash layout */
	if (!_load_splash_layout(splash_screen)) {
		/* Fallback to default RDM logo */
		_show_default_logo(splash_screen);
	}

	lv_scr_load(splash_screen);

	/* Auto-transition to dashboard after 900 ms */
	esp_timer_create_args_t timer_args = {
		.callback = splash_timer_cb,
		.name = "splash_timer"
	};
	esp_timer_create(&timer_args, &splash_timer);
	esp_timer_start_once(splash_timer, 900000);
}

/* ── Edit mode API ───────────────────────────────────────────────────── */

bool splash_screen_is_edit_mode(void) { return s_edit_mode; }

bool splash_screen_has_custom(void) { return _splash_file_exists(); }

const char *splash_screen_get_active_name(void) { return s_active_splash_name; }

void splash_screen_set_active_name(const char *name)
{
	if (!name || !name[0]) return;
	strncpy(s_active_splash_name, name, sizeof(s_active_splash_name) - 1);
	s_active_splash_name[sizeof(s_active_splash_name) - 1] = '\0';
}

widget_t **splash_screen_get_widgets(void) { return s_widgets; }

uint8_t splash_screen_get_widget_count(void) { return s_widget_count; }

void splash_screen_enter_edit_mode(void)
{
	s_edit_mode = true;

	/* Cancel boot timer if still running */
	if (splash_timer) {
		esp_timer_stop(splash_timer);
		esp_timer_delete(splash_timer);
		splash_timer = NULL;
	}

	/* Read active splash name from NVS */
	layout_manager_init();
	layout_manager_get_active_splash(s_active_splash_name,
	                                 sizeof(s_active_splash_name));

	/* Create a new splash screen for editing */
	lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());

	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));

	splash_screen = _create_screen();

	if (!_load_splash_layout(splash_screen)) {
		/* No custom splash yet — show empty black screen for editing */
		ESP_LOGI(TAG, "No custom splash — showing blank canvas");
	}

	lv_scr_load(splash_screen);
	if (old && old != splash_screen)
		lv_obj_del(old);

	ESP_LOGI(TAG, "Entered splash edit mode");
}

void splash_screen_exit_edit_mode(void)
{
	s_edit_mode = false;
	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));

	/* Reload the dashboard */
	lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
	ui_Screen3_screen_init();
	lv_scr_load(ui_Screen3);
	if (old && old != ui_Screen3)
		lv_obj_del(old);

	ESP_LOGI(TAG, "Exited splash edit mode");
}

void splash_screen_apply_preview(cJSON *root)
{
	if (!root) return;

	lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());

	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));

	splash_screen = _create_screen();

	/* Reset registries and apply the JSON layout */
	font_manager_reset_instances();
	widget_registry_reset();

	layout_manager_init();
	esp_err_t err = layout_manager_apply_json(root, splash_screen);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "splash preview apply failed: %s",
		         esp_err_to_name(err));
	}

	widget_registry_snapshot(s_widgets, SPLASH_MAX_WIDGETS, &s_widget_count);

	lv_scr_load(splash_screen);
	if (old && old != splash_screen)
		lv_obj_del(old);
}
