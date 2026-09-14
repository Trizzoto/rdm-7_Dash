/*
 * ui_diagnostics.c — System diagnostics screen.
 *
 * Read-only at-a-glance view of CAN bus, SD card, WiFi, signals and system
 * health. Auto-refreshes every 1s so you can watch counters tick.
 *
 * Layout: built from ui_kit parts (ADR-0071) — brand bar, then a 3x2 grid of
 * cards, each card = caps section label + key/value rows. CAN has the most
 * rows, so it takes the whole left column; the other four share the right two
 * columns. Everything fits the 800x480 panel without scrolling.
 *
 * Threading: refresh callback runs on the LVGL task (lv_timer), so it can call
 * any LVGL or signal/wifi/sd API directly without locking.
 */
#include "ui_diagnostics.h"
#include "ui_can_list.h"
#include "../theme.h"
#include "kit/ui_kit.h"
#include "screen_config.h"
#include "net/wifi_manager.h"
#include "storage/sd_manager.h"
#include "storage/config_store.h"
#include "storage/data_logger.h"
#include "storage/signal_replay.h"
#include "widgets/signal.h"
#include "layout/layout_manager.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/twai.h"
#include <stdio.h>
#include <string.h>

#define REFRESH_PERIOD_MS  1000

static lv_obj_t *s_screen          = NULL;
static lv_obj_t *s_return_screen   = NULL;
static lv_timer_t *s_refresh_timer = NULL;

/* Value labels we update on each refresh — held in arrays so the refresh
 * callback can iterate without naming each individually. */
typedef struct {
	const char *name;
	lv_obj_t   *value;
} kv_label_t;

#define MAX_KV  48
static kv_label_t s_kvs[MAX_KV];
static uint8_t    s_kv_count = 0;

/* ── Card helpers ────────────────────────────────────────────────────────── */

/* Add a "Label: value" row to a card and register the value label so the
 * refresh callback can update it. Returns the value label so the caller can
 * also style it (e.g. red for error states). The row is a kit row made a
 * little tighter, so five rows fit a half-height card; long values (an SSID,
 * the replay position) end in dots rather than running under the key. */
static lv_obj_t *_add_kv(lv_obj_t *parent, const char *name)
{
	lv_obj_t *val_lbl = uk_row(parent, name, "-");
	lv_obj_set_style_pad_ver(lv_obj_get_parent(val_lbl), 4, 0);
	lv_obj_set_flex_grow(val_lbl, 1);
	lv_label_set_long_mode(val_lbl, LV_LABEL_LONG_DOT);

	if (s_kv_count < MAX_KV) {
		s_kvs[s_kv_count].name  = name;
		s_kvs[s_kv_count].value = val_lbl;
		s_kv_count++;
	}
	return val_lbl;
}

/* Find a value label by name. Linear scan, but the table is tiny. */
static lv_obj_t *_kv(const char *name)
{
	for (uint8_t i = 0; i < s_kv_count; i++) {
		if (strcmp(s_kvs[i].name, name) == 0) return s_kvs[i].value;
	}
	return NULL;
}

/* ── Data fetching helpers ───────────────────────────────────────────────── */

static const char *_wifi_state_str(wifi_mgr_state_t s)
{
	switch (s) {
		case WIFI_MGR_STATE_OFF:        return "Off";
		case WIFI_MGR_STATE_IDLE:       return "Idle";
		case WIFI_MGR_STATE_SCANNING:   return "Scanning";
		case WIFI_MGR_STATE_CONNECTING: return "Connecting";
		case WIFI_MGR_STATE_CONNECTED:  return "Connected";
		case WIFI_MGR_STATE_AP_ONLY:    return "Hotspot only";
		case WIFI_MGR_STATE_FAILED:     return "Failed";
		default:                         return "?";
	}
}

static const char *_twai_state_str(twai_state_t s)
{
	switch (s) {
		case TWAI_STATE_STOPPED:     return "Stopped";
		case TWAI_STATE_RUNNING:     return "Running";
		case TWAI_STATE_BUS_OFF:     return "Bus Off";
		case TWAI_STATE_RECOVERING:  return "Recovering";
		default:                      return "?";
	}
}

static void _format_bytes(size_t bytes, char *out, size_t outsz)
{
	if (bytes >= 1024UL * 1024UL)
		snprintf(out, outsz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
	else if (bytes >= 1024UL)
		snprintf(out, outsz, "%.1f KB", (double)bytes / 1024.0);
	else
		snprintf(out, outsz, "%u B", (unsigned)bytes);
}

static void _format_uptime(uint64_t us, char *out, size_t outsz)
{
	uint32_t sec_total = (uint32_t)(us / 1000000ULL);
	uint32_t hours     = sec_total / 3600;
	uint32_t mins      = (sec_total % 3600) / 60;
	uint32_t secs      = sec_total % 60;
	snprintf(out, outsz, "%uh %um %us",
	         (unsigned)hours, (unsigned)mins, (unsigned)secs);
}

/* ── Refresh ─────────────────────────────────────────────────────────────── */

static void _refresh(lv_timer_t *t)
{
	(void)t;
	if (!s_screen) return;

	char buf[64];

	/* ── CAN ── */
	twai_status_info_t can;
	if (twai_get_status_info(&can) == ESP_OK) {
		lv_label_set_text(_kv("State"), _twai_state_str(can.state));
		lv_obj_set_style_text_color(_kv("State"),
		    can.state == TWAI_STATE_RUNNING ? THEME_COLOR_STATUS_CONNECTED :
		    can.state == TWAI_STATE_BUS_OFF ? THEME_COLOR_STATUS_ERROR :
		                                       THEME_COLOR_TEXT_MUTED, 0);
		snprintf(buf, sizeof(buf), "%lu", (unsigned long)can.msgs_to_rx);
		lv_label_set_text(_kv("Pending RX"), buf);
		snprintf(buf, sizeof(buf), "%lu", (unsigned long)can.tx_error_counter);
		lv_label_set_text(_kv("TX errors"), buf);
		snprintf(buf, sizeof(buf), "%lu", (unsigned long)can.rx_error_counter);
		lv_label_set_text(_kv("RX errors"), buf);
		snprintf(buf, sizeof(buf), "%lu", (unsigned long)can.bus_error_count);
		lv_label_set_text(_kv("Bus errors"), buf);
		snprintf(buf, sizeof(buf), "%lu", (unsigned long)can.rx_missed_count);
		lv_label_set_text(_kv("RX missed"), buf);
	} else {
		lv_label_set_text(_kv("State"), "Driver not started");
	}

	/* ── SD ── */
	if (sd_manager_is_mounted()) {
		lv_label_set_text(_kv("SD"), "Mounted");
		lv_obj_set_style_text_color(_kv("SD"), THEME_COLOR_STATUS_CONNECTED, 0);
		size_t total = 0, used = 0, freeb = 0;
		if (sd_manager_get_info(&total, &used, &freeb) == ESP_OK) {
			char a[32], b[32];
			_format_bytes(used, a, sizeof(a));
			_format_bytes(total, b, sizeof(b));
			snprintf(buf, sizeof(buf), "%s / %s", a, b);
			lv_label_set_text(_kv("Usage"), buf);
			_format_bytes(freeb, buf, sizeof(buf));
			lv_label_set_text(_kv("Free"), buf);
		}
	} else {
		lv_label_set_text(_kv("SD"), "Not mounted");
		lv_obj_set_style_text_color(_kv("SD"), THEME_COLOR_TEXT_MUTED, 0);
		lv_label_set_text(_kv("Usage"), "-");
		lv_label_set_text(_kv("Free"), "-");
	}

	/* ── WiFi ── */
	wifi_mgr_state_t wstate = wifi_manager_get_state();
	lv_label_set_text(_kv("WiFi"), _wifi_state_str(wstate));
	lv_obj_set_style_text_color(_kv("WiFi"),
	    wstate == WIFI_MGR_STATE_CONNECTED ? THEME_COLOR_STATUS_CONNECTED :
	    wstate == WIFI_MGR_STATE_FAILED    ? THEME_COLOR_STATUS_ERROR :
	                                          THEME_COLOR_TEXT_MUTED, 0);
	const char *ssid = wifi_manager_get_connected_ssid();
	lv_label_set_text(_kv("SSID"), (ssid && ssid[0]) ? ssid : "-");
	const char *ip = wifi_manager_get_sta_ip();
	lv_label_set_text(_kv("STA IP"), (ip && ip[0]) ? ip : "-");
	lv_label_set_text(_kv("AP"),
	                  wifi_manager_is_ap_enabled() ? "Enabled" : "Disabled");
	const char *apip = wifi_manager_get_ap_ip();
	lv_label_set_text(_kv("AP IP"), (apip && apip[0]) ? apip : "-");

	/* ── Signals ── */
	uint16_t total_sigs = signal_get_count();
	uint16_t fresh = 0;
	uint16_t stale = 0;
	for (uint16_t i = 0; i < total_sigs; i++) {
		signal_t *sig = signal_get_by_index(i);
		if (!sig) continue;
		if (sig->is_stale) stale++;
		else               fresh++;
	}
	snprintf(buf, sizeof(buf), "%u", (unsigned)total_sigs);
	lv_label_set_text(_kv("Total"), buf);
	snprintf(buf, sizeof(buf), "%u", (unsigned)fresh);
	lv_label_set_text(_kv("Fresh"), buf);
	snprintf(buf, sizeof(buf), "%u", (unsigned)stale);
	lv_label_set_text(_kv("Stale"), buf);
	lv_obj_set_style_text_color(_kv("Stale"),
	    stale > 0 ? THEME_COLOR_STATUS_ERROR : THEME_COLOR_TEXT_PRIMARY, 0);

	/* ── System ── */
	_format_uptime(esp_timer_get_time(), buf, sizeof(buf));
	lv_label_set_text(_kv("Uptime"), buf);
	_format_bytes(heap_caps_get_free_size(MALLOC_CAP_INTERNAL), buf, sizeof(buf));
	lv_label_set_text(_kv("Free heap"), buf);
	_format_bytes(heap_caps_get_free_size(MALLOC_CAP_SPIRAM), buf, sizeof(buf));
	lv_label_set_text(_kv("Free PSRAM"), buf);

	/* Data logger status */
	if (data_logger_is_active()) {
		uint32_t samples = data_logger_get_sample_count();
		uint16_t hz      = data_logger_get_rate_hz();
		if (hz == 0) {
			snprintf(buf, sizeof(buf), "Recording (%lu, Max)",
			         (unsigned long)samples);
		} else {
			snprintf(buf, sizeof(buf), "Recording (%lu, %u Hz)",
			         (unsigned long)samples, (unsigned)hz);
		}
		lv_label_set_text(_kv("Logger"), buf);
		lv_obj_set_style_text_color(_kv("Logger"),
		                             THEME_COLOR_STATUS_CONNECTED, 0);
	} else {
		lv_label_set_text(_kv("Logger"), "Idle");
		lv_obj_set_style_text_color(_kv("Logger"),
		                             THEME_COLOR_TEXT_MUTED, 0);
	}

	/* Replay status — only render text if active so the label stays unobtrusive */
	if (signal_replay_is_active()) {
		uint32_t row   = signal_replay_get_row();
		uint32_t total = signal_replay_get_total_rows();
		float    speed = signal_replay_get_speed();
		snprintf(buf, sizeof(buf), "Playing %lu/%lu @ %.1fx",
		         (unsigned long)row, (unsigned long)total, (double)speed);
		lv_label_set_text(_kv("Replay"), buf);
		lv_obj_set_style_text_color(_kv("Replay"),
		                             THEME_COLOR_STATUS_CONNECTED, 0);
	} else {
		lv_label_set_text(_kv("Replay"), "Idle");
		lv_obj_set_style_text_color(_kv("Replay"),
		                             THEME_COLOR_TEXT_MUTED, 0);
	}
}

/* ── Event handlers ──────────────────────────────────────────────────────── */

static void _back_btn_cb(lv_event_t *e)
{
	(void)e;
	diagnostics_ui_hide();
}

static void _refresh_btn_cb(lv_event_t *e)
{
	(void)e;
	_refresh(NULL);
}

static void _can_ids_btn_cb(lv_event_t *e)
{
	(void)e;
	can_list_ui_show();
}

/* ── Build / teardown ────────────────────────────────────────────────────── */

/* A screen-wide action in the brand bar, left of Back. uk_bar's status strip
 * (its child 2) is the only slot there; the kit has no uk_bar_action() yet. */
static lv_obj_t *_bar_action(lv_obj_t *bar, uk_icon_t icon, const char *text,
                             lv_event_cb_t cb)
{
	lv_obj_t *strip = lv_obj_get_child(bar, 2);
	lv_obj_t *b = uk_btn(strip ? strip : bar, icon, text, UK_BTN_NEUTRAL, cb, NULL);
	lv_obj_set_height(b, 36);
	lv_obj_set_ext_click_area(b, 6);
	return b;
}

static lv_obj_t *_make_card(lv_obj_t *grid, uint8_t col, uint8_t row,
                            uint8_t row_span, const char *title)
{
	lv_obj_t *card = uk_card(grid);
	uk_grid_place(card, col, row, 1, row_span);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	/* Rows carry their own hairlines; the gap goes under the title only. */
	lv_obj_set_style_pad_row(card, 0, 0);
	lv_obj_t *t = uk_section(card, title);
	lv_obj_set_style_pad_bottom(t, 6, 0);
	return card;
}

static void _create(void)
{
	s_kv_count = 0;

	s_screen = uk_screen();

	/* Brand bar — title, Live CAN IDs, Refresh, Back */
	lv_obj_t *bar = uk_bar(s_screen, "Diagnostics", UK_BAR_BACK, _back_btn_cb, NULL);
	/* "Live CAN IDs" opens the live per-ID list screen. */
	_bar_action(bar, UK_ICON_CAN, "Live CAN IDs", _can_ids_btn_cb);
	_bar_action(bar, UK_ICON_NONE, "Refresh", _refresh_btn_cb);

	/* Body — 3 columns x 2 rows. The CAN column is a little narrower (its
	 * values are short counters) and spans both rows for its six readings;
	 * WiFi / System take the top of the other two columns and SD / Signals
	 * the bottom. Math at this resolution:
	 *   body content = 776 x 404, gaps 10  -> rows 197 tall
	 *   five kit rows at 4 px padding (~25 each) + title + card padding
	 *   = ~183, so the half-height cards fit without scrolling. */
	static const lv_coord_t col_dsc[] = { LV_GRID_FR(4), LV_GRID_FR(5), LV_GRID_FR(5),
	                                      LV_GRID_TEMPLATE_LAST };
	static const lv_coord_t row_dsc[] = { LV_GRID_FR(1), LV_GRID_FR(1),
	                                      LV_GRID_TEMPLATE_LAST };
	lv_obj_t *grid = uk_grid(uk_body(s_screen), 3, 2);
	lv_obj_set_grid_dsc_array(grid, col_dsc, row_dsc);

	/* CAN BUS */
	lv_obj_t *can_card = _make_card(grid, 0, 0, 2, "CAN bus");
	_add_kv(can_card, "State");
	_add_kv(can_card, "Pending RX");
	_add_kv(can_card, "TX errors");
	_add_kv(can_card, "RX errors");
	_add_kv(can_card, "Bus errors");
	_add_kv(can_card, "RX missed");

	/* WiFi */
	lv_obj_t *wf_card = _make_card(grid, 1, 0, 1, "WiFi");
	_add_kv(wf_card, "WiFi");
	_add_kv(wf_card, "SSID");
	_add_kv(wf_card, "STA IP");
	_add_kv(wf_card, "AP");
	_add_kv(wf_card, "AP IP");

	/* SYSTEM — 5 KV rows */
	lv_obj_t *sys_card = _make_card(grid, 2, 0, 1, "System");
	_add_kv(sys_card, "Uptime");
	_add_kv(sys_card, "Free heap");
	_add_kv(sys_card, "Free PSRAM");
	_add_kv(sys_card, "Logger");
	_add_kv(sys_card, "Replay");

	/* SD CARD */
	lv_obj_t *sd_card = _make_card(grid, 1, 1, 1, "SD card");
	_add_kv(sd_card, "SD");
	_add_kv(sd_card, "Usage");
	_add_kv(sd_card, "Free");

	/* SIGNALS */
	lv_obj_t *sig_card = _make_card(grid, 2, 1, 1, "Signals");
	_add_kv(sig_card, "Total");
	_add_kv(sig_card, "Fresh");
	_add_kv(sig_card, "Stale");

	/* Initial paint so the screen is populated before the timer ticks */
	_refresh(NULL);

	/* Auto-refresh timer */
	if (s_refresh_timer) {
		lv_timer_del(s_refresh_timer);
	}
	s_refresh_timer = lv_timer_create(_refresh, REFRESH_PERIOD_MS, NULL);
}

static void _destroy(void)
{
	if (s_refresh_timer) {
		lv_timer_del(s_refresh_timer);
		s_refresh_timer = NULL;
	}
	if (s_screen) {
		lv_obj_del(s_screen);
		s_screen = NULL;
	}
	s_kv_count = 0;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void diagnostics_ui_show(void)
{
	if (s_screen) return;
	s_return_screen = lv_scr_act();
	_create();
	lv_scr_load(s_screen);
}

void diagnostics_ui_hide(void)
{
	if (!s_screen) return;
	/* Load return screen BEFORE destroying current — LVGL v8 crashes if you
	 * delete the active screen. */
	lv_obj_t *ret = s_return_screen;
	s_return_screen = NULL;
	if (ret && lv_obj_is_valid(ret)) {
		lv_scr_load(ret);
	}
	_destroy();
}

bool diagnostics_ui_is_active(void)
{
	return s_screen != NULL;
}
