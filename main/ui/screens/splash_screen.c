#include "splash_screen.h"
#include "system/rdm_lv_async.h"
#include "ui.h"
#include "../theme.h"
#include "ui_helpers.h"
#include "ui_Screen3.h"
#include "first_run_wizard.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "layout/layout_manager.h"
#include "widgets/font_manager.h"
#include "widgets/signal.h"
#include "storage/config_store.h"
#include "widgets/widget_registry.h"
#include "storage/config_store.h"
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
static bool s_fade_enabled = true;
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
static void _first_run_check_cb(lv_timer_t *t)
{
	(void)t;
	bool done = false;
	if (config_store_load_first_run_done(&done) == ESP_OK && !done) {
		show_first_run_wizard();
	}
}

/* ── Dashboard entrance: top-to-bottom curtain reveal ────────────────────
 * The single boot animation: the fully-built (and already live) dashboard is
 * covered by an opaque black curtain whose edge sweeps down the screen, with
 * a soft semi-transparent band trailing the edge so content appears to fade
 * in as the edge passes over it. A single on/off setting (config_store
 * boot_anim) chooses this reveal vs. an instant appear. Runs on the LVGL
 * task and assumes ui_Screen3 is built.
 *
 * Why a curtain and not the old per-widget staggered fades: any opacity fade
 * invalidates the widget's full bounding box every animation tick, and the
 * big centre widgets (450 px meter, 600 px arcs) overlap to near-full-screen
 * — a full repaint of a real layout costs ~150 ms, so the staggered fade ran
 * at 5-7 fps no matter how cheaply the fade itself rendered (opa_layered's
 * chunked intermediate layers: 201 ms/frame; plain recursive opa: still
 * 157 ms — measured with tools/perf_bootanim.py on Time_Attack). The curtain
 * inverts the cost: per frame only the freshly revealed strip (~10-20 rows)
 * renders, and everything still covered is skipped entirely by LVGL's
 * cover-check because the curtain is opaque. Frame cost is bounded by sweep
 * speed, not by layout complexity. */

#define REVEAL_SLAT_H       16    /* px per curtain slat */
#define REVEAL_MAX_SLATS    40    /* supports screens up to 640 px tall */
#define REVEAL_SOFT_STEPS   4     /* strips in the soft trailing edge */
#define REVEAL_SOFT_STEP_H  10    /* px per strip */
#define REVEAL_SWEEP_MS     950   /* edge travel time over the screen */

typedef struct {
	lv_obj_t *slat[REVEAL_MAX_SLATS];    /* opaque cover, hidden one by one */
	uint8_t   slat_count;
	uint8_t   slats_hidden;              /* slats already revealed */
	lv_obj_t *soft[REVEAL_SOFT_STEPS];   /* feathered band above the edge */
	lv_coord_t scr_h;
} reveal_ctx_t;

static reveal_ctx_t s_reveal;

/* Edge position v sweeps 0 .. scr_h + soft band height.
 *
 * The curtain is built from fixed slats that get LV_OBJ_FLAG_HIDDEN as the
 * edge passes them — hiding invalidates ONLY that slat's few rows, unlike
 * moving/resizing one big cover, which invalidates the whole remaining
 * cover area every tick (still ~full-screen union → the cover-check can't
 * rescue it because the union also contains the uncovered fresh strip).
 * With slats, the still-covered region is never invalidated at all: the
 * framebuffer already holds its black pixels. Per tick the work is the
 * newly revealed rows (the full layout renders exactly once, amortized
 * across the sweep) plus the small soft band. */
static void _reveal_anim_cb(void *var, int32_t v)
{
	(void)var;
	reveal_ctx_t *r = &s_reveal;

	/* Hide every slat whose bottom edge is above the sweep position. They
	 * are created top-to-bottom, so this is a simple cursor walk. */
	while (r->slats_hidden < r->slat_count) {
		lv_coord_t bottom = (lv_coord_t)(r->slats_hidden + 1) * REVEAL_SLAT_H;
		if (bottom > (lv_coord_t)v) break;
		lv_obj_t *s = r->slat[r->slats_hidden];
		if (s && lv_obj_is_valid(s))
			lv_obj_add_flag(s, LV_OBJ_FLAG_HIDDEN);
		r->slats_hidden++;
	}

	for (int i = 0; i < REVEAL_SOFT_STEPS; i++) {
		lv_obj_t *s = r->soft[i];
		if (!s || !lv_obj_is_valid(s)) continue;
		lv_coord_t sy = (lv_coord_t)v - (lv_coord_t)(i + 1) * REVEAL_SOFT_STEP_H;
		/* Park strips just off-screen until the sweep brings them in, and
		 * let them run off the bottom at the end. */
		if (sy < -REVEAL_SOFT_STEP_H) sy = -REVEAL_SOFT_STEP_H;
		lv_obj_set_y(s, sy);
	}
}

static void _reveal_anim_ready_cb(lv_anim_t *a)
{
	(void)a;
	reveal_ctx_t *r = &s_reveal;
	for (int i = 0; i < r->slat_count; i++) {
		if (r->slat[i] && lv_obj_is_valid(r->slat[i])) lv_obj_del(r->slat[i]);
		r->slat[i] = NULL;
	}
	r->slat_count = 0;
	r->slats_hidden = 0;
	for (int i = 0; i < REVEAL_SOFT_STEPS; i++) {
		if (r->soft[i] && lv_obj_is_valid(r->soft[i])) lv_obj_del(r->soft[i]);
		r->soft[i] = NULL;
	}
	/* Resume live updates right away (the pause would also self-expire). */
	signal_dispatch_pause_ms(0);
}

/* Build the slat curtain + soft band over the (already-built) ui_Screen3 and
 * start the sweep. Must run BEFORE the screen becomes visible so the finished
 * layout never flashes into view ahead of the animation. */
static void _start_curtain_reveal(void)
{
	lv_coord_t w = lv_disp_get_hor_res(lv_disp_get_default());
	lv_coord_t h = lv_disp_get_ver_res(lv_disp_get_default());
	s_reveal.scr_h = h;
	s_reveal.slats_hidden = 0;

	uint8_t n = (uint8_t)((h + REVEAL_SLAT_H - 1) / REVEAL_SLAT_H);
	if (n > REVEAL_MAX_SLATS) n = REVEAL_MAX_SLATS;
	s_reveal.slat_count = n;
	for (uint8_t i = 0; i < n; i++) {
		lv_obj_t *s = lv_obj_create(ui_Screen3);
		lv_obj_remove_style_all(s);
		lv_obj_set_size(s, w, REVEAL_SLAT_H);
		lv_obj_set_pos(s, 0, (lv_coord_t)i * REVEAL_SLAT_H);
		lv_obj_set_style_bg_color(s, lv_color_black(), LV_PART_MAIN);
		lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
		s_reveal.slat[i] = s;
	}

	for (int i = 0; i < REVEAL_SOFT_STEPS; i++) {
		lv_obj_t *s = lv_obj_create(ui_Screen3);
		lv_obj_remove_style_all(s);
		lv_obj_set_size(s, w, REVEAL_SOFT_STEP_H);
		lv_obj_set_pos(s, 0, -REVEAL_SOFT_STEP_H);
		lv_obj_set_style_bg_color(s, lv_color_black(), LV_PART_MAIN);
		/* i=0 is nearest the curtain → most opaque. */
		lv_obj_set_style_bg_opa(s,
		        (lv_opa_t)(255 * (REVEAL_SOFT_STEPS - i) /
		                   (REVEAL_SOFT_STEPS + 1)),
		        LV_PART_MAIN);
		lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
		s_reveal.soft[i] = s;
	}

	/* Freeze CAN-driven widget updates while the curtain sweeps: every live
	 * needle/value change would invalidate (and re-render) the expensive
	 * centre widgets on top of the reveal work. Deadline-based with margin,
	 * so it self-expires even if the ready callback never runs. */
	signal_dispatch_pause_ms(REVEAL_SWEEP_MS + 500);

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &s_reveal);
	lv_anim_set_values(&a, 0,
	                   h + REVEAL_SOFT_STEPS * REVEAL_SOFT_STEP_H);
	lv_anim_set_time(&a, REVEAL_SWEEP_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_exec_cb(&a, _reveal_anim_cb);
	lv_anim_set_ready_cb(&a, _reveal_anim_ready_cb);
	lv_anim_start(&a);
}

/* Show the dashboard. When animate is set, cover it with the curtain first,
 * swap the screen in instantly (both screens are black so there's no visible
 * cut), then sweep the curtain away top-to-bottom. Otherwise the layout just
 * appears. */
static void _reveal_dashboard_now(bool animate)
{
	if (animate) _start_curtain_reveal();
	lv_scr_load_anim(ui_Screen3, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

void splash_screen_reveal_dashboard(void)
{
	ui_Screen3_screen_init();

	/* Reset our splash-widget bookkeeping — the dashboard owns the registry
	 * now, and the old splash screen is about to be auto-deleted. */
	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));

	bool animate = true;
	config_store_load_boot_anim(&animate);
	_reveal_dashboard_now(animate);
}

void splash_screen_preview_boot_anim(void)
{
	if (!ui_Screen3 || !lv_obj_is_valid(ui_Screen3)) return;

	/* Cover the live dashboard with black, then re-run the layered reveal on the
	 * EXISTING Screen3 (no rebuild). _reveal_dashboard_now auto-deletes this
	 * cover when it swaps the dashboard back in. */
	lv_obj_t *black = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(black, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(black, LV_OPA_COVER, 0);
	lv_obj_clear_flag(black, LV_OBJ_FLAG_SCROLLABLE);
	lv_scr_load(black);

	_reveal_dashboard_now(true);
}

static void _splash_build_dashboard(lv_timer_t *t)
{
	(void)t;

	/* Dashboard is built here (on the black screen, unseen) then swept in. */
	splash_screen_reveal_dashboard();

	/* First-run wizard: show 800ms after dashboard appears so the user sees
	   the dashboard first, then the welcome overlay. Check NVS before showing
	   so returning users are not disturbed. */
	lv_timer_t *wiz = lv_timer_create(_first_run_check_cb, 800, NULL);
	lv_timer_set_repeat_count(wiz, 1);
}

/* Drives the splash-cover opacity during the fade-out. */
static void _splash_cover_opa_cb(void *obj, int32_t v)
{
	if (obj && lv_obj_is_valid((lv_obj_t *)obj))
		lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN);
}

/* Fires exactly when the black cover has reached full opacity — i.e. the splash
 * is 100% hidden. Only now do we build + reveal the dashboard. Chaining off the
 * animation's real completion (instead of a fixed timer) guarantees the splash
 * has fully faded out before anything else touches the screen, even at low boot
 * FPS. The cover is a child of the splash screen, so the reveal's screen swap
 * auto-deletes both together. */
static void _splash_fade_ready_cb(lv_anim_t *a)
{
	(void)a;
	_splash_build_dashboard(NULL);
}

/** Phase 1: fade the splash fully out, then build + reveal the dashboard. */
static void _splash_transition_cb(void *arg)
{
	(void)arg;

	splash_screen = NULL;
	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));

	if (s_fade_enabled) {
		/* Overlay a full-screen black cover on the splash and animate its
		 * opacity 0 → 255. When it's fully opaque the splash is invisible, and
		 * the ready_cb chains the dashboard reveal. Center-aligned at full
		 * display resolution so it covers every pixel regardless of screen
		 * padding. */
		lv_obj_t *cover = lv_obj_create(lv_scr_act());
		lv_obj_remove_style_all(cover);
		lv_coord_t w = lv_disp_get_hor_res(lv_disp_get_default());
		lv_coord_t h = lv_disp_get_ver_res(lv_disp_get_default());
		lv_obj_set_size(cover, w, h);
		lv_obj_set_align(cover, LV_ALIGN_CENTER);
		lv_obj_set_style_bg_color(cover, lv_color_black(), LV_PART_MAIN);
		lv_obj_set_style_bg_opa(cover, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, cover);
		lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
		lv_anim_set_time(&a, 350);
		lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
		lv_anim_set_exec_cb(&a, _splash_cover_opa_cb);
		lv_anim_set_ready_cb(&a, _splash_fade_ready_cb);
		lv_anim_start(&a);
	} else {
		/* No splash fade — build + reveal directly. The reveal animation still
		 * applies (it swaps from the splash, which it auto-deletes), so the
		 * chosen load style is honoured even with the fade transition off. */
		_splash_build_dashboard(NULL);
	}
}

/** esp_timer callback — runs on the esp_timer task, NOT safe for LVGL calls. */
static void splash_timer_cb(void *arg)
{
	(void)arg;
	/* Defer all LVGL work to the LVGL task */
	rdm_async_call(_splash_transition_cb, NULL);

	/* Clean up the esp_timer (safe from esp_timer callback context) */
	if (splash_timer) {
		esp_timer_delete(splash_timer);
		splash_timer = NULL;
	}
}

void show_splash_screen(void)
{
	splash_screen = _create_screen();

	/* Read settings from NVS */
	layout_manager_init(); /* idempotent — ensures LittleFS mounted */
	layout_manager_get_active_splash(s_active_splash_name,
	                                 sizeof(s_active_splash_name));
	config_store_load_splash_fade(&s_fade_enabled);
	ESP_LOGI(TAG, "Active splash: '%s', fade: %s", s_active_splash_name,
	         s_fade_enabled ? "on" : "off");

	/* Try to load custom splash layout */
	if (!_load_splash_layout(splash_screen)) {
		/* Fallback to default RDM logo */
		_show_default_logo(splash_screen);
	}

	lv_scr_load(splash_screen);

	/* Paint the splash in ONE synchronous pass before returning. show_splash_screen
	 * runs inside ui_init() while app_main holds the LVGL mutex, so without this
	 * the first splash frame (logo decode + panel flush) would instead render
	 * asynchronously on the LVGL task right as the backlight is already on — and
	 * a partial/interrupted first frame shows up as a quick tear. lv_refr_now()
	 * forces the whole splash to render and flush atomically here, so the panel
	 * goes straight from black to a fully-drawn splash. */
	lv_refr_now(NULL);

	/* Auto-transition to dashboard after 900 ms */
	esp_timer_create_args_t timer_args = {
		.callback = splash_timer_cb,
		.name = "splash_timer"
	};
	esp_timer_create(&timer_args, &splash_timer);
	esp_timer_start_once(splash_timer, 900000);
}

/* ── WiFi-loss fallback ──────────────────────────────────────────────────
 * If WiFi drops while a splash screen is still showing (boot splash OR
 * splash edit-mode), transition to the dashboard so the user isn't stuck
 * looking at the splash. A WiFi drop during normal driving (dashboard
 * already up) is a no-op. Runs on the LVGL task (the ui_wifi event callback
 * is already dispatched there via lv_async_call), so it may call lv_* and
 * the splash transition helpers directly. */
void splash_screen_handle_wifi_lost(void)
{
	if (s_edit_mode) {
		/* Editing a splash over WiFi — the connection is gone, so rebuild
		 * and load the dashboard. */
		ESP_LOGI(TAG, "WiFi lost in splash edit mode — exiting to dashboard");
		splash_screen_exit_edit_mode();
		return;
	}

	if (splash_screen != NULL && splash_timer != NULL) {
		/* Boot splash still up — cancel the auto-transition timer and run
		 * the same transition immediately. */
		ESP_LOGI(TAG, "WiFi lost during boot splash — jumping to dashboard");
		esp_timer_stop(splash_timer);
		esp_timer_delete(splash_timer);
		splash_timer = NULL;
		_splash_transition_cb(NULL);
		return;
	}

	/* Dashboard already showing (or transition already in flight) — no-op. */
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
