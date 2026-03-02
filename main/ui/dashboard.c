#include "ui/dashboard.h"
#include "layout/layout_manager.h"

/* Existing widget create functions — used as fallback */
#include "widgets/widget_bar.h"
#include "widgets/widget_gear.h"
#include "widgets/widget_indicator.h"
#include "widgets/widget_panel.h"
#include "widgets/widget_rpm_bar.h"
#include "widgets/widget_speed.h"
#include "widgets/widget_warning.h"


/* CAN dispatch rebuild */
#include "can/can_dispatch.h"

#include "esp_log.h"
#include <stdlib.h>
#include <string.h>


static const char *TAG = "dashboard";

/* ── Internal widget registry ────────────────────────────────────────────── */

/* Maximum widgets the dashboard tracks (7 types × worst-case instances):
 *   panel×8, rpm_bar×1, speed×1, gear×1, bar×2, indicator×2, warning×8 = 23 */
#define DASHBOARD_MAX_WIDGETS 24

static widget_t *s_widgets[DASHBOARD_MAX_WIDGETS];
static uint8_t s_widget_count = 0;

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
	widget_speed_create(parent);
	widget_gear_create(parent);
	widget_bar_create(parent);
	widget_indicator_create(parent);
	widget_warning_create(parent);
	/* s_widget_count stays 0 — no widget_t handles in this path */
}

/* ════════════════════════════════════════════════════════════════════════════
 *  dashboard_init
 * ════════════════════════════════════════════════════════════════════════════
 */
void dashboard_init(lv_obj_t *parent) {
	s_widget_count = 0;
	memset(s_widgets, 0, sizeof(s_widgets));

	/* 1 ── Mount LittleFS + generate default layout on first boot ───────── */
	esp_err_t err = layout_manager_init();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "layout_manager_init failed (%s) — using fallback",
				 esp_err_to_name(err));
		_fallback_create_all(parent);
		rebuild_can_dispatch();
		return;
	}

	/* 2 ── Retrieve the active layout name ─────────────────────────────── */
	char layout_name[LAYOUT_MAX_NAME];
	layout_manager_get_active(layout_name, sizeof(layout_name));
	ESP_LOGI(TAG, "Loading layout '%s'", layout_name);

	/* 3 ── Load layout: factory + create() for each widget entry ────────── */
	err = layout_manager_load(layout_name, parent);
	if (err != ESP_OK) {
		ESP_LOGW(TAG, "layout_manager_load('%s') failed (%s) — using fallback",
				 layout_name, esp_err_to_name(err));
		_fallback_create_all(parent);
		rebuild_can_dispatch();
		return;
	}

	/* Layout loaded successfully.  Collect widget handles from the registry
	 * that layout_manager_load() builds internally.
	 *
	 * NOTE: dashboard_get_widgets() returns an empty array for now because
	 * the Phase 3 layout manager does not yet have a centralised widget
	 * registry — each widget_t is created and orphaned. Phase 5 will add the
	 * registry. For Phase 4 the important result is that all LVGL objects
	 * are correctly created and positioned on @p parent. */
	ESP_LOGI(TAG, "Widget layer initialised via '%s'", layout_name);

	/* 5 ── Rebuild CAN dispatch table ──────────────────────────────────── */
	rebuild_can_dispatch();
}
