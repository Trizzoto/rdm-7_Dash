/*
 * ui_diagnostics.c — System diagnostics screen.
 *
 * Read-only at-a-glance view of CAN bus, SD card, WiFi, signals and system
 * health. Auto-refreshes every 1s so you can watch counters tick.
 *
 * Layout: 2-column grid of "cards", each card = title + label rows. Auto-fits
 * the 800x480 panel; scrollable if content overflows on smaller screen sizes.
 *
 * Threading: refresh callback runs on the LVGL task (lv_timer), so it can call
 * any LVGL or signal/wifi/sd API directly without locking.
 */
#include "ui_diagnostics.h"
#include "../theme.h"
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

/* ── Theme helpers (mirrors style used by ui_wifi.c) ─────────────────────── */

static void _style_card(lv_obj_t *card)
{
	lv_obj_set_style_bg_color(card, THEME_COLOR_SURFACE, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(card, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(card, 1, 0);
	lv_obj_set_style_radius(card, THEME_RADIUS_NORMAL, 0);
	lv_obj_set_style_pad_all(card, 10, 0);
	lv_obj_set_style_pad_gap(card, 4, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
}

static void _style_card_title(lv_obj_t *lbl, lv_color_t accent)
{
	lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
	lv_obj_set_style_text_color(lbl, accent, 0);
	lv_obj_set_style_text_letter_space(lbl, 1, 0);
	lv_obj_set_style_pad_bottom(lbl, 4, 0);
}

/* Add a "Label: value" row to a card and register the value label so the
 * refresh callback can update it. Returns the value label so the caller can
 * also style it (e.g. red for error states). */
static lv_obj_t *_add_kv(lv_obj_t *parent, const char *name)
{
	lv_obj_t *row = lv_obj_create(parent);
	lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *name_lbl = lv_label_create(row);
	lv_label_set_text(name_lbl, name);
	lv_obj_set_style_text_font(name_lbl, THEME_FONT_TINY, 0);
	lv_obj_set_style_text_color(name_lbl, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);

	lv_obj_t *val_lbl = lv_label_create(row);
	lv_label_set_text(val_lbl, "-");
	lv_obj_set_style_text_font(val_lbl, THEME_FONT_TINY, 0);
	lv_obj_set_style_text_color(val_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_align(val_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

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
		case WIFI_MGR_STATE_AP_ONLY:    return "AP Only";
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
		lv_label_set_text(_kv("State"), "(driver not init)");
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

/* ── Build / teardown ────────────────────────────────────────────────────── */

static lv_obj_t *_make_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h,
                             const char *title, lv_color_t accent)
{
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_size(card, w, h);
	_style_card(card);
	lv_obj_t *t = lv_label_create(card);
	lv_label_set_text(t, title);
	_style_card_title(t, accent);
	return card;
}

static void _create(void)
{
	s_kv_count = 0;

	s_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(s_screen, THEME_COLOR_BG, 0);
	lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

	/* Header — Back / Title / Refresh */
	lv_obj_t *header = lv_obj_create(s_screen);
	lv_obj_set_size(header, SCREEN_W, 44);
	lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(header, THEME_COLOR_SURFACE, 0);
	lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(header, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_width(header, 1, 0);
	lv_obj_set_style_radius(header, 0, 0);
	lv_obj_set_style_pad_hor(header, 10, 0);
	lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *back_btn = lv_btn_create(header);
	lv_obj_set_size(back_btn, 80, 30);
	lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_style_bg_color(back_btn, THEME_COLOR_SECTION_BG, 0);
	lv_obj_set_style_border_color(back_btn, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(back_btn, 1, 0);
	lv_obj_set_style_radius(back_btn, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_shadow_width(back_btn, 0, 0);
	lv_obj_add_event_cb(back_btn, _back_btn_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *back_lbl = lv_label_create(back_btn);
	lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
	lv_obj_set_style_text_font(back_lbl, THEME_FONT_SMALL, 0);
	lv_obj_set_style_text_color(back_lbl, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_center(back_lbl);

	lv_obj_t *title = lv_label_create(header);
	lv_label_set_text(title, "System Diagnostics");
	lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
	lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

	lv_obj_t *refresh_btn = lv_btn_create(header);
	lv_obj_set_size(refresh_btn, 80, 30);
	lv_obj_align(refresh_btn, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_bg_color(refresh_btn, THEME_COLOR_SECTION_BG, 0);
	lv_obj_set_style_border_color(refresh_btn, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(refresh_btn, 1, 0);
	lv_obj_set_style_radius(refresh_btn, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_shadow_width(refresh_btn, 0, 0);
	lv_obj_add_event_cb(refresh_btn, _refresh_btn_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *refresh_lbl = lv_label_create(refresh_btn);
	lv_label_set_text(refresh_lbl, LV_SYMBOL_REFRESH " Now");
	lv_obj_set_style_text_font(refresh_lbl, THEME_FONT_SMALL, 0);
	lv_obj_set_style_text_color(refresh_lbl, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_center(refresh_lbl);

	/* Body — 3-column flex-wrap grid. Cards size to their content height so
	 * SD/Signals (3 rows) sit shorter than CAN/WiFi/System (5-6 rows), which
	 * lets the whole thing fit in 800x480 without scrolling. The body is
	 * scrollable as a safety net if a future card grows beyond what fits.
	 *
	 * Math at this resolution:
	 *   body height = 480 - 44 (header) = 436
	 *   3-col card width = (800 - 8 pad - 8 pad - 6 gap*2) / 3 ≈ 260
	 *   2 rows of ~140-180px tall + 6px gap easily fits ≤ 366. */
	lv_obj_t *body = lv_obj_create(s_screen);
	lv_obj_set_size(body, SCREEN_W, SCREEN_H - 44);
	lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 44);
	lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, 8, 0);
	lv_obj_set_style_pad_gap(body, 6, 0);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW_WRAP);
	/* Scrollable as a safety net — content currently fits, but future cards
	 * could push beyond 480. Vertical scroll only; horizontal disabled. */
	lv_obj_set_scroll_dir(body, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);
	lv_obj_set_style_bg_color(body, THEME_COLOR_SCROLLBAR, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(body, LV_OPA_50, LV_PART_SCROLLBAR);
	lv_obj_set_style_radius(body, 2, LV_PART_SCROLLBAR);
	lv_obj_set_style_width(body, 4, LV_PART_SCROLLBAR);

	/* All cards share the same fixed footprint for a uniform grid look.
	 *   width  = (SCREEN_W - 16 padding - 12 gap) / 3   ≈ 257
	 *   height = sized for the 6-row max (CAN, SYSTEM); shorter cards leave
	 *            empty space at the bottom rather than mismatched heights.
	 *
	 * Math: pad_all(10) + title(~16) + pad_gap(4) + 6 rows × (16+4 gap)
	 *       + pad_all(10) - 4 (last gap) = 28 + 120 - 4 = ~144. Round to
	 *       150 for a little breathing room. */
	const lv_coord_t CARD_W = (SCREEN_W - 16 - 12) / 3;
	const lv_coord_t CARD_H = 150;

	/* CAN BUS */
	lv_obj_t *can_card = _make_card(body, CARD_W, CARD_H,
	                                  "CAN BUS", THEME_COLOR_ACCENT_BLUE);
	_add_kv(can_card, "State");
	_add_kv(can_card, "Pending RX");
	_add_kv(can_card, "TX errors");
	_add_kv(can_card, "RX errors");
	_add_kv(can_card, "Bus errors");
	_add_kv(can_card, "RX missed");

	/* WiFi */
	lv_obj_t *wf_card = _make_card(body, CARD_W, CARD_H,
	                                 "WI-FI", THEME_COLOR_ACCENT_BLUE);
	_add_kv(wf_card, "WiFi");
	_add_kv(wf_card, "SSID");
	_add_kv(wf_card, "STA IP");
	_add_kv(wf_card, "AP");
	_add_kv(wf_card, "AP IP");

	/* SYSTEM — 5 KV rows */
	lv_obj_t *sys_card = _make_card(body, CARD_W, CARD_H,
	                                  "SYSTEM", THEME_COLOR_ACCENT_BLUE);
	_add_kv(sys_card, "Uptime");
	_add_kv(sys_card, "Free heap");
	_add_kv(sys_card, "Free PSRAM");
	_add_kv(sys_card, "Logger");
	_add_kv(sys_card, "Replay");

	/* SD CARD */
	lv_obj_t *sd_card = _make_card(body, CARD_W, CARD_H,
	                                 "SD CARD", THEME_COLOR_ACCENT_AMBER);
	_add_kv(sd_card, "SD");
	_add_kv(sd_card, "Usage");
	_add_kv(sd_card, "Free");

	/* SIGNALS */
	lv_obj_t *sig_card = _make_card(body, CARD_W, CARD_H,
	                                  "SIGNALS", THEME_COLOR_ACCENT_AMBER);
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
