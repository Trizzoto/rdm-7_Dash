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

#define SPLASH_LAYOUT_NAME "_splash"
#define SPLASH_MAX_WIDGETS 16

/* Forward declarations */
void ui_Screen3_screen_init(void);

/* ── State ────────────────────────────────────────────────────────────── */

static lv_obj_t *splash_screen = NULL;
static esp_timer_handle_t splash_timer = NULL;
static bool s_edit_mode = false;

/* Widget tracking for splash edit mode (parallels dashboard.c) */
static widget_t *s_widgets[SPLASH_MAX_WIDGETS];
static uint8_t s_widget_count = 0;

/* ── Helpers ──────────────────────────────────────────────────────────── */

static bool _splash_file_exists(void)
{
	char path[64];
	snprintf(path, sizeof(path), "/lfs/layouts/%s.json", SPLASH_LAYOUT_NAME);
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

/** Load splash widgets from _splash.json into the given parent.
 *  Returns true on success, false on failure (caller should show default). */
static bool _load_splash_layout(lv_obj_t *parent)
{
	if (!_splash_file_exists()) return false;

	/* Reset registries for splash loading. Signal registry is harmless
	 * to reset here since splash is loaded before or instead of dashboard. */
	font_manager_reset_instances();
	font_manager_init();
	signal_registry_init();
	widget_registry_reset();

	esp_err_t err = layout_manager_load(SPLASH_LAYOUT_NAME, parent);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "Failed to load %s: %s", SPLASH_LAYOUT_NAME,
		         esp_err_to_name(err));
		return false;
	}

	widget_registry_snapshot(s_widgets, SPLASH_MAX_WIDGETS, &s_widget_count);
	ESP_LOGI(TAG, "Loaded custom splash (%d widgets)", s_widget_count);
	return true;
}

/* ── Boot splash ─────────────────────────────────────────────────────── */

static void splash_timer_cb(void *arg)
{
	(void)arg;
	/* Initialize and load the main dashboard screen */
	ui_Screen3_screen_init();
	lv_scr_load(ui_Screen3);

	/* Clean up splash */
	if (splash_screen) {
		lv_obj_del(splash_screen);
		splash_screen = NULL;
	}
	if (splash_timer) {
		esp_timer_delete(splash_timer);
		splash_timer = NULL;
	}
	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));
}

void show_splash_screen(void)
{
	splash_screen = _create_screen();

	/* Try to load custom splash layout */
	layout_manager_init(); /* idempotent — ensures LittleFS mounted */
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

	/* Create a new splash screen for editing */
	lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());

	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));

	splash_screen = _create_screen();

	layout_manager_init();
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
