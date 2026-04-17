/* first_run_wizard.c - two-step onboarding: CAN scan, then WiFi options.
 *
 * Runs CAN bitrate scan inline (step 1), then shows connection options
 * (step 2). User can skip at any point - the same screens are reachable
 * from Device Settings later. NVS flag marks completion on Finish Setup
 * only; plain Skip lets the wizard return on the next boot. */

#include "first_run_wizard.h"

#include "esp_log.h"

#include "../theme.h"
#include "../../storage/config_store.h"
#include "../../can/can_bus_test.h"
#include "../../can/can_manager.h"
#include "../../net/wifi_manager.h"
#include "ui_wifi.h"

static const char *TAG = "first_run";

/* ── Layout constants ─────────────────────────────────────────────────── */
#define CARD_W   560
#define CARD_H   430
#define BTN_W    500
#define BTN_H     40

/* ── State ────────────────────────────────────────────────────────────── */
static lv_obj_t  *s_overlay    = NULL;
static lv_obj_t  *s_card       = NULL;

/* Step 1: CAN scan */
static lv_obj_t  *s_step1          = NULL;
static lv_obj_t  *s_scan_status    = NULL;
static lv_obj_t  *s_scan_progress  = NULL;
static lv_obj_t  *s_scan_bar      = NULL;
static lv_obj_t  *s_scan_results[4] = {NULL};
static lv_obj_t  *s_scan_detail   = NULL;
static lv_obj_t  *s_btn_apply     = NULL;
static lv_obj_t  *s_btn_next1     = NULL;
static lv_obj_t  *s_btn_cancel    = NULL;

/* Step 2: WiFi info */
static lv_obj_t  *s_step2         = NULL;

static const char *BR_NAMES[] = {"125 kbps", "250 kbps", "500 kbps", "1 Mbps"};

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Tear down the overlay. If mark_done is true, also set the NVS flag so the
 * wizard never shows again; otherwise it re-appears on the next boot. */
static void _close_wizard(bool mark_done) {
    if (mark_done) {
        config_store_save_first_run_done(true);
        ESP_LOGI(TAG, "First-run wizard completed (flag set)");
    } else {
        ESP_LOGI(TAG, "First-run wizard skipped (will show next boot)");
    }
    can_bus_test_set_ui_callback(NULL);
    if (can_bus_test_is_running()) can_bus_test_cancel();
    if (s_overlay && lv_obj_is_valid(s_overlay))
        lv_obj_del_async(s_overlay);
    s_overlay = s_card = s_step1 = s_step2 = NULL;
    s_scan_status = s_scan_progress = s_scan_bar = s_scan_detail = NULL;
    s_btn_apply = s_btn_next1 = s_btn_cancel = NULL;
    for (int i = 0; i < 4; i++) s_scan_results[i] = NULL;
}

static void _show_step2(void);

static lv_obj_t *_make_btn(lv_obj_t *parent, const char *text,
                           lv_color_t bg, lv_color_t fg,
                           bool border, lv_coord_t y,
                           lv_event_cb_t cb) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, lv_color_brightness(bg) == 0
                                    ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(btn, border ? 1 : 0, 0);
    lv_obj_set_style_border_color(btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, fg, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* ── CAN scan UI callbacks ────────────────────────────────────────────── */

static void _scan_ui_update(void) {
    if (!s_step1 || !lv_obj_is_valid(s_step1)) return;
    const can_scan_report_t *r = can_bus_test_get_report();

    switch (r->state) {
    case CAN_SCAN_STOPPING:
        lv_label_set_text(s_scan_status, "Stopping CAN for scan...");
        break;

    case CAN_SCAN_TESTING_BITRATE: {
        uint8_t idx = r->current_bitrate_idx;
        lv_label_set_text(s_scan_status, "Scanning for CAN traffic...");
        lv_label_set_text_fmt(s_scan_progress,
            "Testing %s  (%d of 4)", BR_NAMES[idx], idx + 1);
        lv_bar_set_value(s_scan_bar, idx * 25, LV_ANIM_ON);
        for (uint8_t i = 0; i < 4; i++) {
            if (i < idx) {
                if (r->results[i].traffic_detected)
                    lv_label_set_text_fmt(s_scan_results[i],
                        "%s  --  %lu frames", BR_NAMES[i],
                        (unsigned long)r->results[i].frames_received);
                else
                    lv_label_set_text_fmt(s_scan_results[i],
                        "%s  --  No traffic", BR_NAMES[i]);
                lv_obj_set_style_text_color(s_scan_results[i],
                    r->results[i].traffic_detected
                        ? THEME_COLOR_STATUS_CONNECTED : THEME_COLOR_TEXT_MUTED, 0);
            } else if (i == idx) {
                lv_label_set_text_fmt(s_scan_results[i],
                    "%s  --  Testing...", BR_NAMES[i]);
                lv_obj_set_style_text_color(s_scan_results[i],
                    THEME_COLOR_ACCENT_YELLOW, 0);
            }
        }
        break;
    }

    case CAN_SCAN_RESTORING:
        lv_bar_set_value(s_scan_bar, 95, LV_ANIM_ON);
        lv_label_set_text(s_scan_status, "Restoring CAN...");
        break;

    case CAN_SCAN_COMPLETE:
    case CAN_SCAN_CANCELLED:
        lv_bar_set_value(s_scan_bar, 100, LV_ANIM_ON);

        /* Final result labels */
        for (uint8_t i = 0; i < 4; i++) {
            if (r->results[i].traffic_detected) {
                lv_label_set_text_fmt(s_scan_results[i],
                    "%s  --  %lu frames", BR_NAMES[i],
                    (unsigned long)r->results[i].frames_received);
                lv_obj_set_style_text_color(s_scan_results[i],
                    THEME_COLOR_STATUS_CONNECTED, 0);
            } else {
                lv_label_set_text_fmt(s_scan_results[i],
                    "%s  --  No traffic", BR_NAMES[i]);
                lv_obj_set_style_text_color(s_scan_results[i],
                    THEME_COLOR_TEXT_MUTED, 0);
            }
        }

        if (r->recommended_bitrate >= 0) {
            uint8_t bi = (uint8_t)r->recommended_bitrate;
            lv_label_set_text_fmt(s_scan_status,
                "Detected CAN at %s", BR_NAMES[bi]);
            lv_obj_set_style_text_color(s_scan_status,
                THEME_COLOR_STATUS_CONNECTED, 0);
            lv_label_set_text_fmt(s_scan_detail,
                "%lu frames, %u unique IDs",
                (unsigned long)r->results[bi].frames_received,
                r->results[bi].unique_id_count);
            /* Show Apply button */
            lv_obj_t *lbl = lv_obj_get_child(s_btn_apply, 0);
            lv_label_set_text_fmt(lbl, "Apply %s & Continue", BR_NAMES[bi]);
            lv_obj_clear_flag(s_btn_apply, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_scan_status,
                "No CAN traffic detected");
            lv_obj_set_style_text_color(s_scan_status,
                THEME_COLOR_STATUS_ERROR, 0);
            lv_label_set_text(s_scan_detail,
                "Check wiring & ignition. You can re-scan from Device Settings.");
        }

        /* Hide cancel, show next */
        lv_obj_add_flag(s_btn_cancel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_next1, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_scan_progress, "");
        break;

    default:
        break;
    }
}

/* ── Button callbacks ─────────────────────────────────────────────────── */

static void _btn_cancel_scan_cb(lv_event_t *e) {
    (void)e;
    can_bus_test_cancel();
}

static void _btn_apply_cb(lv_event_t *e) {
    (void)e;
    const can_scan_report_t *r = can_bus_test_get_report();
    if (r->recommended_bitrate >= 0) {
        uint8_t idx = (uint8_t)r->recommended_bitrate;
        config_store_save_bitrate(idx);
        can_change_bitrate(idx);
        ESP_LOGI(TAG, "Applied bitrate %s", BR_NAMES[idx]);
    }
    _show_step2();
}

static void _btn_next1_cb(lv_event_t *e) {
    (void)e;
    _show_step2();
}

static void _btn_skip_cb(lv_event_t *e) {
    (void)e;
    /* Skip for now - wizard comes back on next boot */
    _close_wizard(false);
}

static void _btn_wifi_join_cb(lv_event_t *e) {
    (void)e;
    /* Jumping into WiFi setup - keep the wizard flag unset so the user
     * can return to it next boot if they abandon the WiFi screen. */
    _close_wizard(false);
    wifi_ui_show();
}

static void _btn_finish_cb(lv_event_t *e) {
    (void)e;
    /* Only path that permanently marks setup complete */
    _close_wizard(true);
}

/* ── Step 2: WiFi info ────────────────────────────────────────────────── */

static void _show_step2(void) {
    can_bus_test_set_ui_callback(NULL);

    /* Remove step 1 content */
    if (s_step1 && lv_obj_is_valid(s_step1))
        lv_obj_del(s_step1);
    s_step1 = NULL;

    /* Step 2 container */
    s_step2 = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_step2);
    lv_obj_set_size(s_step2, lv_pct(100), lv_pct(100));
    lv_obj_center(s_step2);
    lv_obj_clear_flag(s_step2, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(s_step2);
    lv_label_set_text(title, "Connect Your Device");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *sub = lv_label_create(s_step2);
    lv_label_set_text(sub, "Step 2 of 2  -  you have a few ways to connect");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_font(sub, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(sub, THEME_COLOR_TEXT_MUTED, 0);

    /* ── Option 1 (Recommended): Join WiFi network ─────────────────── */
    lv_obj_t *opt1_label = lv_label_create(s_step2);
    lv_label_set_text(opt1_label, "1.  WiFi  (recommended)");
    lv_obj_align(opt1_label, LV_ALIGN_TOP_LEFT, 30, 66);
    lv_obj_set_style_text_font(opt1_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(opt1_label, THEME_COLOR_ACCENT_BLUE, 0);

    lv_obj_t *opt1_sub = lv_label_create(s_step2);
    lv_label_set_text(opt1_sub,
        "Join your home/shop WiFi - dash appears at http://rdm7.local");
    lv_obj_align(opt1_sub, LV_ALIGN_TOP_LEFT, 30, 86);
    lv_obj_set_style_text_font(opt1_sub, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(opt1_sub, THEME_COLOR_TEXT_MUTED, 0);

    _make_btn(s_step2, "Join a WiFi Network",
              THEME_COLOR_ACCENT_BLUE, THEME_COLOR_TEXT_ON_ACCENT,
              false, 108, _btn_wifi_join_cb);

    /* ── Option 2: USB ──────────────────────────────────────────────── */
    lv_obj_t *opt2_label = lv_label_create(s_step2);
    lv_label_set_text(opt2_label, "2.  USB");
    lv_obj_align(opt2_label, LV_ALIGN_TOP_LEFT, 30, 164);
    lv_obj_set_style_text_font(opt2_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(opt2_label, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *opt2_sub = lv_label_create(s_step2);
    lv_label_set_long_mode(opt2_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(opt2_sub, BTN_W - 30);
    lv_label_set_text(opt2_sub,
        "Plug the UART USB port into your laptop - the RDM Desktop Studio app "
        "will detect the device automatically. No setup required.");
    lv_obj_align(opt2_sub, LV_ALIGN_TOP_LEFT, 30, 184);
    lv_obj_set_style_text_font(opt2_sub, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(opt2_sub, THEME_COLOR_TEXT_MUTED, 0);

    /* ── Option 3: Hotspot fallback ─────────────────────────────────── */
    const char *ap_ssid = wifi_manager_get_ap_ssid();
    const char *ap_ip   = wifi_manager_get_ap_ip();

    lv_obj_t *opt3_label = lv_label_create(s_step2);
    lv_label_set_text(opt3_label, "3.  Hotspot  (fallback, no WiFi needed)");
    lv_obj_align(opt3_label, LV_ALIGN_TOP_LEFT, 30, 230);
    lv_obj_set_style_text_font(opt3_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(opt3_label, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *opt3_sub = lv_label_create(s_step2);
    lv_label_set_long_mode(opt3_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(opt3_sub, BTN_W - 30);
    lv_label_set_text_fmt(opt3_sub,
        "Connect to \"%s\"  /  password: rdm7dash\n"
        "Then open http://rdm7.local  or  http://%s",
        ap_ssid ? ap_ssid : "RDM7-????",
        ap_ip   ? ap_ip   : "192.168.4.1");
    lv_obj_align(opt3_sub, LV_ALIGN_TOP_LEFT, 30, 250);
    lv_obj_set_style_text_font(opt3_sub, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(opt3_sub, THEME_COLOR_TEXT_MUTED, 0);

    /* Finish button */
    _make_btn(s_step2, "Finish Setup",
              lv_color_black(), THEME_COLOR_TEXT_MUTED,
              false, 296, _btn_finish_cb);
}

/* ── Step 1: CAN scan ─────────────────────────────────────────────────── */

static void _build_step1(void) {
    s_step1 = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_step1);
    lv_obj_set_size(s_step1, lv_pct(100), lv_pct(100));
    lv_obj_center(s_step1);
    lv_obj_clear_flag(s_step1, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(s_step1);
    lv_label_set_text(title, "CAN Bus Setup");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *sub = lv_label_create(s_step1);
    lv_label_set_text(sub, "Step 1 of 2  -  Scanning all bitrates...");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_font(sub, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(sub, THEME_COLOR_TEXT_MUTED, 0);

    /* Status label */
    s_scan_status = lv_label_create(s_step1);
    lv_label_set_text(s_scan_status, "Starting scan...");
    lv_obj_align(s_scan_status, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_text_font(s_scan_status, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_scan_status, THEME_COLOR_TEXT_PRIMARY, 0);

    /* Progress bar */
    s_scan_bar = lv_bar_create(s_step1);
    lv_obj_set_size(s_scan_bar, BTN_W, 8);
    lv_obj_align(s_scan_bar, LV_ALIGN_TOP_MID, 0, 86);
    lv_bar_set_range(s_scan_bar, 0, 100);
    lv_bar_set_value(s_scan_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_scan_bar, THEME_COLOR_SECTION_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_scan_bar, THEME_COLOR_ACCENT_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_scan_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_scan_bar, 4, LV_PART_INDICATOR);

    /* Progress text */
    s_scan_progress = lv_label_create(s_step1);
    lv_label_set_text(s_scan_progress, "");
    lv_obj_align(s_scan_progress, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_text_font(s_scan_progress, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_scan_progress, THEME_COLOR_TEXT_MUTED, 0);

    /* Per-bitrate result lines */
    for (int i = 0; i < 4; i++) {
        s_scan_results[i] = lv_label_create(s_step1);
        lv_label_set_text_fmt(s_scan_results[i], "%s  --  ...", BR_NAMES[i]);
        lv_obj_align(s_scan_results[i], LV_ALIGN_TOP_LEFT, 20, 122 + i * 22);
        lv_obj_set_style_text_font(s_scan_results[i], THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(s_scan_results[i], THEME_COLOR_TEXT_MUTED, 0);
    }

    /* Detail label */
    s_scan_detail = lv_label_create(s_step1);
    lv_label_set_text(s_scan_detail, "");
    lv_label_set_long_mode(s_scan_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_scan_detail, lv_pct(90));
    lv_obj_align(s_scan_detail, LV_ALIGN_TOP_MID, 0, 216);
    lv_obj_set_style_text_font(s_scan_detail, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_scan_detail, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(s_scan_detail, LV_TEXT_ALIGN_CENTER, 0);

    /* Apply button (hidden until scan completes with a result) */
    s_btn_apply = _make_btn(s_step1, "Apply & Continue",
                            THEME_COLOR_ACCENT_BLUE, THEME_COLOR_TEXT_ON_ACCENT,
                            false, 248, _btn_apply_cb);
    lv_obj_add_flag(s_btn_apply, LV_OBJ_FLAG_HIDDEN);

    /* Cancel scan button */
    s_btn_cancel = _make_btn(s_step1, "Cancel Scan",
                             THEME_COLOR_SECTION_BG, THEME_COLOR_TEXT_MUTED,
                             true, 296, _btn_cancel_scan_cb);

    /* Next (skip CAN) button - hidden until scan finishes */
    s_btn_next1 = _make_btn(s_step1, "Continue without CAN",
                            THEME_COLOR_SECTION_BG, THEME_COLOR_TEXT_MUTED,
                            true, 296, _btn_next1_cb);
    lv_obj_add_flag(s_btn_next1, LV_OBJ_FLAG_HIDDEN);

    /* Skip (dismiss entire wizard) */
    _make_btn(s_step1, "Skip for now  (ask again next boot)",
              lv_color_black(), THEME_COLOR_TEXT_MUTED,
              false, 340, _btn_skip_cb);
}

/* ── Public entry point ───────────────────────────────────────────────── */

void show_first_run_wizard(void) {
    if (s_overlay && lv_obj_is_valid(s_overlay)) return;

    /* Full-screen translucent overlay */
    lv_obj_t *scr = lv_scr_act();
    s_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_overlay);

    /* Card */
    s_card = lv_obj_create(s_overlay);
    lv_obj_set_size(s_card, CARD_W, CARD_H);
    lv_obj_center(s_card);
    lv_obj_set_style_bg_color(s_card, THEME_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_radius(s_card, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(s_card, 20, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);

    /* Build step 1 (CAN scan) and auto-start */
    _build_step1();
    can_bus_test_set_ui_callback(_scan_ui_update);
    can_bus_test_start();

    ESP_LOGI(TAG, "First-run wizard shown, CAN scan started");
}
