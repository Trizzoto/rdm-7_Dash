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
#include "ui_ecu_picker.h"
#include "ui_Screen3.h"

static const char *TAG = "first_run";

/* ── Layout constants ─────────────────────────────────────────────────── */
#define CARD_W   560
#define CARD_H   470   /* Bumped from 430 to fit the Skip-for-good button in step 1 */
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
static lv_obj_t  *s_btn_start     = NULL;

/* Auto-retry scan-start: when the user re-runs the wizard from Device
 * Settings just after closing/skipping a previous instance, the scan task
 * may still be in its 50–500 ms cleanup window. Polling for ~3 s gives the
 * old scan time to fully exit before we declare a real failure. */
static lv_timer_t *s_start_retry_timer = NULL;
static uint8_t     s_start_retry_count = 0;
#define START_RETRY_PERIOD_MS  300
#define START_RETRY_MAX        10   /* 10 × 300 ms = 3 s budget */

/* Step 3: WiFi info (step 2 is the ECU picker - handled by ui_ecu_picker) */
static lv_obj_t  *s_step3         = NULL;

/* Set when the ECU picker applies a preset; triggers a dashboard reload
 * when the wizard finishes. Reloading inline would destroy the wizard card. */
static bool       s_ecu_applied   = false;

/* WiFi return flow: when the user taps "Join a WiFi Network" we hide
 * (but keep alive) the wizard overlay + show ui_wifi. A 200 ms polling
 * timer watches wifi_ui_is_active(); once it flips false we re-reveal
 * the overlay so the user lands back on step 3. */
static bool       s_wifi_return_pending = false;
static lv_timer_t *s_wifi_return_timer  = NULL;

static const char *BR_NAMES[] = {"125 kbps", "250 kbps", "500 kbps", "1 Mbps"};

/* ── Helpers ──────────────────────────────────────────────────────────── */

/* Runs on the LVGL async queue. Deletes the wizard overlay (if still
 * around) and rebuilds ui_Screen3 so the new ECU signals take effect.
 * Done in a single callback to avoid any queue-ordering surprises
 * between lv_obj_del_async and lv_async_call. */
static void _deferred_reload_after_wizard(void *arg) {
    lv_obj_t *overlay = (lv_obj_t *)arg;
    if (overlay && lv_obj_is_valid(overlay)) {
        lv_obj_del(overlay);
    }
    lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
    ui_Screen3_screen_init();
    lv_scr_load(ui_Screen3);
    if (old && old != ui_Screen3 && lv_obj_is_valid(old))
        lv_obj_del(old);
}

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
    if (s_wifi_return_timer) {
        lv_timer_del(s_wifi_return_timer);
        s_wifi_return_timer = NULL;
    }
    if (s_start_retry_timer) {
        lv_timer_del(s_start_retry_timer);
        s_start_retry_timer = NULL;
    }
    s_start_retry_count = 0;
    s_wifi_return_pending = false;
    if (s_overlay && lv_obj_is_valid(s_overlay))
        lv_obj_del_async(s_overlay);
    s_overlay = s_card = s_step1 = s_step3 = NULL;
    s_scan_status = s_scan_progress = s_scan_bar = s_scan_detail = NULL;
    s_btn_apply = s_btn_next1 = s_btn_cancel = s_btn_start = NULL;
    for (int i = 0; i < 4; i++) s_scan_results[i] = NULL;
    /* NB: the dashboard reload (if s_ecu_applied) is NOT triggered here.
     * Finish fires it explicitly after _close_wizard; the WiFi-join path
     * intentionally skips it so _deferred_reload_after_wizard doesn't
     * clobber the wifi_screen that wifi_ui_show just loaded. */
}

static void _show_step3(void);
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
        if (s_btn_start)  lv_obj_add_flag(s_btn_start, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_apply)  lv_obj_add_flag(s_btn_apply, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_next1)  lv_obj_add_flag(s_btn_next1, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_cancel) lv_obj_clear_flag(s_btn_cancel, LV_OBJ_FLAG_HIDDEN);
        break;

    case CAN_SCAN_TESTING_BITRATE: {
        uint8_t idx = r->current_bitrate_idx;
        lv_label_set_text(s_scan_status, "Scanning for CAN traffic...");
        lv_label_set_text_fmt(s_scan_progress,
            "Testing %s  (%d of 4)", BR_NAMES[idx], idx + 1);
        lv_bar_set_value(s_scan_bar, idx * 25, LV_ANIM_ON);
        for (uint8_t i = 0; i < 4; i++) {
            if (i < idx) {
                /* bus_errors == 0xFFFFFFFFu is the "install failed"
                 * sentinel set by can_bus_test.c when the TWAI install
                 * couldn't recover after retries. Show something useful
                 * instead of "No traffic" which implies wiring is fine. */
                bool install_failed =
                    (r->results[i].bus_errors == 0xFFFFFFFFu);
                if (r->results[i].traffic_detected) {
                    lv_label_set_text_fmt(s_scan_results[i],
                        "%s  --  %lu frames", BR_NAMES[i],
                        (unsigned long)r->results[i].frames_received);
                    lv_obj_set_style_text_color(s_scan_results[i],
                        THEME_COLOR_STATUS_CONNECTED, 0);
                } else if (install_failed) {
                    lv_label_set_text_fmt(s_scan_results[i],
                        "%s  --  CAN driver busy, retrying", BR_NAMES[i]);
                    lv_obj_set_style_text_color(s_scan_results[i],
                        THEME_COLOR_STATUS_ERROR, 0);
                } else {
                    lv_label_set_text_fmt(s_scan_results[i],
                        "%s  --  No traffic", BR_NAMES[i]);
                    lv_obj_set_style_text_color(s_scan_results[i],
                        THEME_COLOR_TEXT_MUTED, 0);
                }
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

        /* Final result labels — same install-failed sentinel handling
         * as the in-progress case above, so the user can tell driver
         * trouble (orange) apart from a quiet bus (muted). */
        for (uint8_t i = 0; i < 4; i++) {
            bool install_failed = (r->results[i].bus_errors == 0xFFFFFFFFu);
            if (r->results[i].traffic_detected) {
                lv_label_set_text_fmt(s_scan_results[i],
                    "%s  --  %lu frames", BR_NAMES[i],
                    (unsigned long)r->results[i].frames_received);
                lv_obj_set_style_text_color(s_scan_results[i],
                    THEME_COLOR_STATUS_CONNECTED, 0);
            } else if (install_failed) {
                lv_label_set_text_fmt(s_scan_results[i],
                    "%s  --  CAN driver busy", BR_NAMES[i]);
                lv_obj_set_style_text_color(s_scan_results[i],
                    THEME_COLOR_STATUS_ERROR, 0);
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
            /* If every bitrate failed to install, that's a peripheral
             * state issue rather than a wiring problem — say so. */
            uint8_t install_fails = 0;
            for (uint8_t i = 0; i < 4; i++) {
                if (r->results[i].bus_errors == 0xFFFFFFFFu) install_fails++;
            }
            if (install_fails == 4) {
                lv_label_set_text(s_scan_status,
                    "CAN driver could not initialise");
                lv_obj_set_style_text_color(s_scan_status,
                    THEME_COLOR_STATUS_ERROR, 0);
                lv_label_set_text(s_scan_detail,
                    "TWAI peripheral is stuck. Reboot the dash and try again.\n"
                    "If this persists, check the serial log for CAN_TEST errors.");
            } else {
                lv_label_set_text(s_scan_status,
                    "No CAN traffic detected");
                lv_obj_set_style_text_color(s_scan_status,
                    THEME_COLOR_STATUS_ERROR, 0);
                lv_label_set_text(s_scan_detail,
                    "Check wiring & ignition. You can re-scan from Device Settings.");
            }
        }

        /* Hide cancel, show Re-scan + (when no traffic) Continue */
        lv_obj_add_flag(s_btn_cancel, LV_OBJ_FLAG_HIDDEN);
        if (s_btn_start) {
            lv_obj_t *slbl = lv_obj_get_child(s_btn_start, 0);
            if (slbl) lv_label_set_text(slbl, "Re-scan");
            lv_obj_clear_flag(s_btn_start, LV_OBJ_FLAG_HIDDEN);
        }
        if (r->recommended_bitrate < 0) {
            /* No traffic — surface "Continue without CAN" at the Apply slot */
            lv_obj_clear_flag(s_btn_next1, LV_OBJ_FLAG_HIDDEN);
        }
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

/* Reset step 1 visuals back to "scan starting" state. Used by both the
 * initial auto-start path and the manual Start/Re-scan button. */
static void _reset_step1_for_scan(void) {
    if (s_btn_apply)  lv_obj_add_flag(s_btn_apply, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_next1)  lv_obj_add_flag(s_btn_next1, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_start)  lv_obj_add_flag(s_btn_start, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_cancel) lv_obj_clear_flag(s_btn_cancel, LV_OBJ_FLAG_HIDDEN);
    if (s_scan_status) {
        lv_label_set_text(s_scan_status, "Starting scan...");
        lv_obj_set_style_text_color(s_scan_status, THEME_COLOR_TEXT_PRIMARY, 0);
    }
    if (s_scan_detail)   lv_label_set_text(s_scan_detail, "");
    if (s_scan_progress) lv_label_set_text(s_scan_progress, "");
    if (s_scan_bar)      lv_bar_set_value(s_scan_bar, 0, LV_ANIM_OFF);
    for (int i = 0; i < 4; i++) {
        if (!s_scan_results[i]) continue;
        lv_label_set_text_fmt(s_scan_results[i], "%s  --  ...", BR_NAMES[i]);
        lv_obj_set_style_text_color(s_scan_results[i],
                                    THEME_COLOR_TEXT_MUTED, 0);
    }
}

/* Show the start button in a "could not start" state so the user can
 * retry instead of being stuck staring at "Starting scan..." forever. */
static void _show_start_failed_state(void) {
    if (s_btn_cancel) lv_obj_add_flag(s_btn_cancel, LV_OBJ_FLAG_HIDDEN);
    if (s_btn_start) {
        lv_obj_t *slbl = lv_obj_get_child(s_btn_start, 0);
        if (slbl) lv_label_set_text(slbl, "Start Scan");
        lv_obj_clear_flag(s_btn_start, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_scan_status) {
        lv_label_set_text(s_scan_status,
            "Could not start scan - tap Start to retry");
        lv_obj_set_style_text_color(s_scan_status,
            THEME_COLOR_STATUS_ERROR, 0);
    }
}

/* Retry timer body — see _try_start_scan for usage. */
static void _start_retry_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_step1 || !lv_obj_is_valid(s_step1)) {
        /* Wizard torn down mid-retry — clean up and bail. */
        if (s_start_retry_timer) {
            lv_timer_del(s_start_retry_timer);
            s_start_retry_timer = NULL;
        }
        s_start_retry_count = 0;
        return;
    }

    s_start_retry_count++;
    if (can_bus_test_start()) {
        /* Success — kill the timer; _scan_ui_update will take over. */
        ESP_LOGI(TAG, "scan started after %u retries", s_start_retry_count);
        lv_timer_del(s_start_retry_timer);
        s_start_retry_timer = NULL;
        s_start_retry_count = 0;
        return;
    }

    if (s_start_retry_count >= START_RETRY_MAX) {
        ESP_LOGW(TAG, "scan-start retry budget exhausted (%u attempts)",
                 s_start_retry_count);
        lv_timer_del(s_start_retry_timer);
        s_start_retry_timer = NULL;
        s_start_retry_count = 0;
        _show_start_failed_state();
        return;
    }

    /* Still waiting — keep status visible so user knows we're trying. */
    if (s_scan_status) {
        lv_label_set_text_fmt(s_scan_status,
            "Waiting for previous scan to finish... (%u/%d)",
            s_start_retry_count, START_RETRY_MAX);
    }
}

/* Try to start the scan immediately; if a previous scan is still cleaning
 * up, schedule a retry timer that polls until it can start (up to
 * START_RETRY_MAX × START_RETRY_PERIOD_MS). */
static void _try_start_scan(void)
{
    _reset_step1_for_scan();
    can_bus_test_set_ui_callback(_scan_ui_update);

    if (can_bus_test_start()) return;

    /* Couldn't start right now — most likely a previous scan is still
     * cleaning up. Show waiting status and poll. */
    if (s_scan_status) {
        lv_label_set_text(s_scan_status,
            "Waiting for previous scan to finish...");
        lv_obj_set_style_text_color(s_scan_status,
            THEME_COLOR_TEXT_PRIMARY, 0);
    }
    s_start_retry_count = 0;
    if (s_start_retry_timer) lv_timer_del(s_start_retry_timer);
    s_start_retry_timer = lv_timer_create(_start_retry_timer_cb,
                                          START_RETRY_PERIOD_MS, NULL);
}

static void _btn_start_scan_cb(lv_event_t *e) {
    (void)e;
    /* Don't double-start. If a retry is already in flight, the user just
     * needs to wait — but give them visible feedback so the click isn't
     * silently swallowed. */
    if (s_start_retry_timer) {
        if (s_scan_status) {
            lv_label_set_text(s_scan_status,
                "Already retrying — please wait...");
        }
        return;
    }
    _try_start_scan();
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

static void _btn_skip_forever_cb(lv_event_t *e) {
    (void)e;
    /* Skip for good - mark first-run done so the wizard never shows again.
     * User can still reach the same setup screens from Device Settings. */
    _close_wizard(true);
}

static void _wizard_check_wifi_return_cb(lv_timer_t *timer);

/* Shared body for the two WiFi-screen entry points (Join WiFi / Start Hotspot).
 * Keeps the wizard overlay alive so the user returns to step 3 after they
 * dismiss the WiFi screen. First-run-done is only written when the user
 * explicitly taps Finish Setup, not from this path. */
static void _open_wifi_ui_with_preset(wifi_ui_preset_t preset) {
    ESP_LOGI(TAG, "Wizard → WiFi UI preset=%s (overlay preserved for return)",
             preset == WIFI_UI_PRESET_AP ? "AP" : "STA");
    can_bus_test_set_ui_callback(NULL);
    if (can_bus_test_is_running()) can_bus_test_cancel();

    /* Temporarily hide the overlay so the WiFi screen isn't obscured.
     * When the user dismisses the WiFi screen, wifi_ui_hide restores the
     * dashboard screen and the return-timer below re-shows the overlay. */
    if (s_overlay && lv_obj_is_valid(s_overlay)) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    s_wifi_return_pending = true;
    if (!s_wifi_return_timer) {
        s_wifi_return_timer = lv_timer_create(_wizard_check_wifi_return_cb,
                                              200, NULL);
    }
    wifi_ui_show_with_preset(preset);
}

static void _btn_wifi_join_cb(lv_event_t *e) {
    (void)e;
    _open_wifi_ui_with_preset(WIFI_UI_PRESET_STA);
}

static void _btn_hotspot_start_cb(lv_event_t *e) {
    (void)e;
    _open_wifi_ui_with_preset(WIFI_UI_PRESET_AP);
}

/* Called from the LVGL task after WiFi UI hides. Re-reveals the wizard
 * overlay so the user lands back on step 3. */
static void _wizard_check_wifi_return_cb(lv_timer_t *timer) {
    if (!s_wifi_return_pending) return;
    if (wifi_ui_is_active()) return;  /* still up */
    s_wifi_return_pending = false;
    lv_timer_del(timer);
    s_wifi_return_timer = NULL;
    if (s_overlay && lv_obj_is_valid(s_overlay)) {
        lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        ESP_LOGI(TAG, "Returned from WiFi UI — wizard overlay restored");
    }
}

static void _btn_finish_cb(lv_event_t *e) {
    (void)e;
    /* Only path that permanently marks setup complete */
    bool reload = s_ecu_applied;
    s_ecu_applied = false;
    if (reload) {
        /* Atomic path: pass the overlay pointer into the async callback
         * and skip _close_wizard's del_async so we don't queue two
         * deletes for the same object. */
        config_store_save_first_run_done(true);
        ESP_LOGI(TAG, "First-run wizard completed (flag set)");
        can_bus_test_set_ui_callback(NULL);
        if (can_bus_test_is_running()) can_bus_test_cancel();
        lv_obj_t *overlay_to_free = s_overlay;
        s_overlay = s_card = s_step1 = s_step3 = NULL;
        s_scan_status = s_scan_progress = s_scan_bar = s_scan_detail = NULL;
        s_btn_apply = s_btn_next1 = s_btn_cancel = s_btn_start = NULL;
        for (int i = 0; i < 4; i++) s_scan_results[i] = NULL;
        lv_async_call(_deferred_reload_after_wizard, overlay_to_free);
    } else {
        _close_wizard(true);
    }
}

/* ── Step 2: ECU picker (opens the shared overlay) ────────────────────── */

static void _ecu_picker_done(bool applied, void *ctx) {
    (void)ctx;
    if (applied) s_ecu_applied = true;
    /* Whether the user applied or skipped, move on to WiFi step. The card
     * is still up from step 1 - the ECU picker used its own overlay on
     * lv_layer_top(), so we can proceed to WiFi directly. */
    _show_step3();
}

static void _show_step2(void) {
    can_bus_test_set_ui_callback(NULL);

    /* Remove step 1 content so the card behind the picker isn't stale. */
    if (s_step1 && lv_obj_is_valid(s_step1))
        lv_obj_del(s_step1);
    s_step1 = NULL;

    /* Open ECU picker on top of the card. Applies to the default layout. */
    ecu_picker_open("default", true, _ecu_picker_done, NULL);
}

/* ── Step 3: WiFi info ────────────────────────────────────────────────── */

static void _show_step3(void) {
    can_bus_test_set_ui_callback(NULL);

    /* Step 2 container */
    s_step3 = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_step3);
    lv_obj_set_size(s_step3, lv_pct(100), lv_pct(100));
    lv_obj_center(s_step3);
    lv_obj_clear_flag(s_step3, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(s_step3);
    lv_label_set_text(title, "Connect Your Device");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *sub = lv_label_create(s_step3);
    lv_label_set_text(sub, "Step 3 of 3  -  pick how to connect");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_font(sub, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(sub, THEME_COLOR_TEXT_MUTED, 0);

    /* ── Option 1 (Recommended): Join WiFi network ─────────────────── */
    lv_obj_t *opt1_label = lv_label_create(s_step3);
    lv_label_set_text(opt1_label, "1.  WiFi  (recommended)");
    lv_obj_align(opt1_label, LV_ALIGN_TOP_LEFT, 30, 66);
    lv_obj_set_style_text_font(opt1_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(opt1_label, THEME_COLOR_ACCENT_BLUE, 0);

    lv_obj_t *opt1_sub = lv_label_create(s_step3);
    lv_label_set_text(opt1_sub,
        "Join your home/shop WiFi - dash will show its IP in Device Settings");
    lv_obj_align(opt1_sub, LV_ALIGN_TOP_LEFT, 30, 86);
    lv_obj_set_style_text_font(opt1_sub, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(opt1_sub, THEME_COLOR_TEXT_MUTED, 0);

    _make_btn(s_step3, "Join a WiFi Network",
              THEME_COLOR_ACCENT_BLUE, THEME_COLOR_TEXT_ON_ACCENT,
              false, 108, _btn_wifi_join_cb);

    /* ── Option 2: Hotspot fallback ─────────────────────────────────── */
    const char *ap_ssid = wifi_manager_get_ap_ssid();
    const char *ap_ip   = wifi_manager_get_ap_ip();

    lv_obj_t *opt2_label = lv_label_create(s_step3);
    lv_label_set_text(opt2_label, "2.  Hotspot  (fallback, no WiFi needed)");
    lv_obj_align(opt2_label, LV_ALIGN_TOP_LEFT, 30, 170);
    lv_obj_set_style_text_font(opt2_label, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(opt2_label, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *opt2_sub = lv_label_create(s_step3);
    lv_label_set_long_mode(opt2_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(opt2_sub, BTN_W - 30);
    lv_label_set_text_fmt(opt2_sub,
        "Connect to \"%s\"  /  password: rdm7dash\n"
        "Then open http://%s in a browser",
        ap_ssid ? ap_ssid : "RDM7-????",
        ap_ip   ? ap_ip   : "192.168.4.1");
    lv_obj_align(opt2_sub, LV_ALIGN_TOP_LEFT, 30, 190);
    lv_obj_set_style_text_font(opt2_sub, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(opt2_sub, THEME_COLOR_TEXT_MUTED, 0);

    /* Start Hotspot button — flips runtime to AP mode AND persists
     * hotspot-on-boot, then drops the user into the WiFi screen so they
     * can see the SSID/IP/password live. */
    _make_btn(s_step3, "Start Hotspot",
              THEME_COLOR_SECTION_BG, THEME_COLOR_TEXT_PRIMARY,
              true, 234, _btn_hotspot_start_cb);

    /* Finish button */
    _make_btn(s_step3, "Finish Setup",
              lv_color_black(), THEME_COLOR_TEXT_MUTED,
              false, 300, _btn_finish_cb);
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
    lv_label_set_text(sub, "Step 1 of 3  -  Scanning all bitrates...");
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

    /* Row 1 (Y=248): primary forward action — Apply (with traffic) OR
     * Continue without CAN (no traffic). Mutually exclusive, share Y. */
    s_btn_apply = _make_btn(s_step1, "Apply & Continue",
                            THEME_COLOR_ACCENT_BLUE, THEME_COLOR_TEXT_ON_ACCENT,
                            false, 248, _btn_apply_cb);
    lv_obj_add_flag(s_btn_apply, LV_OBJ_FLAG_HIDDEN);

    s_btn_next1 = _make_btn(s_step1, "Continue without CAN",
                            THEME_COLOR_SECTION_BG, THEME_COLOR_TEXT_MUTED,
                            true, 248, _btn_next1_cb);
    lv_obj_add_flag(s_btn_next1, LV_OBJ_FLAG_HIDDEN);

    /* Row 2 (Y=296): scan control — Cancel (while running) OR
     * Start/Re-scan (idle/complete/error). Mutually exclusive, share Y. */
    s_btn_cancel = _make_btn(s_step1, "Cancel Scan",
                             THEME_COLOR_SECTION_BG, THEME_COLOR_TEXT_MUTED,
                             true, 296, _btn_cancel_scan_cb);

    s_btn_start = _make_btn(s_step1, "Start Scan",
                            THEME_COLOR_SECTION_BG, THEME_COLOR_TEXT_PRIMARY,
                            true, 296, _btn_start_scan_cb);
    lv_obj_add_flag(s_btn_start, LV_OBJ_FLAG_HIDDEN);

    /* Row 3 (Y=340): Skip for now — wizard returns on next boot */
    _make_btn(s_step1, "Skip for now  (ask again next boot)",
              lv_color_black(), THEME_COLOR_TEXT_MUTED,
              false, 340, _btn_skip_cb);

    /* Row 4 (Y=384): Skip for good — never show again.
     * The same setup screens stay reachable from Device Settings, so
     * this is non-destructive — just dismisses the boot prompt forever. */
    _make_btn(s_step1, "Skip for good  (don't show again)",
              lv_color_black(), THEME_COLOR_TEXT_MUTED,
              false, 384, _btn_skip_forever_cb);
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

    /* Build step 1 (CAN scan) and auto-start. _try_start_scan handles the
     * common race where the user re-runs the wizard while a previous scan
     * task is still in cleanup (sub-second window) — it polls for up to
     * 3 s before giving up. */
    _build_step1();
    ESP_LOGI(TAG, "First-run wizard shown, kicking scan");
    _try_start_scan();
}
