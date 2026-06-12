/* first_run_wizard.c - four-step onboarding: CAN scan, ECU auto-detect,
 * Channels review, WiFi options.
 *
 * Runs CAN bitrate scan inline (step 1), auto-detects the ECU by
 * matching CAN IDs against the preconfig catalog (step 2), shows the
 * canonical channels that just got auto-bound from the detected
 * preset (step 3, tap any row to retarget), then connection options
 * (step 4). User can skip at any point — the same screens are reachable
 * from Device Settings later. NVS flag marks completion on Finish Setup
 * only; plain Skip lets the wizard return on the next boot.
 *
 * The legacy `ecu_presets[]` static table has been replaced everywhere
 * the wizard touches it: ECU auto-detect uses `preconfig_items[]` (same
 * one the web Studio uses, dynamically extended when DBCs are imported)
 * and the channels picker uses it too. New signal libraries land
 * everywhere without code changes. */

#include "first_run_wizard.h"

#include "esp_log.h"
#include <ctype.h>
#include <string.h>

#include "../theme.h"
#include "../../system/rdm_lv_async.h"
#include "../../storage/config_store.h"
#include "../../can/can_bus_test.h"
#include "../../can/can_id_tracker.h"
#include "../../can/can_manager.h"
#include "../../can/obd2.h"
#include "../../data/channel_manager.h"
#include "../../data/channel_source_apply.h"
#include "../../data/canonical_channels.h"
#include "../../data/unit_convert.h"
#include "../../layout/ecu_presets.h"
#include "../../layout/layout_manager.h"
#include "../../net/wifi_manager.h"
#include "../../widgets/signal.h"
#include "../../widgets/widget_types.h"   /* widget_t + widget_get_channel_id_buf */
#include "../dashboard.h"                 /* dashboard_persist_layout */
#include "../menu/menu_screen.h"          /* load_menu_screen_for_widget (transition link) */
#include "../menu/config_modal.h"         /* config_modal_has_content */
#include "../callbacks/ui_callbacks.h"    /* show_numeric_input_dialog */
#include "../settings/preset_picker.h"
#include <stdlib.h>                        /* strtol / strtof */
#include "ui_wifi.h"
#include "ui_Screen3.h"

#include <stdio.h>

static const char *TAG = "first_run";

/* ── Layout constants ─────────────────────────────────────────────────── */
#define CARD_W   560
#define CARD_H   470   /* Bumped from 430 to fit the Skip-for-good button in step 1 */
#define BTN_W    500
#define BTN_H     40

/* Channels step uses a wider card so the split-pane (list left, detail
 * right) lays out properly. Card resizes on enter and restores when
 * leaving for the WiFi step.
 *
 * Layout — mirrors the web Studio's Channels modal:
 *   ┌─ Hero (title + step chip + stats line + hairline) ────────────┐
 *   │ ┌── List (left) ────┐ ┌── Detail (right) ─────────────────────┐│
 *   │ │ scrollable rows   │ │ hero value, source pill, range edits  ││
 *   │ └───────────────────┘ └───────────────────────────────────────┘│
 *   │ ┌── Continue (full width) ────────────────────────────────────┐│
 *   └────────────────────────────────────────────────────────────────┘
 *
 * Row width was 720 (full card). Compressing to ~336 halves the dirty
 * area per scroll/repaint — the framerate cost of the old design was
 * unacceptable on the dash. */
#define CH_CARD_W      760
#define CH_CARD_H      460
#define CH_HERO_H       60   /* title row + stats row + 1 px rule */
#define CH_BODY_TOP     CH_HERO_H + 8
#define CH_BODY_H      332
#define CH_PANE_GAP     12
#define CH_LIST_W      336
#define CH_DETAIL_W    372
#define CH_ROW_W       (CH_LIST_W - 4)  /* inside list pad */
#define CH_ROW_H        40              /* compressed — label + value, no subtitle */

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

/* Step 3: Channels review (read-only confirmation of what got auto-bound
 * by the ECU pick). User can verify their dash is wired before moving on
 * to WiFi setup. Sub-modal style + live-value polling mirror the web
 * Studio's Channels view for familiarity. */
static lv_obj_t  *s_step_channels         = NULL;
static lv_obj_t  *s_channels_list_box     = NULL;
static lv_obj_t  *s_channels_stats_lbl    = NULL;  /* hero "N bound, M unbound" line */
static lv_timer_t *s_channels_refresh_timer = NULL;
/* One entry per row in the list — either a canonical channel (ghost
 * row, activated on first tap) or an already-active channel (canonical
 * or custom). The id pointer lives in flash (canonical def) or in the
 * channel struct (custom) so we never copy. Channel lookup by id is
 * O(N) but N stays under ~120 and we only do it on click / refresh,
 * not per-frame.
 *
 * Bumped from 32 → 144 because the list now shows every canonical
 * channel (~90) plus custom rows + the bottom "+ Add" button. */
#define WIZ_CH_MAX_ROWS 144
static lv_obj_t  *s_channels_rows[WIZ_CH_MAX_ROWS]       = {NULL};
static lv_obj_t  *s_channels_value_lbls[WIZ_CH_MAX_ROWS] = {NULL};
/* id pointer — canonical_def.id or channel_t.id. NULL means slot
 * vacant. Replaces the old s_channels_refs[] (channel_t pointer) so
 * ghost rows have something to bind their click handler to before
 * the channel is activated. */
static const char *s_channels_ids[WIZ_CH_MAX_ROWS]       = {NULL};
static uint16_t    s_channels_count        = 0;
/* Set by the "+ Add custom channel" button so the next preset-picker
 * apply creates a custom_* channel instead of re-binding an existing
 * one. Always cleared in the picker close path. */
static bool        s_creating_custom       = false;

/* ── OBD2 gap-fill state ───────────────────────────────────────────────
 * Optional, opt-in: poll standard OBD2 PIDs to source channels the
 * primary ECU broadcast doesn't cover (fuel level/pressure on a GT86,
 * etc.). Never auto-runs — only on an explicit "Fill gaps" tap. Scoped
 * to ACTIVE + unbound + has-OBD2 channels so we don't poll PIDs nothing
 * needs (keeps bus traffic minimal; standalone-ECU users just ignore
 * the chip). */
#define WIZ_OBD2_MAX_GAPS   24
#define WIZ_OBD2_PROBE_MS   3200   /* response window per attempt */
static const char *s_obd2_gap_ids[WIZ_OBD2_MAX_GAPS]      = {NULL}; /* channel id */
static const char *s_obd2_gap_signames[WIZ_OBD2_MAX_GAPS] = {NULL}; /* obd2 signal name */
static uint32_t    s_obd2_gap_pids[WIZ_OBD2_MAX_GAPS]     = {0};    /* encoded (svc,pid) */
static uint8_t     s_obd2_gap_count                       = 0;
/* Original polled-PID set, saved before a probe so we can unwind cleanly
 * if the user gives up (no OBD2 on this vehicle). */
static uint32_t    s_obd2_orig_pids[OBD2_MAX_ENABLED]     = {0};
static uint8_t     s_obd2_orig_count                      = 0;
static lv_obj_t   *s_obd2_chip          = NULL;   /* "Fill N from OBD2" pill */
static lv_obj_t   *s_step_chip_lbl      = NULL;   /* header text the pill anchors to */
static lv_obj_t   *s_obd2_modal         = NULL;   /* probe/result overlay */
static lv_obj_t   *s_obd2_status_lbl    = NULL;
static lv_obj_t   *s_obd2_hint_lbl      = NULL;
static lv_obj_t   *s_obd2_rescan_btn    = NULL;
static lv_obj_t   *s_obd2_spinner       = NULL;
static lv_timer_t *s_obd2_probe_timer   = NULL;
static uint8_t     s_obd2_attempt       = 0;
/* Forward decl so the early teardown paths (_close_wizard, _show_step3)
 * can tear down the OBD2 modal + standalone probe timer. */
static void _obd2_close_modal(void);

/* Right-pane detail state. The pane is fully rebuilt whenever a new
 * channel is selected — cheap because there's only one of it. Live
 * value at the top is refreshed by the same 500 ms timer that drives
 * the row values, so we cache its label pointer too. */
static lv_obj_t  *s_detail_pane           = NULL;
static lv_obj_t  *s_detail_value_lbl      = NULL;
static char       s_selected_ch_id[32]    = {0};
/* Cached editable-field widgets so steppers can re-read after a mutate
 * without a rebuild. NULL when the detail pane is empty. */
static lv_obj_t  *s_detail_min_lbl        = NULL;
static lv_obj_t  *s_detail_max_lbl        = NULL;
static lv_obj_t  *s_detail_decimals_lbl   = NULL;
static lv_obj_t  *s_detail_warn_lbl       = NULL;
static lv_obj_t  *s_detail_source_lbl     = NULL;

/* Step 2: ECU auto-detect — match seen CAN IDs against the preconfig
 * catalog, scored by (matched IDs) / (total IDs for that ECU). The top
 * (ECU, version) wins and is offered to the user with a "Use this" CTA.
 * Per-(ECU, version) state lives in s_ecu_matches; UI handles live in
 * s_step_ecu_*. */
#define WIZ_ECU_MAX           16
/* Auto-pick floor — *absolute* matched IDs rather than percentage.
 * Percentage-floor gating drops superset presets whose extra IDs aren't
 * all broadcast: MaxxECU 1.3 covers 21 IDs but a real 1.3 unit might
 * only emit 5 of them (5/21 = 24%); the narrower 1.2 preset with the
 * same 5 IDs hits 5/12 = 42%, so a pct-floor at 30% kills 1.3 before
 * the matched-count tiebreaker can run.
 *
 * Three distinct 11-bit IDs as the gate is statistically robust (CAN
 * ID coincidence ≈ (tracked_n/2048)^3 — under 1e-6 for typical bus
 * snapshots) and lets the tiered (matched, pct, total) comparator
 * decide the winner. WIZ_ECU_MATCH_PCT_MIN stays defined for the
 * picker chip's "strong vs weak" colouring. */
#define WIZ_ECU_MIN_MATCHED    3
#define WIZ_ECU_MATCH_PCT_MIN 30   /* Picker chip colour threshold */
#define WIZ_ECU_PROBE_MS      4500 /* Long enough for slow 1 Hz broadcasters */

typedef struct {
    char    ecu[24];
    char    version[24];
    uint8_t matched;
    uint8_t total;
} wiz_ecu_match_t;

static wiz_ecu_match_t s_ecu_matches[WIZ_ECU_MAX];
static uint8_t         s_ecu_match_count = 0;
static int             s_ecu_top_match   = -1;

static lv_obj_t  *s_step_ecu        = NULL;
static lv_obj_t  *s_ecu_progress    = NULL;
static lv_obj_t  *s_ecu_status      = NULL;
static lv_obj_t  *s_ecu_result_card = NULL;
static lv_obj_t  *s_ecu_picker_sheet = NULL;
static lv_timer_t *s_ecu_probe_timer = NULL;
static uint32_t   s_ecu_probe_start_ms = 0;

/* (ECU, Version) shortlist used by the "Pick a different ECU" sheet.
 * Walked from preconfig_items[] when the sheet opens. Each row =
 * one unique pair; tapping applies all signals for that pair. */
#define WIZ_PICK_MAX 48
static const char *s_pick_ecus[WIZ_PICK_MAX]     = {NULL};
static const char *s_pick_versions[WIZ_PICK_MAX] = {NULL};
static uint8_t     s_pick_count                  = 0;
static bool        s_pick_is_obd2[WIZ_PICK_MAX]  = {false};
/* Live-CAN match scores per pick row — populated from s_ecu_matches[]
 * during _ecu_pick_collect. matched_n=0 means "we didn't see any of
 * this ECU's IDs on the bus". Used by the picker to (a) sort rows by
 * descending score and (b) show the "N%" chip on each row. */
static uint8_t     s_pick_matched[WIZ_PICK_MAX]  = {0};
static uint8_t     s_pick_total[WIZ_PICK_MAX]    = {0};

/* Step 3: Channels review */
/* (state already declared above — Step 2 ECU detect inserted between
 * scan and channels.) */

/* Step 4: WiFi info */
static lv_obj_t  *s_step3         = NULL;

/* Sub-overlay for the per-channel source picker (Step 3). Holds the
 * preset_picker_embedded preconfig catalog. Owned by the wizard. */
static lv_obj_t  *s_bind_sheet            = NULL;
static const channel_t *s_bind_target_chan = NULL;

/* Set when the user picks an ECU (Step 2) OR binds any channel (Step 3).
 * Trips a dashboard reload at Finish so widgets pick up new bindings. */
static bool       s_channels_changed = false;

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

/* When true, the overlay was launched as a standalone Channels editor
 * (from Device Settings) rather than the full onboarding flow. The
 * channels step uses this to relabel its footer ("Done" not "Continue")
 * and close on commit instead of advancing to the Wi-Fi step. */
static bool s_standalone_channels = false;

/* Apply-to-widget mode: the editor was opened by long-pressing a
 * dashboard widget. The footer swaps to "Apply to widget" (binds the
 * selected channel to s_apply_target_widget) plus a "Widget settings"
 * link to the legacy per-widget config modal during the phase-out. */
static bool      s_apply_to_widget_mode = false;
static widget_t *s_apply_target_widget  = NULL;
/* Channel id the target widget is CURRENTLY bound to (explicit channel_id or,
 * for legacy signal-bound widgets, the channel whose signal matches). Cached
 * at open so the orange "applied" highlight + auto-select work regardless of
 * how the widget was bound. */
static char      s_apply_widget_chid[40] = {0};

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
    /* Channels review timer — independent of can_bus_test/wifi loops. */
    if (s_channels_refresh_timer) {
        lv_timer_del(s_channels_refresh_timer);
        s_channels_refresh_timer = NULL;
    }
    /* OBD2 gap-fill modal + standalone probe timer — tear down before
     * the overlay (its parent) is freed, and unwind any in-flight poll. */
    _obd2_close_modal();
    s_obd2_chip = NULL;
    /* ECU detect probe timer — must die before its target container does. */
    if (s_ecu_probe_timer) {
        lv_timer_del(s_ecu_probe_timer);
        s_ecu_probe_timer = NULL;
    }
    /* Restore the CAN filter if the wizard is being torn down mid-probe. */
    if (can_is_promiscuous()) {
        can_set_promiscuous_mode(false);
    }
    s_start_retry_count = 0;
    s_wifi_return_pending = false;
    if (s_overlay && lv_obj_is_valid(s_overlay))
        rdm_obj_del_async(s_overlay);   /* crash-safe: cancelled if a reload frees it first */
    s_overlay = s_card = s_step1 = s_step_channels = s_step3 = NULL;
    s_step_ecu = NULL;
    s_ecu_progress = s_ecu_status = s_ecu_result_card = NULL;
    s_ecu_picker_sheet = NULL;
    s_channels_list_box = NULL;
    s_bind_sheet = NULL;
    s_bind_target_chan = NULL;
    for (int i = 0; i < WIZ_CH_MAX_ROWS; i++) {
        s_channels_value_lbls[i] = NULL;
        s_channels_rows[i]       = NULL;
        s_channels_ids[i]        = NULL;
    }
    s_channels_count       = 0;
    s_creating_custom      = false;
    s_detail_pane          = NULL;
    s_detail_value_lbl     = NULL;
    s_detail_min_lbl       = NULL;
    s_detail_max_lbl       = NULL;
    s_detail_decimals_lbl  = NULL;
    s_detail_warn_lbl      = NULL;
    s_detail_source_lbl    = NULL;
    s_channels_stats_lbl   = NULL;
    s_step_chip_lbl        = NULL;
    s_selected_ch_id[0]    = '\0';
    s_scan_status = s_scan_progress = s_scan_bar = s_scan_detail = NULL;
    s_btn_apply = s_btn_next1 = s_btn_cancel = s_btn_start = NULL;
    for (int i = 0; i < 4; i++) s_scan_results[i] = NULL;
    s_standalone_channels  = false;
    s_apply_to_widget_mode = false;
    s_apply_target_widget  = NULL;
    s_apply_widget_chid[0] = '\0';
    /* NB: the dashboard reload (if s_ecu_applied) is NOT triggered here.
     * Finish fires it explicitly after _close_wizard; the WiFi-join path
     * intentionally skips it so _deferred_reload_after_wizard doesn't
     * clobber the wifi_screen that wifi_ui_show just loaded. */
}

static void _show_step3(void);
static void _show_step_channels(void);
static void _show_step_ecu_detect(void);
static void _close_bind_sheet(void);

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
    /* After the bitrate is applied the live can_id_tracker starts
     * receiving frames. Step 2 lets it accumulate for a couple of
     * seconds, then matches against the preconfig catalog. */
    if (s_step1 && lv_obj_is_valid(s_step1)) lv_obj_del(s_step1);
    s_step1 = NULL;
    _show_step_ecu_detect();
}

static void _btn_next1_cb(lv_event_t *e) {
    (void)e;
    if (s_step1 && lv_obj_is_valid(s_step1)) lv_obj_del(s_step1);
    s_step1 = NULL;
    _show_step_ecu_detect();
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
static void _btn_finish_cb(lv_event_t *e);

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

/* Called from the LVGL task after WiFi UI hides. Picking either WiFi or
 * Hotspot in step 3 is treated as wizard completion — the user has made
 * their connection choice, so we mark first_run_done and tear down rather
 * than bouncing them back to step 3 to tap Finish Setup. _btn_finish_cb
 * handles the dashboard reload if step 2 applied an ECU preset. */
static void _wizard_check_wifi_return_cb(lv_timer_t *timer) {
    if (!s_wifi_return_pending) return;
    if (wifi_ui_is_active()) return;  /* still up */
    s_wifi_return_pending = false;
    lv_timer_del(timer);
    s_wifi_return_timer = NULL;
    ESP_LOGI(TAG, "Returned from WiFi UI — wizard auto-completing");
    _btn_finish_cb(NULL);
}

static void _btn_finish_cb(lv_event_t *e) {
    (void)e;
    /* Only path that permanently marks setup complete */
    bool reload = s_channels_changed;
    s_channels_changed = false;
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

/* ── Step 2: ECU Auto-Detect ──────────────────────────────────────────── */

/* Lowercase + alnum-only signal name from a preconfig label. Duplicates
 * web_server_channels.c::_derive_signal_name (file-local there) so the
 * wizard can apply preconfigs without going through HTTP. */
static void _wiz_derive_signal_name(const char *label, char *out, size_t sz) {
    if (!label || !out || sz == 0) { if (out && sz) out[0] = '\0'; return; }
    size_t j = 0;
    for (size_t i = 0; label[i] && j < sz - 1; i++) {
        char c = label[i];
        if (isalnum((unsigned char)c))
            out[j++] = (char)toupper((unsigned char)c);
        else if (j > 0 && out[j - 1] != '_')
            out[j++] = '_';
    }
    while (j > 0 && out[j - 1] == '_') j--;
    out[j] = '\0';
}

/* Score the preconfig catalog against the live can_id_tracker. For
 * every (ecu, version) pair, count the distinct CAN IDs in that group
 * and how many of them appear in the tracker right now. Best-scoring
 * pair (above WIZ_ECU_MATCH_PCT_MIN) wins. */
static void _compute_ecu_matches(void) {
    s_ecu_match_count = 0;
    s_ecu_top_match   = -1;

    /* Pass 1: enumerate distinct (ecu, version) keys. */
    for (int i = 0; i < preconfig_items_count; i++) {
        const preconfig_item_t *it = &preconfig_items[i];
        if (!it->ecu || !it->version || !it->can_id) continue;
        /* OBD2 entries don't broadcast — they get polled. Skip from
         * match scoring; users with an OBD2 dongle will pick "Pick
         * different" → preset catalog and select OBD2 explicitly. */
        if (it->obd2_pid) continue;
        bool found = false;
        for (uint8_t j = 0; j < s_ecu_match_count; j++) {
            if (strcmp(s_ecu_matches[j].ecu, it->ecu) == 0 &&
                strcmp(s_ecu_matches[j].version, it->version) == 0) {
                found = true;
                break;
            }
        }
        if (!found && s_ecu_match_count < WIZ_ECU_MAX) {
            wiz_ecu_match_t *m = &s_ecu_matches[s_ecu_match_count++];
            memset(m, 0, sizeof(*m));
            strncpy(m->ecu,     it->ecu,     sizeof(m->ecu)     - 1);
            strncpy(m->version, it->version, sizeof(m->version) - 1);
        }
    }

    /* Pass 2: for each pair, collect its distinct CAN IDs, then check
     * how many are present in the live tracker. */
    uint16_t tracker_n = can_id_tracker_count();
    for (uint8_t j = 0; j < s_ecu_match_count; j++) {
        wiz_ecu_match_t *m = &s_ecu_matches[j];
        uint32_t ids[48] = {0};
        uint8_t ids_n = 0;
        for (int i = 0; i < preconfig_items_count; i++) {
            const preconfig_item_t *it = &preconfig_items[i];
            if (!it->ecu || !it->version || !it->can_id) continue;
            if (it->obd2_pid) continue;
            if (strcmp(it->ecu, m->ecu) != 0) continue;
            if (strcmp(it->version, m->version) != 0) continue;
            uint32_t cid = (uint32_t)strtol(it->can_id, NULL, 16);
            bool dup = false;
            for (uint8_t k = 0; k < ids_n; k++) {
                if (ids[k] == cid) { dup = true; break; }
            }
            if (!dup && ids_n < 48) ids[ids_n++] = cid;
        }
        m->total = ids_n;
        m->matched = 0;
        for (uint8_t i = 0; i < ids_n; i++) {
            for (uint16_t t = 0; t < tracker_n; t++) {
                const can_id_entry_t *e = can_id_tracker_get(t);
                if (e && e->can_id == ids[i]) {
                    m->matched++;
                    break;
                }
            }
        }
    }

    /* Ranking — tiered descending by (matched, pct, total).
     *
     * Why matched first: when one preset's CAN-ID set is a strict
     * superset of another's (MaxxECU 1.3 ⊃ 1.2, same base IDs + new
     * ones for new features), the smaller preset hits 100% on its own
     * IDs even when the device is the larger one. Pure-percentage
     * ranking then picks 1.2 over 1.3 — wrong.
     *
     * Counting matched IDs in absolute terms breaks that tie correctly:
     *   - real 1.3 bus (20 IDs of 21):  1.3 matched=20 wins vs 1.2 matched=11
     *   - real 1.2 bus (11 IDs of 11):  1.2 matched=11 ties with 1.3 matched=11
     *                                   → secondary key (pct) picks 1.2 (100% > 52%)
     *
     * Tertiary `total` keeps ties stable in catalog order when two
     * presets cover exactly the same IDs (currently no such pair, but
     * leaves the comparator robust). */
    uint8_t best_matched = 0;
    int     best_pct     = 0;
    uint8_t best_total   = 0;
    for (uint8_t j = 0; j < s_ecu_match_count; j++) {
        const wiz_ecu_match_t *m = &s_ecu_matches[j];
        if (m->total == 0) continue;
        int score = (m->matched * 100) / m->total;
        if (m->matched > 0) {
            ESP_LOGI(TAG, "ECU candidate: %-22s %-14s %u/%u (%d%%)",
                     m->ecu, m->version, m->matched, m->total, score);
        }
        if (m->matched < WIZ_ECU_MIN_MATCHED) continue;
        bool wins =
            (m->matched > best_matched) ||
            (m->matched == best_matched && score > best_pct) ||
            (m->matched == best_matched && score == best_pct &&
             m->total > best_total);
        if (wins) {
            best_matched = m->matched;
            best_pct     = score;
            best_total   = m->total;
            s_ecu_top_match = j;
        }
    }
    if (s_ecu_top_match >= 0) {
        const wiz_ecu_match_t *m = &s_ecu_matches[s_ecu_top_match];
        ESP_LOGI(TAG, "ECU PICK: %s %s  %u/%u (%d%%)",
                 m->ecu, m->version, m->matched, m->total, best_pct);
    } else {
        ESP_LOGI(TAG, "ECU PICK: none above %u-matched threshold (%u IDs heard)",
                 (unsigned)WIZ_ECU_MIN_MATCHED, can_id_tracker_count());
    }
}

/* Map a derived preconfig signal name back onto the canonical ECU
 * vocabulary the rest of the dash uses ("RPM", "COOLANT_TEMP",
 * "THROTTLE", ...). Toyota's preconfig label "ENGINE RPM" derives to
 * "ENGINE_RPM" — but every default widget binds to "RPM", so we'd end
 * up with two signal rows ("RPM" can_id=0, "ENGINE_RPM" can_id=0x140)
 * and the widget would stay subscribed to the dead one.
 *
 * Path: derived "ENGINE_RPM" → canonical id "rpm" via
 * ecu_signal_name_to_canonical (suffix-aware), then "rpm" → ECU_SIG_RPM
 * via ecu_slot_from_canonical_id, then the legacy normalized name "RPM"
 * via ecu_signal_slot_name. That string is what widgets/default_layout
 * agree on. When the derived name has no mapping (e.g. body-CAN-only
 * GT86 signals like DRIVER_DOOR_OPEN), the original derived name
 * survives — losing the auto-bind but keeping the signal present. */
static void _wiz_canonicalize_signal_name(char *sname, size_t sz) {
    if (!sname || !sname[0]) return;
    const char *canon_id = ecu_signal_name_to_canonical(sname);
    if (!canon_id) return;
    ecu_signal_slot_t slot = ecu_slot_from_canonical_id(canon_id);
    if (slot >= ECU_SIG__COUNT) return;
    const char *legacy = ecu_signal_slot_name(slot);
    if (!legacy || !legacy[0]) return;
    /* In-place replace only when different — avoids needless copy. */
    if (strcmp(sname, legacy) != 0) {
        strncpy(sname, legacy, sz - 1);
        sname[sz - 1] = '\0';
    }
}

/* Resetup hygiene. Re-running the wizard is "start fresh", so the user
 * shouldn't see the previous setup's bindings prepopulated. Clear every
 * vehicle-fed channel's signal binding before (re)applying an ECU or
 * proceeding to the channels step, so only the channels the newly chosen
 * source actually provides come back populated — the rest stay present in
 * the list but unbound ("extra channels can stay, but unpopulated").
 *
 * Preserved (never cleared):
 *   - CHGRP_DASH_SYSTEM channels (fps, free heap, wifi rssi, uptime…) —
 *     fed by always-on internal signals; nothing in the wizard would ever
 *     repopulate them, so clearing would just permanently break them.
 *   - Calculated channels (calculate_fn != NULL, e.g. CALCULATED_GEAR) —
 *     self-driven, not signal-bound. */
static void _wiz_clear_vehicle_channel_bindings(void) {
    size_t n = channel_manager_count();
    size_t cleared = 0;
    for (size_t i = 0; i < n; i++) {
        channel_t *c = channel_manager_at(i);
        if (!c) continue;
        if (c->group == CHGRP_DASH_SYSTEM) continue;   /* dash internals */
        if (c->calculate_fn) continue;                 /* self-driven */
        if (c->signal_name[0] == '\0') continue;       /* already unbound */
        channel_manager_set_signal(c, "");
        cleared++;
    }
    if (cleared) {
        channel_manager_resolve_signals();
        ESP_LOGI(TAG, "resetup: cleared %u vehicle channel binding(s)",
                 (unsigned)cleared);
    }
}

/* Ensure a channel exists for a freshly-registered preset signal and bind it.
 *
 * Honours the wizard's "100% of a preset's signals become channels" contract:
 *   1. Map to a canonical channel via the ECU vocabulary (handles the 20 ECU
 *      slots + the explicit alias table in ecu_presets.c).
 *   2. Else map to a canonical channel by exact (case-insensitive) id match
 *      (e.g. "OIL_PRESSURE" -> oil_pressure, "COOLANT_PRESSURE" -> coolant_pressure).
 *   3. Else auto-create a custom_<name> channel so the signal still lands
 *      somewhere the user can see and use.
 *
 * Collision guard: if the resolved canonical channel is already bound to a
 * DIFFERENT signal during this apply, don't share it — two distinct signals on
 * one channel is exactly what the channels.json v2 heal later tears down — so
 * fall through to a custom channel and keep every signal on its own channel.
 *
 * Returns true if a channel was bound. */
static bool _wiz_ensure_channel_for_signal(const char *sname,
                                           const char *label,
                                           uint8_t decimals,
                                           uint32_t can_id, uint8_t bit_start,
                                           uint8_t bit_length, float scale,
                                           float offset, bool is_signed,
                                           uint8_t endian) {
    if (!sname || !sname[0]) return false;

    const canonical_channel_def_t *def = NULL;
    const char *canon = ecu_signal_name_to_canonical(sname);
    if (canon) def = canonical_channel_find(canon);
    if (!def)  def = canonical_channel_find_ci(sname);

    channel_t *ch = NULL;
    if (def) {
        ch = channel_manager_get(def->id);
        if (ch && ch->signal_name[0] && strcmp(ch->signal_name, sname) != 0) {
            ch = NULL;          /* canonical channel already taken — go custom */
        } else if (!ch) {
            ch = channel_manager_activate(def->id);
        }
    }

    if (!ch) {
        /* Custom fallback: "custom_" + lowercased signal name, capped to the
         * 32-char channel id buffer. */
        char cid[32];
        size_t j = 0;
        for (const char *p = "custom_"; *p && j < sizeof(cid) - 1; p++) cid[j++] = *p;
        for (size_t k = 0; sname[k] && j < sizeof(cid) - 1; k++) {
            char c = sname[k];
            cid[j++] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
        }
        cid[j] = '\0';
        ch = channel_manager_get(cid);
        if (!ch)
            ch = channel_manager_create_custom(
                cid, (label && label[0]) ? label : sname,
                CHGRP_DIAGNOSTIC, CHCARD_SCALAR, "", "", decimals, 0.0f, 100.0f);
    }

    if (!ch) return false;
    channel_manager_set_signal(ch, sname);
    /* ADR 0005: the channel OWNS its decode. Without this the channel keeps
     * whatever decode the first migration seeded, and on the next layout load
     * channel_manager_register_decoded_signals() UPSERTs that STALE decode over
     * the freshly-applied ECU decode (re-introducing the exact "stale decode
     * wins" misalignment the ADR fixes) when the user re-runs the wizard with a
     * different ECU. persist_now=false: set_signal already flushed; the bulk
     * apply does one explicit flush after the loop. */
    channel_manager_set_decode(ch, can_id, bit_start, bit_length, scale, offset,
                               is_signed, endian, NULL, false);
    return true;
}

/* Apply every preconfig item belonging to (ecu, version) to BOTH the
 * layout JSON (persistent) AND the live signal registry (immediate),
 * then trigger channel resolution so channels rebind to the new
 * signals. Returns number of items written. */
static int _apply_ecu_preconfigs(const char *ecu, const char *version) {
    /* Bulk-guard channel persistence: every set_signal / set_decode /
     * create_custom below would otherwise synchronously rewrite the WHOLE
     * channels.json (one full-file LittleFS write per edit — ~100+ for a
     * Haltech apply), stalling the LVGL task for seconds and overflowing
     * the CAN RX queue. begin/end_bulk coalesces them into ONE flush. */
    channel_manager_begin_bulk();

    /* Resetup: wipe the previous setup's bindings so only the channels
     * THIS ECU provides come back populated. The force-rebind pass below
     * only overrides channels the ECU covers and leaves the rest as-is —
     * without this clear, a prior run's bindings on uncovered channels
     * would survive and show as prepopulated on a re-run. */
    _wiz_clear_vehicle_channel_bindings();

    /* Always write into the layout that's actually loaded right now —
     * hardcoding "default" silently strands the writes when the user
     * is on any other layout. */
    char active_layout[LAYOUT_MAX_NAME] = "default";
    if (layout_manager_get_active(active_layout, sizeof(active_layout))
            != ESP_OK || !active_layout[0]) {
        strncpy(active_layout, "default", sizeof(active_layout) - 1);
    }

    /* Open the layout ONCE; signal rows batch in memory and write on commit
     * (was a full read-modify-write of the layout file per signal). A NULL
     * writer (read/parse failure) is tolerated — channels own their decode
     * (ADR 0005) so the channels.json copy is authoritative regardless. */
    ecu_layout_writer_t *lw = ecu_layout_writer_open(active_layout);
    if (!lw) {
        ESP_LOGW(TAG, "layout writer open failed for '%s' — persisting "
                 "via channels.json only", active_layout);
    }

    int applied = 0;
    for (int i = 0; i < preconfig_items_count; i++) {
        const preconfig_item_t *it = &preconfig_items[i];
        if (!it->ecu || !it->version || !it->label || !it->can_id) continue;
        if (it->obd2_pid) continue;
        if (strcmp(it->ecu, ecu) != 0) continue;
        if (strcmp(it->version, version) != 0) continue;

        char sname[32];
        _wiz_derive_signal_name(it->label, sname, sizeof(sname));
        if (!sname[0]) continue;

        /* CRITICAL: align the signal name to the canonical vocabulary
         * so existing widgets find it (see helper comment). */
        _wiz_canonicalize_signal_name(sname, sizeof(sname));

        /* Register/persist EVERY signal this preset provides — 100% of the
         * preconfig must end up assigned to a channel. _wiz_ensure_channel_for_signal
         * (below) maps each to a canonical channel where one exists and
         * auto-creates a custom_<name> channel otherwise, so nothing is dropped.
         * (Previously only canonical-mapped signals were kept; the rest were
         * silently skipped and never got a channel.) The largest preset is
         * Haltech Nexus at 68 signals — well within the 200-signal registry
         * and 128-channel caps. */
        uint32_t cid = (uint32_t)strtol(it->can_id, NULL, 16);

        /* Persist to layout JSON — batched in memory, written once on commit
         * (was a full read-modify-write of the layout file per signal). A NULL
         * writer is a no-op; the channel decode below is authoritative. */
        ecu_layout_writer_upsert(lw, sname, cid,
            it->bit_start, it->bit_length,
            it->scale, it->value_offset,
            it->is_signed, it->endianess,
            NULL, it->decimals);

        /* Apply to live registry too so channels can resolve NOW. */
        int16_t idx = signal_find_by_name(sname);
        if (idx >= 0) {
            signal_t *s = signal_get_by_index((uint16_t)idx);
            if (s) {
                s->can_id     = cid;
                s->bit_start  = it->bit_start;
                s->bit_length = it->bit_length;
                s->scale      = it->scale;
                s->offset     = it->value_offset;
                s->is_signed  = it->is_signed;
                s->endian     = it->endianess;
                /* Reset decode freshness so the next frame is treated
                 * as the first one. Without this, an existing slot
                 * whose current_value happens to match the new decode
                 * silently swallows the first notify_subscribers call
                 * — see signal_dispatch_frame's was_stale/value gate.
                 * Showed up as "RPM bound but no live value" after
                 * the ECU apply because the default-layout RPM slot
                 * survived the in-place update with is_stale already
                 * false. */
                s->is_stale       = true;
                s->current_value  = 0.0f;
                s->last_update_ms = 0;
            }
        } else {
            signal_register(sname, cid,
                it->bit_start, it->bit_length,
                it->scale, it->value_offset,
                it->is_signed, it->endianess, "");
        }

        /* Guarantee this signal has a channel — canonical where it maps,
         * custom otherwise. This both activates canonical channels the preset
         * provides (the old code only rebound already-active ones) and gives a
         * home to non-canonical signals (the old code dropped them). */
        _wiz_ensure_channel_for_signal(sname, it->label, it->decimals,
                                       cid, it->bit_start, it->bit_length,
                                       it->scale, it->value_offset,
                                       it->is_signed, it->endianess);

        applied++;
    }

    /* Commit the batched layout signals in ONE write (was one full-file
     * write per signal). */
    if (lw) {
        esp_err_t cerr = ecu_layout_writer_commit(lw);
        if (cerr != ESP_OK)
            ESP_LOGW(TAG, "layout writer commit failed for '%s': %d",
                     active_layout, cerr);
    }

    /* Re-resolve so every binding made above is subscribed consistently. */
    channel_manager_resolve_signals();
    /* End the bulk guard — flushes channels.json ONCE for all the binds made
     * above (was one synchronous full-file write per channel). MUST balance
     * the begin_bulk at the top or channel persistence stays suppressed. */
    channel_manager_end_bulk();

    /* The old "definitive force-rebind" pass that iterated canonical channels
     * and re-pointed each at a matching ECU signal is now superseded: the
     * per-signal _wiz_ensure_channel_for_signal() above already activates +
     * (re)binds the channel for every signal this preset provides, OVERRIDING
     * any stale prior binding (the wizard cleared vehicle bindings first, then
     * bound each signal fresh). Channels this ECU doesn't cover are left
     * untouched, so user-picked sources for them survive a re-run. */

    /* Save the picked ECU so the dashboard remembers it across reboots. */
    config_store_save_ecu(ecu, version);
    s_channels_changed = true;
    ESP_LOGI(TAG, "Applied %d preconfigs from %s/%s into layout '%s'",
             applied, ecu, version, active_layout);
    /* Restore a signal-derived filter — the apply just registered new
     * signals so the rebuild will use them. */
    can_set_promiscuous_mode(false);
    return applied;
}

/* Render (or re-render) the result card showing the top match + buttons.
 * Called once the probe timer elapses. */
static void _render_ecu_result(void);

static void _ecu_btn_use_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_ecu_top_match < 0) return;
    const wiz_ecu_match_t *m = &s_ecu_matches[s_ecu_top_match];
    _apply_ecu_preconfigs(m->ecu, m->version);
    _show_step_channels();
}

static void _ecu_btn_skip_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* User declined to bind anything — restore the signal-derived filter
     * we replaced at probe start. */
    can_set_promiscuous_mode(false);
    /* Resetup: skipping ECU detection means "I'll bind manually" — start
     * the channels step from a blank slate rather than the prior setup's
     * bindings, same as the apply paths do. */
    _wiz_clear_vehicle_channel_bindings();
    _show_step_channels();
}

/* The "Pick different" path opens a flat (ECU, Version) list — no
 * signal-column drill-down. The user picks the preset and the entire
 * (ECU, Version) gets applied to the layout. Matches the "pick the
 * whole vehicle, not individual signals" intent the user described. */

static void _ecu_picker_close_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_ecu_picker_sheet && lv_obj_is_valid(s_ecu_picker_sheet)) {
        lv_obj_del(s_ecu_picker_sheet);
        s_ecu_picker_sheet = NULL;
    }
}

/* Per-row click handler — `user_data` is the index into s_pick_*. */
static void _ecu_pick_row_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || (uint8_t)idx >= s_pick_count) return;
    const char *ecu = s_pick_ecus[idx];
    const char *ver = s_pick_versions[idx];
    if (!ecu || !ver) return;
    _apply_ecu_preconfigs(ecu, ver);
    if (s_ecu_picker_sheet && lv_obj_is_valid(s_ecu_picker_sheet)) {
        lv_obj_del(s_ecu_picker_sheet);
        s_ecu_picker_sheet = NULL;
    }
    _show_step_channels();
}

/* Rebuild s_pick_* from the preconfig catalog. One row per unique
 * (ecu, version) pair, OBD2 items excluded (they don't broadcast and
 * need their own activation path — users wanting OBD2 should bind the
 * specific PIDs to channels in Step 3).
 *
 * Each row's live-CAN match score (matched / total IDs) is folded in
 * from s_ecu_matches[] so the picker can sort by "best match first"
 * and surface "N%" on each row. Falls back to the unscored order when
 * the probe hasn't run yet (e.g. user opens picker before Step 2). */
static void _ecu_pick_collect(void) {
    s_pick_count = 0;
    for (int i = 0; i < preconfig_items_count; i++) {
        const preconfig_item_t *it = &preconfig_items[i];
        if (!it->ecu || !it->version) continue;
        if (it->obd2_pid) continue;
        bool dup = false;
        for (uint8_t j = 0; j < s_pick_count; j++) {
            if (strcmp(s_pick_ecus[j], it->ecu) == 0 &&
                strcmp(s_pick_versions[j], it->version) == 0) {
                dup = true; break;
            }
        }
        if (!dup && s_pick_count < WIZ_PICK_MAX) {
            s_pick_ecus[s_pick_count]     = it->ecu;
            s_pick_versions[s_pick_count] = it->version;
            s_pick_is_obd2[s_pick_count]  = false;
            s_pick_matched[s_pick_count]  = 0;
            s_pick_total[s_pick_count]    = 0;
            /* Pull the match counts from s_ecu_matches[] if the probe
             * already ran. The scoring loop computes (matched, total)
             * for every (ecu, version) in the catalog, not just the
             * top one. */
            for (uint8_t k = 0; k < s_ecu_match_count; k++) {
                const wiz_ecu_match_t *m = &s_ecu_matches[k];
                if (strcmp(m->ecu, it->ecu) == 0 &&
                    strcmp(m->version, it->version) == 0) {
                    s_pick_matched[s_pick_count] = m->matched;
                    s_pick_total[s_pick_count]   = m->total;
                    break;
                }
            }
            s_pick_count++;
        }
    }

    /* Sort by the same (matched, pct, total) tiered comparator the
     * auto-detect uses — see _compute_ecu_matches for why. Keeps the
     * picker's "best match at top" view in sync with the auto-pick so
     * the user never sees the wizard pick X but find Y at the top of
     * the picker list. */
    for (uint8_t pass = 0; pass + 1 < s_pick_count; pass++) {
        bool swapped = false;
        for (uint8_t j = 0; j + 1 + pass < s_pick_count; j++) {
            uint8_t ma = s_pick_matched[j];
            uint8_t mb = s_pick_matched[j + 1];
            int     pa = s_pick_total[j]     > 0 ? (ma * 100) / s_pick_total[j]     : 0;
            int     pb = s_pick_total[j + 1] > 0 ? (mb * 100) / s_pick_total[j + 1] : 0;
            uint8_t ta = s_pick_total[j];
            uint8_t tb = s_pick_total[j + 1];
            bool gt = (ma < mb) ||
                      (ma == mb && pa < pb) ||
                      (ma == mb && pa == pb && ta < tb);
            if (gt) {
                #define SWAP(t, a, b) do { t _t = (a); (a) = (b); (b) = _t; } while (0)
                SWAP(const char *, s_pick_ecus[j],     s_pick_ecus[j + 1]);
                SWAP(const char *, s_pick_versions[j], s_pick_versions[j + 1]);
                SWAP(bool,         s_pick_is_obd2[j],  s_pick_is_obd2[j + 1]);
                SWAP(uint8_t,      s_pick_matched[j],  s_pick_matched[j + 1]);
                SWAP(uint8_t,      s_pick_total[j],    s_pick_total[j + 1]);
                #undef SWAP
                swapped = true;
            }
        }
        if (!swapped) break;
    }
}

static void _ecu_btn_pick_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    _ecu_pick_collect();

    /* Full-overlay sheet — backdrop covers wizard card. */
    s_ecu_picker_sheet = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_ecu_picker_sheet);
    lv_obj_set_size(s_ecu_picker_sheet, lv_pct(100), lv_pct(100));
    lv_obj_center(s_ecu_picker_sheet);
    lv_obj_set_style_bg_color(s_ecu_picker_sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ecu_picker_sheet, LV_OPA_80, 0);
    lv_obj_clear_flag(s_ecu_picker_sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_ecu_picker_sheet);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, CH_CARD_W, CH_CARD_H);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, THEME_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Pick your ECU");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub,
        "Tap a make + version - we'll auto-populate every channel for it.");
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_set_style_text_font(sub, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(sub, THEME_COLOR_TEXT_MUTED, 0);

    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 32, 28);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_radius(close_btn, 4, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_t *xlbl = lv_label_create(close_btn);
    lv_label_set_text(xlbl, LV_SYMBOL_CLOSE);
    lv_obj_center(xlbl);
    lv_obj_set_style_text_color(xlbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(close_btn, _ecu_picker_close_cb, LV_EVENT_CLICKED, NULL);

    /* Scrollable list of (ECU, Version) rows. Same look as the channel
     * rows in Step 3 so the wizard reads as one coherent UI. */
    lv_obj_t *list = lv_obj_create(card);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, CH_CARD_W - 32, CH_CARD_H - 68);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, 52);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (uint8_t i = 0; i < s_pick_count; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, CH_ROW_W - 8, 44);
        lv_obj_set_style_bg_color(row, THEME_COLOR_BG, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, THEME_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, _ecu_pick_row_clicked_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);

        char buf[64];
        snprintf(buf, sizeof(buf), "%s  %s",
                 s_pick_ecus[i], s_pick_versions[i]);
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, buf);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, CH_ROW_W - 140);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_PRIMARY, 0);

        /* Live-match chip — "matched/total live". Showing the absolute
         * matched count alongside the percentage makes superset/subset
         * presets read correctly: when one preset's IDs are a strict
         * subset of another's and the bus has all the larger preset's
         * IDs, the larger preset shows a bigger numerator → wins the
         * user's eye AND the auto-pick. "no live" stays for zero hits. */
        int pct = (s_pick_total[i] > 0)
                ? (s_pick_matched[i] * 100) / s_pick_total[i]
                : 0;
        char chip_buf[24];
        bool has_live = (s_pick_matched[i] > 0);
        bool strong   = (pct >= WIZ_ECU_MATCH_PCT_MIN);
        if (has_live) {
            snprintf(chip_buf, sizeof(chip_buf), "%u/%u live",
                     s_pick_matched[i], s_pick_total[i]);
        } else {
            snprintf(chip_buf, sizeof(chip_buf), "no live");
        }
        lv_obj_t *chip = lv_label_create(row);
        lv_label_set_text(chip, chip_buf);
        lv_obj_align(chip, LV_ALIGN_RIGHT_MID, -22, 0);
        lv_obj_set_style_text_font(chip, THEME_FONT_TINY, 0);
        lv_obj_set_style_pad_hor(chip, 8, 0);
        lv_obj_set_style_pad_ver(chip, 3, 0);
        lv_obj_set_style_radius(chip, 10, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_30, 0);
        if (strong) {
            /* Strong match — accent blue. */
            lv_obj_set_style_bg_color(chip, THEME_COLOR_ACCENT_BLUE, 0);
            lv_obj_set_style_text_color(chip, THEME_COLOR_TEXT_ON_ACCENT, 0);
        } else if (has_live) {
            /* Some IDs match but below 30% — show muted to discourage
             * picking it but still give the user the data point. */
            lv_obj_set_style_bg_color(chip, THEME_COLOR_BORDER_MED, 0);
            lv_obj_set_style_text_color(chip, THEME_COLOR_TEXT_MUTED, 0);
        } else {
            /* No live frames for this ECU — clearly mark as "not seen". */
            lv_obj_set_style_bg_color(chip, THEME_COLOR_STATUS_WARN, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_20, 0);
            lv_obj_set_style_text_color(chip, THEME_COLOR_TEXT_MUTED, 0);
        }

        lv_obj_t *arrow = lv_label_create(row);
        lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_text_font(arrow, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(arrow, THEME_COLOR_TEXT_MUTED, 0);
    }

    if (s_pick_count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty,
            "Preset catalog is empty.\nImport a DBC from Web Studio first.");
        lv_obj_set_style_text_font(empty, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(empty, THEME_COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(empty);
    }
}

static void _render_ecu_result(void) {
    if (!s_step_ecu || !lv_obj_is_valid(s_step_ecu)) return;
    /* Tear down anything we already rendered as a result. */
    if (s_ecu_result_card && lv_obj_is_valid(s_ecu_result_card)) {
        lv_obj_del(s_ecu_result_card);
    }
    s_ecu_result_card = NULL;

    s_ecu_result_card = lv_obj_create(s_step_ecu);
    lv_obj_remove_style_all(s_ecu_result_card);
    lv_obj_set_size(s_ecu_result_card, BTN_W, 220);
    lv_obj_align(s_ecu_result_card, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_set_style_bg_color(s_ecu_result_card, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(s_ecu_result_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ecu_result_card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_ecu_result_card, 1, 0);
    lv_obj_set_style_radius(s_ecu_result_card, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(s_ecu_result_card, 16, 0);
    lv_obj_clear_flag(s_ecu_result_card, LV_OBJ_FLAG_SCROLLABLE);

    if (s_ecu_top_match >= 0) {
        const wiz_ecu_match_t *m = &s_ecu_matches[s_ecu_top_match];
        int pct = (m->total > 0) ? (m->matched * 100) / m->total : 0;

        lv_obj_t *head = lv_label_create(s_ecu_result_card);
        lv_label_set_text(head, "Detected");
        lv_obj_align(head, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_text_font(head, THEME_FONT_TINY, 0);
        lv_obj_set_style_text_color(head, THEME_COLOR_TEXT_MUTED, 0);

        char title_buf[64];
        snprintf(title_buf, sizeof(title_buf), "%s  %s", m->ecu, m->version);
        lv_obj_t *t = lv_label_create(s_ecu_result_card);
        lv_label_set_text(t, title_buf);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, BTN_W - 32);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 16);
        lv_obj_set_style_text_font(t, THEME_FONT_LARGE, 0);
        lv_obj_set_style_text_color(t, THEME_COLOR_TEXT_PRIMARY, 0);

        char sub[64];
        snprintf(sub, sizeof(sub), "%u of %u CAN IDs matched  (%d%% confidence)",
                 m->matched, m->total, pct);
        lv_obj_t *s = lv_label_create(s_ecu_result_card);
        lv_label_set_text(s, sub);
        lv_obj_align(s, LV_ALIGN_TOP_LEFT, 0, 50);
        lv_obj_set_style_text_font(s, THEME_FONT_TINY, 0);
        lv_obj_set_style_text_color(s, THEME_COLOR_TEXT_MUTED, 0);

        /* Use this ECU */
        lv_obj_t *use_btn = lv_btn_create(s_ecu_result_card);
        lv_obj_set_size(use_btn, BTN_W - 32, BTN_H);
        lv_obj_align(use_btn, LV_ALIGN_TOP_LEFT, 0, 84);
        lv_obj_set_style_bg_color(use_btn, THEME_COLOR_ACCENT_BLUE, 0);
        lv_obj_set_style_radius(use_btn, THEME_RADIUS_NORMAL, 0);
        lv_obj_set_style_shadow_width(use_btn, 0, 0);
        lv_obj_t *ulbl = lv_label_create(use_btn);
        lv_label_set_text(ulbl, "Use this ECU");
        lv_obj_center(ulbl);
        lv_obj_set_style_text_font(ulbl, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(ulbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
        lv_obj_add_event_cb(use_btn, _ecu_btn_use_cb, LV_EVENT_CLICKED, NULL);

        /* Pick different — secondary */
        lv_obj_t *pick_btn = lv_btn_create(s_ecu_result_card);
        lv_obj_set_size(pick_btn, BTN_W - 32, BTN_H);
        lv_obj_align(pick_btn, LV_ALIGN_TOP_LEFT, 0, 132);
        lv_obj_set_style_bg_color(pick_btn, THEME_COLOR_BG, 0);
        lv_obj_set_style_border_color(pick_btn, THEME_COLOR_BORDER, 0);
        lv_obj_set_style_border_width(pick_btn, 1, 0);
        lv_obj_set_style_radius(pick_btn, THEME_RADIUS_NORMAL, 0);
        lv_obj_set_style_shadow_width(pick_btn, 0, 0);
        lv_obj_t *plbl = lv_label_create(pick_btn);
        lv_label_set_text(plbl, "Pick a different ECU");
        lv_obj_center(plbl);
        lv_obj_set_style_text_font(plbl, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(plbl, THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_add_event_cb(pick_btn, _ecu_btn_pick_cb, LV_EVENT_CLICKED, NULL);
    } else {
        /* No match — let the user pick from the catalog or skip. */
        lv_obj_t *head = lv_label_create(s_ecu_result_card);
        lv_label_set_text(head, "No ECU detected");
        lv_obj_align(head, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_style_text_font(head, THEME_FONT_MEDIUM, 0);
        lv_obj_set_style_text_color(head, THEME_COLOR_TEXT_PRIMARY, 0);

        lv_obj_t *sub = lv_label_create(s_ecu_result_card);
        lv_label_set_text(sub,
            "Nothing on the bus matched the preset catalog. You can\n"
            "pick one manually, or skip and configure later.");
        lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, 28);
        lv_obj_set_style_text_font(sub, THEME_FONT_TINY, 0);
        lv_obj_set_style_text_color(sub, THEME_COLOR_TEXT_MUTED, 0);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(sub, BTN_W - 32);

        lv_obj_t *pick_btn = lv_btn_create(s_ecu_result_card);
        lv_obj_set_size(pick_btn, BTN_W - 32, BTN_H);
        lv_obj_align(pick_btn, LV_ALIGN_TOP_LEFT, 0, 84);
        lv_obj_set_style_bg_color(pick_btn, THEME_COLOR_ACCENT_BLUE, 0);
        lv_obj_set_style_radius(pick_btn, THEME_RADIUS_NORMAL, 0);
        lv_obj_set_style_shadow_width(pick_btn, 0, 0);
        lv_obj_t *plbl = lv_label_create(pick_btn);
        lv_label_set_text(plbl, "Pick an ECU manually");
        lv_obj_center(plbl);
        lv_obj_set_style_text_font(plbl, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(plbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
        lv_obj_add_event_cb(pick_btn, _ecu_btn_pick_cb, LV_EVENT_CLICKED, NULL);
    }

    /* Skip — always available, regardless of detection result. */
    lv_obj_t *skip_btn = lv_btn_create(s_step_ecu);
    lv_obj_set_size(skip_btn, BTN_W, BTN_H);
    lv_obj_align(skip_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(skip_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(skip_btn, 0, 0);
    lv_obj_set_style_shadow_width(skip_btn, 0, 0);
    lv_obj_t *slbl = lv_label_create(skip_btn);
    lv_label_set_text(slbl, "Skip ECU setup");
    lv_obj_center(slbl);
    lv_obj_set_style_text_font(slbl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(slbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(skip_btn, _ecu_btn_skip_cb, LV_EVENT_CLICKED, NULL);
}

/* Probe timer: every 100 ms, recompute matches against the live
 * tracker and update the progress bar. When WIZ_ECU_PROBE_MS elapses
 * (or the user explicitly skips), we render the result. */
static void _ecu_probe_tick(lv_timer_t *t) {
    (void)t;
    if (!s_step_ecu || !lv_obj_is_valid(s_step_ecu)) return;
    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - s_ecu_probe_start_ms;
    int pct = (int)((elapsed * 100) / WIZ_ECU_PROBE_MS);
    if (pct > 100) pct = 100;
    if (s_ecu_progress && lv_obj_is_valid(s_ecu_progress)) {
        lv_bar_set_value(s_ecu_progress, pct, LV_ANIM_ON);
    }

    /* Mid-scan status nudges keep the screen feeling alive. */
    if (s_ecu_status && lv_obj_is_valid(s_ecu_status)) {
        uint16_t n = can_id_tracker_count();
        char buf[64];
        snprintf(buf, sizeof(buf), "%u CAN IDs heard so far", (unsigned)n);
        lv_label_set_text(s_ecu_status, buf);
    }

    if (elapsed >= WIZ_ECU_PROBE_MS) {
        lv_timer_del(s_ecu_probe_timer);
        s_ecu_probe_timer = NULL;
        _compute_ecu_matches();
        if (s_ecu_status && lv_obj_is_valid(s_ecu_status))
            lv_obj_add_flag(s_ecu_status, LV_OBJ_FLAG_HIDDEN);
        if (s_ecu_progress && lv_obj_is_valid(s_ecu_progress))
            lv_obj_add_flag(s_ecu_progress, LV_OBJ_FLAG_HIDDEN);
        _render_ecu_result();
    }
}

static void _show_step_ecu_detect(void) {
    can_bus_test_set_ui_callback(NULL);

    /* The previous layout's filter is probably narrowed around its
     * signal IDs (e.g. MaxxECU 0x520-0x536). On a fresh car those bits
     * filter ALL the new ECU's IDs out of the hardware — tracker stays
     * empty and the match never lands. Force ACCEPT_ALL for the probe
     * window. _apply_ecu_preconfigs() flips it back on commit. */
    can_set_promiscuous_mode(true);

    /* Wipe stale tracker entries from whichever bus the device was
     * last on so the score reflects ONLY frames seen during the probe. */
    can_id_tracker_reset();

    /* Use the standard wizard card size for this step. */
    if (s_card && lv_obj_is_valid(s_card)) {
        lv_obj_set_size(s_card, CARD_W, CARD_H);
        lv_obj_center(s_card);
    }

    s_step_ecu = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_step_ecu);
    lv_obj_set_size(s_step_ecu, lv_pct(100), lv_pct(100));
    lv_obj_center(s_step_ecu);
    lv_obj_clear_flag(s_step_ecu, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_step_ecu);
    lv_label_set_text(title, "Detect Your ECU");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *sub = lv_label_create(s_step_ecu);
    lv_label_set_text(sub,
        "Step 2 of 4  -  matching live CAN IDs against the preset catalog");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_font(sub, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(sub, THEME_COLOR_TEXT_MUTED, 0);

    /* Probe progress + status line, hidden once the probe completes. */
    s_ecu_status = lv_label_create(s_step_ecu);
    lv_label_set_text(s_ecu_status, "Listening to the bus...");
    lv_obj_align(s_ecu_status, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_style_text_font(s_ecu_status, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_ecu_status, THEME_COLOR_TEXT_PRIMARY, 0);

    s_ecu_progress = lv_bar_create(s_step_ecu);
    lv_obj_set_size(s_ecu_progress, BTN_W, 8);
    lv_obj_align(s_ecu_progress, LV_ALIGN_TOP_MID, 0, 102);
    lv_bar_set_range(s_ecu_progress, 0, 100);
    lv_bar_set_value(s_ecu_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ecu_progress, THEME_COLOR_SECTION_BG,
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ecu_progress, THEME_COLOR_ACCENT_BLUE,
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ecu_progress, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ecu_progress, 4, LV_PART_INDICATOR);

    /* Kick the timer. _ecu_probe_tick will compute matches when
     * elapsed >= WIZ_ECU_PROBE_MS and swap to the result card. */
    s_ecu_probe_start_ms = lv_tick_get();
    if (s_ecu_probe_timer) lv_timer_del(s_ecu_probe_timer);
    s_ecu_probe_timer = lv_timer_create(_ecu_probe_tick, 100, NULL);
}

/* ── Step 3: Channels picker (web-style split-pane) ───────────────────────
 *
 * Mirrors the web Studio Channels modal:
 *   - LEFT: compressed list (one row = label + value), grouped by
 *     channel_group. Click a row to select it.
 *   - RIGHT: detail pane for the selected channel — hero with live value,
 *     source pill + Change button, range/decimals/threshold steppers.
 *     All edits go through channel_manager_* mutators, which is the
 *     same code path /api/channels/update uses. Add a knob in one place
 *     and both UIs pick it up.
 *
 *   The bind action funnels through channel_apply_preconfig() — the same
 *   helper /api/channels/bind-source calls. Add a preconfig row to
 *   preconfig_items[] and both pickers (this one and the web modal)
 *   surface it automatically.
 */

/* Forward declarations */
static void _close_bind_sheet(void);
static void _open_bind_sheet(const channel_t *c);
static void _channels_refresh_cb(lv_timer_t *t);
static void _select_channel_by_id(const char *id);
static void _render_detail_pane(void);
static void _refresh_hero_stats(void);
static void _populate_channels_list(void);
static void _wiz_canonical_bind_channel(channel_t *c);
static void _wiz_persist_signal_decode(const signal_t *s);
static void _rebuild_channel_row(uint16_t idx);
static void _add_custom_btn_cb(lv_event_t *e);

/* ── List rows ───────────────────────────────────────────────────────── */

/* Compressed row — no subtitle, no per-row source label. Just a label
 * left and a value right, plus a thin left accent strip indicating
 * bound/unbound. Halves the per-row paint count vs. the previous design,
 * which is the difference between scrollable and not on the dash. */
static void _style_channel_row(lv_obj_t *row, bool inactive, bool selected) {
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, CH_ROW_W, CH_ROW_H);
    lv_obj_set_style_bg_color(row,
        selected ? THEME_COLOR_ACCENT_BLUE : THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(row,
        selected ? LV_OPA_20 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row,
        selected ? THEME_COLOR_ACCENT_BLUE
                 : (inactive ? THEME_COLOR_BORDER : THEME_COLOR_ACCENT_BLUE), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_opa(row,
        selected ? LV_OPA_COVER
                 : (inactive ? LV_OPA_50 : LV_OPA_80), 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_left(row, 14, 0);
    lv_obj_set_style_pad_right(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_set_style_opa(row, inactive ? LV_OPA_70 : LV_OPA_COVER, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
}

/* In apply-to-widget mode, outline the row of the channel the target widget
 * is CURRENTLY bound to (s_apply_widget_chid) in orange so it's obvious what
 * is applied — distinct from the blue tap-selection. Re-applied on EVERY
 * (re)style (append + rebuild) so it survives selection changes / row
 * rebuilds that reset the border back to blue. */
static void _apply_bound_widget_highlight(lv_obj_t *row, const char *id) {
    if (!s_apply_to_widget_mode || !id || !s_apply_widget_chid[0]) return;
    if (strcmp(id, s_apply_widget_chid) != 0) return;
    lv_obj_set_style_border_color(row, THEME_COLOR_ACCENT_ORANGE, 0);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
}

/* 3 px left accent strip — bound = blue, unbound = muted grey. */
static void _add_row_accent_strip(lv_obj_t *row, bool inactive) {
    lv_obj_t *strip = lv_obj_create(row);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, 3, CH_ROW_H - 12);
    /* align is relative to the padded box; -11 = -(pad_left 14) + 3. */
    lv_obj_align(strip, LV_ALIGN_LEFT_MID, -11, 0);
    lv_obj_set_style_bg_color(strip,
        inactive ? THEME_COLOR_BORDER_MED : THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(strip, 2, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE);
}

static void _row_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *id = (const char *)lv_event_get_user_data(e);
    if (!id || !id[0]) return;

    /* Ghost row — activate the canonical channel, run a canonical
     * sweep so any signal already in the registry matching this id
     * auto-binds, then refresh the row so the active styling kicks in. */
    channel_t *c = channel_manager_get(id);
    if (!c && canonical_channel_exists(id)) {
        c = channel_manager_activate(id);
        if (c) {
            channel_manager_resolve_signals();
            _wiz_canonical_bind_channel(c);
            s_channels_changed = true;
            /* Re-style the activated row so accent strip + value column
             * flip to the bound look immediately. */
            for (uint16_t i = 0; i < s_channels_count; i++) {
                if (s_channels_ids[i] &&
                    strcmp(s_channels_ids[i], id) == 0) {
                    _rebuild_channel_row(i);
                    break;
                }
            }
            _refresh_hero_stats();
        }
    }
    _select_channel_by_id(id);
    _channels_refresh_cb(NULL);
}

/* Canonical sweep for ONE channel — used by the activate-on-tap path
 * so a newly-activated channel auto-binds against any registered
 * signal whose canonical id mapping returns this channel's id. */
static void _wiz_canonical_bind_channel(channel_t *c) {
    if (!c || c->signal_index >= 0 || !c->is_canonical) return;
    uint16_t reg_n = signal_get_count();
    for (uint16_t si = 0; si < reg_n; si++) {
        signal_t *s = signal_get_by_index(si);
        if (!s || !s->name[0]) continue;
        const char *cid = ecu_signal_name_to_canonical(s->name);
        if (!cid || strcmp(cid, c->id) != 0) continue;
        ESP_LOGI(TAG, "activate-bind: %s <- %s", c->id, s->name);
        channel_manager_set_signal(c, s->name);
        channel_manager_resolve_signals();
        /* Persist the bound signal's CAN decode into the layout's signals[].
         * The tap-bind only stores the signal NAME in the channel — for a
         * custom/DBC signal that lives only in the live registry, the decode
         * params (can_id/bits/scale/...) aren't anywhere in the layout, so at
         * boot _load_signals can't re-register it and the binding resolves to
         * nothing → channel preserved-but-dead. Write the decode so the
         * tap-bind survives reboot. Skip internal/OBD2 signals (can_id==0 or
         * source!=CAN): nothing to bit-decode, and OBD2 re-registers from
         * polled_pids at boot. */
        if (s->source == SIGNAL_SOURCE_CAN && s->can_id != 0) {
            _wiz_persist_signal_decode(s);
        }
        return;
    }
}

/* Refresh value column on every row + the detail-pane hero value. */
static void _channels_refresh_cb(lv_timer_t *t) {
    (void)t;
    for (uint16_t i = 0; i < s_channels_count; i++) {
        lv_obj_t *lbl = s_channels_value_lbls[i];
        if (!lbl || !lv_obj_is_valid(lbl)) continue;
        if (!s_channels_ids[i]) continue;
        const channel_t *c = channel_manager_get(s_channels_ids[i]);
        /* Ghost row — "Inactive" + disabled colour already baked at
         * creation time. Don't touch the label every tick — LVGL v8
         * forces an invalidation on every style write regardless of
         * whether the value changed, which costs ~90 dirty rects per
         * second for the full canonical list and tanks the picker
         * overlay's fps. */
        if (!c) continue;
        char buf[28];
        if (c->signal_index < 0) {
            lv_label_set_text(lbl, "-");
            lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_DISABLED, 0);
            continue;
        }
        if (c->is_stale) {
            lv_label_set_text(lbl, "...");
            lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_DISABLED, 0);
        } else {
            uint8_t d = c->decimals;
            if (d > 3) d = 3;
            /* Show the value in the channel's chosen DISPLAY unit so the list
             * matches the detail-pane hero (was rendering raw native, so a
             * °F channel read 25 in the list but 77 in the detail). */
            const char *u = c->units_display[0] ? c->units_display
                                                : c->units_native;
            float shown = unit_convert(c->current_value, c->units_native, u);
            snprintf(buf, sizeof(buf), "%.*f", d, (double)shown);
            lv_color_t col = THEME_COLOR_TEXT_PRIMARY;
            switch (c->last_zone) {
                case CHZONE_LOW_WARN:
                case CHZONE_HIGH_WARN:
                    col = THEME_COLOR_STATUS_WARN; break;
                default: break;
            }
            lv_obj_set_style_text_color(lbl, col, 0);
            lv_label_set_text(lbl, buf);
        }
    }

    /* Detail-pane hero value — only when a channel is selected. */
    if (s_detail_value_lbl && lv_obj_is_valid(s_detail_value_lbl) &&
        s_selected_ch_id[0]) {
        channel_t *c = channel_manager_get(s_selected_ch_id);
        if (c) {
            char buf[32];
            const char *u = c->units_display[0] ? c->units_display
                                                : c->units_native;
            if (c->signal_index < 0) {
                lv_label_set_text(s_detail_value_lbl, "-");
                lv_obj_set_style_text_color(s_detail_value_lbl,
                    THEME_COLOR_TEXT_DISABLED, 0);
            } else if (c->is_stale) {
                lv_label_set_text(s_detail_value_lbl, "...");
                lv_obj_set_style_text_color(s_detail_value_lbl,
                    THEME_COLOR_TEXT_DISABLED, 0);
            } else {
                uint8_t d = c->decimals;
                if (d > 3) d = 3;
                /* Hero shows the value in the chosen DISPLAY unit (kPa→psi
                 * etc.) so the number and the unit label agree. Identity /
                 * unknown pairs pass through unchanged. */
                float shown = unit_convert(c->current_value,
                                           c->units_native, u);
                snprintf(buf, sizeof(buf), "%.*f %s",
                    d, (double)shown, u);
                lv_color_t col = THEME_COLOR_TEXT_PRIMARY;
                switch (c->last_zone) {
                    case CHZONE_LOW_WARN:
                    case CHZONE_HIGH_WARN:
                        col = THEME_COLOR_STATUS_WARN; break;
                    default: break;
                }
                lv_obj_set_style_text_color(s_detail_value_lbl, col, 0);
                lv_label_set_text(s_detail_value_lbl, buf);
            }
        }
    }
}

/* Flip row styling after a bind/unbind so the strip + border re-tint
 * without a full list rebuild. */
static void _rebuild_channel_row(uint16_t idx) {
    if (idx >= s_channels_count) return;
    const char *id = s_channels_ids[idx];
    lv_obj_t *row = s_channels_rows[idx];
    if (!id || !row || !lv_obj_is_valid(row)) return;
    const channel_t *c = channel_manager_get(id);
    /* Ghost rows (no live channel) read as inactive; active channels
     * are inactive only when they have no signal bound. */
    bool inactive = (!c || c->signal_index < 0);
    bool selected = s_selected_ch_id[0] && strcmp(s_selected_ch_id, id) == 0;
    _style_channel_row(row, inactive, selected);
    _apply_bound_widget_highlight(row, id);
    /* Recolor the left accent strip (first child with width 3). */
    uint32_t n = lv_obj_get_child_cnt(row);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(row, i);
        if (ch && lv_obj_get_width(ch) == 3) {
            lv_obj_set_style_bg_color(ch,
                inactive ? THEME_COLOR_BORDER_MED : THEME_COLOR_ACCENT_BLUE, 0);
            break;
        }
    }
}

static void _btn_channels_continue_cb(lv_event_t *e) {
    (void)e;
    /* Standalone (Device Settings) — there's no Wi-Fi step to advance to;
     * commit is implicit (each bind already persisted via the shared
     * channel_apply_preconfig path), so just close. */
    if (s_standalone_channels) {
        _close_wizard(false);
        return;
    }
    _show_step3();
}

/* Reload the dashboard so the just-rebound widget re-resolves its channel
 * via from_json (the proven hot-reload path the web editor uses). Queued
 * AFTER _close_wizard's overlay del so the old screen — and the overlay it
 * parents — tear down in order, never double-freeing. */
static void _wiz_reload_dashboard(void *arg) {
    (void)arg;
    lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
    ui_Screen3_screen_init();
    lv_scr_load(ui_Screen3);
    if (old && old != ui_Screen3 && lv_obj_is_valid(old))
        lv_obj_del(old);
}

/* Deferred open of the legacy per-widget config modal (visual settings
 * during the channels transition). Runs after the channels overlay del. */
static void _wiz_open_widget_settings(void *arg) {
    widget_t *w = (widget_t *)arg;
    if (w) load_menu_screen_for_widget(w);
}

/* "Apply to widget" — bind the selected channel to the long-pressed
 * widget, persist, and reload so it re-renders on the new channel. */
static void _btn_apply_to_widget_cb(lv_event_t *e) {
    (void)e;
    widget_t *w = s_apply_target_widget;   /* capture before _close_wizard NULLs it */
    if (w && s_selected_ch_id[0]) {
        char *buf = widget_get_channel_id_buf(w);
        if (buf) {
            strncpy(buf, s_selected_ch_id, 31);
            buf[31] = '\0';
            /* Relabel the widget to the newly-bound channel so the title
             * reflects what it now shows — panels/bars/etc keep a stored
             * label that otherwise stays stale after a rebind (the "COOLANT
             * title on an RPM-bound panel" bug). Only widgets with a text
             * label get one; meter/arc/etc return NULL and are left alone. */
            char *lbuf = widget_get_label_buf(w);
            const channel_t *cc = channel_manager_get(s_selected_ch_id);
            if (lbuf && cc && cc->label[0]) {
                strncpy(lbuf, cc->label, 31);
                lbuf[31] = '\0';
            }
            dashboard_persist_layout();
            ESP_LOGI(TAG, "Applied channel '%s' to widget", s_selected_ch_id);
        }
    }
    _close_wizard(false);
    lv_async_call(_wiz_reload_dashboard, NULL);
}

/* "Widget settings" — hand off to the legacy per-widget config modal for
 * visual settings the channels editor doesn't cover (colors, needle style,
 * sizing). Phased out once those move into the channels flow. */
static void _btn_widget_settings_cb(lv_event_t *e) {
    (void)e;
    widget_t *w = s_apply_target_widget;
    _close_wizard(false);
    if (w) lv_async_call(_wiz_open_widget_settings, w);
}

/* ── Detail pane (right side) ────────────────────────────────────────────
 *
 * Mirrors the web modal's right pane: hero value, source picker, range +
 * threshold steppers. Built fresh on every selection change — cheap
 * (single pane, ~12 children) and avoids the bookkeeping of partial
 * updates. The live value at the top is refreshed by the same 500 ms
 * timer that drives the list rows (see _channels_refresh_cb).
 */

/* Helper: section heading inside the detail pane. y_offset relative to
 * the pane origin. Returns the label so the caller can stash it if
 * needed (none currently do). */
static lv_obj_t *_detail_section_label(lv_obj_t *parent, const char *text,
                                       lv_coord_t y) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_text_font(l, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(l, THEME_COLOR_ACCENT_BLUE, 0);
    return l;
}

/* Helper: thin rule under a section header. */
static void _detail_section_rule(lv_obj_t *parent, lv_coord_t y) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, CH_DETAIL_W - 20, 1);
    lv_obj_align(r, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_bg_color(r, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_40, 0);
}

/* The signal_t backing the selected channel's CAN decode, or NULL. Wizard
 * callbacks run on the LVGL task (same as signal_dispatch_frame), so direct
 * registry mutation here is race-free — no lock needed. */
static signal_t *_wiz_selected_signal(void) {
    if (!s_selected_ch_id[0]) return NULL;
    const channel_t *c = channel_manager_get(s_selected_ch_id);
    if (!c || c->signal_index < 0) return NULL;
    return signal_get_by_index((uint16_t)c->signal_index);
}

/* Snapshot of a signal's decode, captured by value so the deferred write
 * doesn't dereference a registry slot that may have moved by the time it
 * runs. */
typedef struct {
    char     layout[64];
    char     name[32];
    char     unit[16];
    bool     has_unit;
    uint32_t can_id;
    uint8_t  bit_start;
    uint8_t  bit_length;
    float    scale;
    float    offset;
    bool     is_signed;
    uint8_t  endian;
} wiz_decode_write_t;

/* Runs on a CLEAN LVGL tick (not inside the originating input-event
 * handler). The actual write does a 32 KB malloc + cJSON parse of the
 * active layout + an atomic LittleFS rewrite — far too heavy to run
 * synchronously inside a dropdown's VALUE_CHANGED while its option list is
 * still mid-close, or inside the deep lv_event dispatch stack: doing so
 * crashed the device when toggling any CAN-decode dropdown (signed, endian,
 * bit start/length). Deferring matches the web editor's path. */
static void _wiz_persist_decode_async(void *arg) {
    wiz_decode_write_t *w = (wiz_decode_write_t *)arg;
    ecu_preset_write_signal_to_layout(w->layout, w->name, w->can_id,
        w->bit_start, w->bit_length, w->scale, w->offset,
        w->is_signed, w->endian, w->has_unit ? w->unit : NULL, -1);
    free(w);
}

/* Persist an (already mutated) signal's decode into the active layout's
 * signals[] so the edit survives reboot. The live effect is immediate
 * because we mutate the registry in place; this only schedules the JSON
 * rewrite (deferred — see _wiz_persist_decode_async). Decimals are omitted
 * (-1) — those are channel-owned now. */
static void _wiz_persist_signal_decode(const signal_t *s) {
    if (!s || !s->name[0]) return;
    char layout[64];
    if (layout_manager_get_active(layout, sizeof(layout)) != ESP_OK) return;
    wiz_decode_write_t *w = calloc(1, sizeof(*w));
    if (!w) return;
    strncpy(w->layout, layout, sizeof(w->layout) - 1);
    strncpy(w->name, s->name, sizeof(w->name) - 1);
    if (s->unit[0]) { strncpy(w->unit, s->unit, sizeof(w->unit) - 1); w->has_unit = true; }
    w->can_id     = s->can_id;
    w->bit_start  = s->bit_start;
    w->bit_length = s->bit_length;
    w->scale      = s->scale;
    w->offset     = s->offset;
    w->is_signed  = s->is_signed;
    w->endian     = s->endian;
    rdm_async_call(_wiz_persist_decode_async, w);
}

static void _change_source_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_selected_ch_id[0]) return;
    const channel_t *c = channel_manager_get(s_selected_ch_id);
    if (!c) return;
    _open_bind_sheet(c);
}

/* Roll the channel back to its canonical defaults — range, decimals,
 * thresholds, units. The user's signal binding is preserved (revert
 * means "redo the numbers", not "undo my source pick"). */
static void _reset_defaults_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_selected_ch_id[0]) return;
    channel_t *c = channel_manager_get(s_selected_ch_id);
    if (!c) return;
    channel_manager_reset_to_defaults(c);
    _render_detail_pane();
}

/* ── Text-box + dropdown input rows (replace the +/- steppers) ────────────
 * Text boxes tap to open the keypad (numeric) or full keyboard (hex CAN ID);
 * dropdowns apply on selection. Channel fields write to the channel; CAN
 * decode fields write to the bound signal live + persist to the layout. */
typedef enum {
    KP_MIN = 1, KP_MAX, KP_HIGH_WARN, KP_LOW_WARN,   /* channel numeric fields */
    KP_CAN_ID, KP_SCALE, KP_OFFSET,                  /* signal decode fields   */
} kp_field_t;

/* Display-unit helpers: the detail pane SHOWS and EDITS range/threshold
 * values in the channel's chosen display unit (psi for a kPa channel etc.),
 * while the channel stores native — same model as the web editor. Identity
 * and unknown pairs pass through unchanged, so unit-less channels behave
 * exactly as before. */
static const char *_ch_disp_unit(const channel_t *c) {
    return c->units_display[0] ? c->units_display : c->units_native;
}
static float _ch_to_disp(const channel_t *c, float v) {
    return unit_convert(v, c->units_native, _ch_disp_unit(c));
}
static float _ch_to_native(const channel_t *c, float v) {
    return unit_convert(v, _ch_disp_unit(c), c->units_native);
}

static void _kp_confirmed(const char *text, void *ud) {
    kp_field_t f = (kp_field_t)(intptr_t)ud;
    channel_t *c = channel_manager_get(s_selected_ch_id);
    if (!c) return;
    bool blank = (!text || !text[0]);
    signal_t *s;
    switch (f) {
        /* Range/threshold input arrives in the DISPLAY unit — convert back
         * to native before storing (the registry compares native). */
        case KP_MIN: if (!blank) channel_manager_set_range(c, _ch_to_native(c, (float)atof(text)), c->max); break;
        case KP_MAX: if (!blank) channel_manager_set_range(c, c->min, _ch_to_native(c, (float)atof(text))); break;
        case KP_HIGH_WARN:
            if (blank) channel_manager_clear_threshold(c, CHZONE_HIGH_WARN);
            else       channel_manager_set_threshold(c, CHZONE_HIGH_WARN, _ch_to_native(c, (float)atof(text)));
            break;
        case KP_LOW_WARN:
            if (blank) channel_manager_clear_threshold(c, CHZONE_LOW_WARN);
            else       channel_manager_set_threshold(c, CHZONE_LOW_WARN, _ch_to_native(c, (float)atof(text)));
            break;
        case KP_CAN_ID:
            s = _wiz_selected_signal();
            if (s && !blank) {
                long v = strtol(text, NULL, 16);     /* hex; tolerates a 0x prefix */
                if (v < 0)     v = 0;
                if (v > 0x7FF) v = 0x7FF;
                s->can_id = (uint32_t)v;
                _wiz_persist_signal_decode(s);
            }
            break;
        case KP_SCALE:
            s = _wiz_selected_signal();
            if (s && !blank) { float v = strtof(text, NULL); if (v == 0.0f) v = 1.0f; s->scale = v; _wiz_persist_signal_decode(s); }
            break;
        case KP_OFFSET:
            s = _wiz_selected_signal();
            if (s && !blank) { s->offset = strtof(text, NULL); _wiz_persist_signal_decode(s); }
            break;
    }
    _render_detail_pane();
}

/* CAN ID is hex, so it needs the full keyboard (A-F). Mirror the numeric
 * wrapper's hidden-helper-textarea trick but leave the keyboard in text mode. */
static void _open_hex_dialog(const char *title, const char *initial, void *ud) {
    static lv_obj_t *hex_ta = NULL;
    if (!hex_ta || !lv_obj_is_valid(hex_ta)) {
        hex_ta = lv_textarea_create(lv_scr_act());
        if (!hex_ta) return;
        lv_obj_add_flag(hex_ta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(hex_ta, 1, 1);
    }
    lv_textarea_set_text(hex_ta, initial ? initial : "");
    show_text_input_dialog_ex(hex_ta, title, NULL, false, _kp_confirmed, NULL, ud);
}

static void _kp_open_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    kp_field_t f = (kp_field_t)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    channel_t *c = channel_manager_get(s_selected_ch_id);
    if (!c) return;
    char initial[24] = {0};
    const char *title = "Value";
    signal_t *s;
    switch (f) {
        /* Keypad pre-fill in the DISPLAY unit to match what the row shows;
         * _kp_confirmed converts the typed value back to native. */
        case KP_MIN: snprintf(initial, sizeof(initial), "%.*f", c->decimals, _ch_to_disp(c, c->min)); title = "Min"; break;
        case KP_MAX: snprintf(initial, sizeof(initial), "%.*f", c->decimals, _ch_to_disp(c, c->max)); title = "Max"; break;
        case KP_HIGH_WARN:
            if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH) snprintf(initial, sizeof(initial), "%.*f", c->decimals, _ch_to_disp(c, c->high_warn));
            title = "High warn (blank = off)"; break;
        case KP_LOW_WARN:
            if (c->low_warn != CHANNEL_THRESHOLD_UNSET_LOW) snprintf(initial, sizeof(initial), "%.*f", c->decimals, _ch_to_disp(c, c->low_warn));
            title = "Low warn (blank = off)"; break;
        case KP_CAN_ID:
            s = _wiz_selected_signal();
            if (s) snprintf(initial, sizeof(initial), "%lX", (unsigned long)s->can_id);
            _open_hex_dialog("CAN ID (hex)", initial, (void *)(intptr_t)f);
            return;
        case KP_SCALE:  s = _wiz_selected_signal(); if (s) snprintf(initial, sizeof(initial), "%g", s->scale);  title = "Scale";  break;
        case KP_OFFSET: s = _wiz_selected_signal(); if (s) snprintf(initial, sizeof(initial), "%g", s->offset); title = "Offset"; break;
    }
    show_numeric_input_dialog(title, initial, _kp_confirmed, NULL, (void *)(intptr_t)f);
}

typedef enum { DD_DECIMALS = 1, DD_BIT_START, DD_BIT_LEN, DD_SIGNED, DD_ENDIAN, DD_UNITS } dd_field_t;

/* Deferred full pane rebuild — used by edits whose event target would be
 * deleted by an inline re-render (dropdowns mid VALUE_CHANGED). The pane
 * validity is re-checked inside _render_detail_pane. */
static void _detail_rerender_async(void *unused) {
    (void)unused;
    _render_detail_pane();
}

/* Display-unit dropdown options for a channel: native unit first, then every
 * unit the conversion table can reach from it. Mirrors the web editor's
 * picker (index.html _chUnitFieldHTML). Returns the option count and writes
 * the newline-joined list into @buf; @sel_out gets the index of the
 * channel's current display unit (0 = native). */
static uint16_t _units_dd_options(const channel_t *c, char *buf, size_t cap,
                                  uint16_t *sel_out) {
    const char *targets[8];
    size_t n = unit_convert_targets(c->units_native, targets, 8);
    size_t pos = (size_t)snprintf(buf, cap, "%s", c->units_native);
    uint16_t sel = 0;
    for (size_t i = 0; i < n && pos < cap; i++) {
        if (c->units_display[0] && strcmp(c->units_display, targets[i]) == 0)
            sel = (uint16_t)(i + 1);
        pos += (size_t)snprintf(buf + pos, cap - pos, "\n%s", targets[i]);
    }
    if (sel_out) *sel_out = sel;
    return (uint16_t)(n + 1);
}

/* Map a units-dropdown index back to the unit string (0 = native). */
static const char *_units_dd_index_to_unit(const channel_t *c, uint16_t idx) {
    if (idx == 0) return c->units_native;
    const char *targets[8];
    size_t n = unit_convert_targets(c->units_native, targets, 8);
    return ((size_t)(idx - 1) < n) ? targets[idx - 1] : c->units_native;
}

static void _dropdown_changed_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *dd = lv_event_get_target(e);
    dd_field_t f = (dd_field_t)(intptr_t)lv_obj_get_user_data(dd);
    uint16_t idx = lv_dropdown_get_selected(dd);
    channel_t *c = channel_manager_get(s_selected_ch_id);
    if (!c) return;
    if (f == DD_DECIMALS) { channel_manager_set_decimals(c, (uint8_t)idx); return; }
    if (f == DD_UNITS) {
        channel_manager_set_units_display(c, _units_dd_index_to_unit(c, idx));
        /* Recalculate the whole pane (hero, min/max, thresholds) in the new
         * unit IMMEDIATELY — but deferred one tick: rebuilding mid
         * VALUE_CHANGED would delete the dropdown that is still settling. */
        rdm_async_call(_detail_rerender_async, NULL);
        return;
    }
    signal_t *s = _wiz_selected_signal();
    if (!s) return;
    switch (f) {
        case DD_BIT_START: s->bit_start  = (uint8_t)idx;        break;  /* 0..63 */
        case DD_BIT_LEN:   s->bit_length = (uint8_t)(idx + 1);  break;  /* 1..64 */
        case DD_SIGNED:    s->is_signed  = (idx == 1);          break;
        case DD_ENDIAN:    s->endian     = (idx == 1) ? 1 : 0;  break;  /* 0=Big,1=Little */
        default: return;
    }
    _wiz_persist_signal_decode(s);
    /* No re-render: the dropdown already shows the new selection and no other
     * row depends on it — re-rendering mid VALUE_CHANGED would delete the
     * dropdown that's still settling. */
}

/* Shared geometry for the input controls. */
#define CH_INPUT_W 160
#define CH_INPUT_H 30

/* Label + a clean dark "text box"; tap opens the keypad/keyboard. No glyph —
 * the filled input field reads as editable, and the absence of a chevron
 * distinguishes it from the dropdown rows. */
static void _make_textbox_row(lv_obj_t *parent, lv_coord_t y, const char *label_text,
                              const char *value_text, kp_field_t field) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, label_text);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, y + 8);
    lv_obj_set_style_text_font(l, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(l, THEME_COLOR_TEXT_MUTED, 0);

    lv_obj_t *box = lv_btn_create(parent);
    lv_obj_set_size(box, CH_INPUT_W, CH_INPUT_H);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, CH_DETAIL_W - 20 - CH_INPUT_W, y);
    lv_obj_set_style_bg_color(box, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_color(box, THEME_COLOR_SECTION_BG, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(box, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_shadow_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_user_data(box, (void *)(intptr_t)field);
    lv_obj_add_event_cb(box, _kp_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(box);
    lv_label_set_text(bl, value_text);
    lv_label_set_long_mode(bl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(bl, CH_INPUT_W - 22);
    lv_obj_set_style_text_font(bl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(bl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_align(bl, LV_ALIGN_LEFT_MID, 11, 0);
}

/* Label + an lv_dropdown (newline-separated options), styled to match the
 * dark text boxes; its built-in chevron signals "pick from a list". */
static void _make_dropdown_row(lv_obj_t *parent, lv_coord_t y, const char *label_text,
                               const char *options, uint16_t sel, dd_field_t field) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, label_text);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, y + 8);
    lv_obj_set_style_text_font(l, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(l, THEME_COLOR_TEXT_MUTED, 0);

    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, sel);
    lv_obj_set_size(dd, CH_INPUT_W, CH_INPUT_H);
    lv_obj_align(dd, LV_ALIGN_TOP_LEFT, CH_DETAIL_W - 20 - CH_INPUT_W, y);
    lv_obj_set_style_bg_color(dd, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_border_color(dd, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_radius(dd, 6, 0);
    lv_obj_set_style_shadow_width(dd, 0, 0);
    lv_obj_set_style_text_font(dd, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(dd, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_pad_left(dd, 11, 0);
    lv_obj_set_user_data(dd, (void *)(intptr_t)field);
    lv_obj_add_event_cb(dd, _dropdown_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Match the open list + selected highlight to the dark theme. */
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_set_style_bg_color(list, THEME_COLOR_INPUT_BG, 0);
        lv_obj_set_style_text_color(list, THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(list, THEME_FONT_SMALL, 0);
        lv_obj_set_style_border_color(list, THEME_COLOR_SECTION_BG, 0);
        lv_obj_set_style_border_width(list, 1, 0);
        lv_obj_set_style_bg_color(list, THEME_COLOR_ACCENT_BLUE, LV_PART_SELECTED | LV_STATE_CHECKED);
    }
}

/* Rebuild the detail pane based on s_selected_ch_id. Cheap to call. */
static void _render_detail_pane(void) {
    if (!s_detail_pane || !lv_obj_is_valid(s_detail_pane)) return;
    lv_obj_clean(s_detail_pane);
    s_detail_value_lbl    = NULL;
    s_detail_min_lbl      = NULL;
    s_detail_max_lbl      = NULL;
    s_detail_decimals_lbl = NULL;
    s_detail_warn_lbl     = NULL;
    s_detail_source_lbl   = NULL;

    /* Empty state */
    if (!s_selected_ch_id[0]) {
        lv_obj_t *empty = lv_label_create(s_detail_pane);
        lv_label_set_text(empty, "Pick a channel from the list");
        lv_obj_set_style_text_font(empty, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(empty, THEME_COLOR_TEXT_MUTED, 0);
        lv_obj_center(empty);
        return;
    }
    channel_t *c = channel_manager_get(s_selected_ch_id);
    if (!c) return;

    /* ── Hero (label + giant live value) ─────────────────────────────── */
    lv_obj_t *lbl = lv_label_create(s_detail_pane);
    lv_label_set_text(lbl, c->label);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, CH_DETAIL_W - 20);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_PRIMARY, 0);

    s_detail_value_lbl = lv_label_create(s_detail_pane);
    lv_label_set_text(s_detail_value_lbl, "...");
    lv_obj_set_width(s_detail_value_lbl, CH_DETAIL_W - 20);
    lv_obj_set_style_text_align(s_detail_value_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_detail_value_lbl, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_obj_set_style_text_font(s_detail_value_lbl, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(s_detail_value_lbl,
        THEME_COLOR_TEXT_DISABLED, 0);

    /* ── Source section ─────────────────────────────────────────────── */
    lv_coord_t y = 60;
    _detail_section_label(s_detail_pane, "SOURCE", y);
    _detail_section_rule(s_detail_pane, y + 14);
    y += 22;

    s_detail_source_lbl = lv_label_create(s_detail_pane);
    if (c->signal_index >= 0 && c->signal_name[0]) {
        char buf[64];
        snprintf(buf, sizeof(buf), "via %s", c->signal_name);
        lv_label_set_text(s_detail_source_lbl, buf);
        lv_obj_set_style_text_color(s_detail_source_lbl,
            THEME_COLOR_TEXT_PRIMARY, 0);
    } else {
        lv_label_set_text(s_detail_source_lbl, "No source");
        lv_obj_set_style_text_color(s_detail_source_lbl,
            THEME_COLOR_STATUS_WARN, 0);
    }
    lv_label_set_long_mode(s_detail_source_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_detail_source_lbl, CH_DETAIL_W - 110);
    lv_obj_align(s_detail_source_lbl, LV_ALIGN_TOP_LEFT, 0, y + 4);
    lv_obj_set_style_text_font(s_detail_source_lbl, THEME_FONT_SMALL, 0);

    lv_obj_t *change = lv_btn_create(s_detail_pane);
    lv_obj_set_size(change, 86, 26);
    lv_obj_align(change, LV_ALIGN_TOP_RIGHT, 0, y);
    lv_obj_set_style_bg_color(change, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(change, 4, 0);
    lv_obj_set_style_shadow_width(change, 0, 0);
    lv_obj_t *cl = lv_label_create(change);
    lv_label_set_text(cl, "Change");
    lv_obj_center(cl);
    lv_obj_set_style_text_font(cl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(cl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(change, _change_source_cb, LV_EVENT_CLICKED, NULL);
    y += 34;

    /* ── Range section ──────────────────────────────────────────────── */
    _detail_section_label(s_detail_pane, "RANGE", y);
    _detail_section_rule(s_detail_pane, y + 14);
    y += 20;

    /* Range + thresholds render AND edit in the channel's display unit —
     * _ch_to_disp here, _kp_open_cb pre-fills converted, _kp_confirmed
     * converts the typed value back to native. Same model as the web. */
    char vbuf[24];
    snprintf(vbuf, sizeof(vbuf), "%.*f", c->decimals, _ch_to_disp(c, c->min));
    _make_textbox_row(s_detail_pane, y, "Min", vbuf, KP_MIN);
    y += 34;
    snprintf(vbuf, sizeof(vbuf), "%.*f", c->decimals, _ch_to_disp(c, c->max));
    _make_textbox_row(s_detail_pane, y, "Max", vbuf, KP_MAX);
    y += 34;
    _make_dropdown_row(s_detail_pane, y, "Decimals", "0\n1\n2\n3",
                       (uint16_t)(c->decimals <= 3 ? c->decimals : 3), DD_DECIMALS);
    y += 38;

    /* Display-unit picker — only when the conversion table can actually
     * convert this channel's native unit somewhere (kPa→bar/psi/…, °C→°F/K,
     * km/h→mph). Picking a unit rebuilds the pane immediately so the hero,
     * range and thresholds all recalculate on the spot. */
    {
        char units_opts[96];
        uint16_t units_sel = 0;
        if (_units_dd_options(c, units_opts, sizeof(units_opts), &units_sel) > 1) {
            _make_dropdown_row(s_detail_pane, y, "Units", units_opts,
                               units_sel, DD_UNITS);
            y += 38;
        }
    }

    /* ── Thresholds section ─────────────────────────────────────────── */
    _detail_section_label(s_detail_pane, "THRESHOLDS", y);
    _detail_section_rule(s_detail_pane, y + 14);
    y += 20;

    const char *u = _ch_disp_unit(c);
    if (c->high_warn != CHANNEL_THRESHOLD_UNSET_HIGH)
        snprintf(vbuf, sizeof(vbuf), "%.*f %s", c->decimals,
                 _ch_to_disp(c, c->high_warn), u);
    else
        snprintf(vbuf, sizeof(vbuf), "%s", "Off");   /* unset warn = off */
    _make_textbox_row(s_detail_pane, y, "High warn", vbuf, KP_HIGH_WARN);
    y += 34;
    if (c->low_warn != CHANNEL_THRESHOLD_UNSET_LOW)
        snprintf(vbuf, sizeof(vbuf), "%.*f %s", c->decimals,
                 _ch_to_disp(c, c->low_warn), u);
    else
        snprintf(vbuf, sizeof(vbuf), "%s", "Off");
    _make_textbox_row(s_detail_pane, y, "Low warn", vbuf, KP_LOW_WARN);
    y += 38;

    /* "Reset to defaults" button under the steppers — handy when the
     * user has been experimenting with thresholds and wants to roll
     * back to the canonical numbers for this channel. Canonical-only;
     * a no-op for custom channels (handled by channel_manager). */
    if (c->is_canonical) {
        lv_obj_t *reset = lv_btn_create(s_detail_pane);
        lv_obj_set_size(reset, CH_DETAIL_W - 40, 30);
        lv_obj_align(reset, LV_ALIGN_TOP_LEFT, 0, y);
        lv_obj_set_style_bg_color(reset, THEME_COLOR_SECTION_BG, 0);
        lv_obj_set_style_border_color(reset, THEME_COLOR_BORDER_MED, 0);
        lv_obj_set_style_border_width(reset, 1, 0);
        lv_obj_set_style_border_opa(reset, LV_OPA_60, 0);
        lv_obj_set_style_radius(reset, 4, 0);
        lv_obj_set_style_shadow_width(reset, 0, 0);
        lv_obj_t *rl = lv_label_create(reset);
        lv_label_set_text(rl, "Reset to defaults");
        lv_obj_center(rl);
        lv_obj_set_style_text_font(rl, THEME_FONT_TINY, 0);
        lv_obj_set_style_text_color(rl, THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_add_event_cb(reset, _reset_defaults_cb, LV_EVENT_CLICKED, NULL);
        y += 40;
    }

    /* ── CAN DECODE section ──────────────────────────────────────────────
     * Only for channels whose bound signal is a real CAN broadcast — OBD2
     * and internal signals have no user-editable bit decode. Edits the
     * SIGNAL (not the channel); applied live + persisted to the layout. */
    signal_t *sig = (c->signal_index >= 0)
                    ? signal_get_by_index((uint16_t)c->signal_index) : NULL;
    if (sig && sig->source == SIGNAL_SOURCE_CAN) {
        _detail_section_label(s_detail_pane, "CAN DECODE", y);
        _detail_section_rule(s_detail_pane, y + 14);
        y += 20;

        char db[28];
        snprintf(db, sizeof(db), "0x%lX", (unsigned long)sig->can_id);
        _make_textbox_row(s_detail_pane, y, "CAN ID", db, KP_CAN_ID);
        y += 34;

        /* Bit start (0..63) + bit length (1..64) as dropdowns. */
        char opts_start[256], opts_len[256];
        int p = 0;
        for (int i = 0; i < 64; i++)
            p += snprintf(opts_start + p, sizeof(opts_start) - p, i ? "\n%d" : "%d", i);
        p = 0;
        for (int i = 1; i <= 64; i++)
            p += snprintf(opts_len + p, sizeof(opts_len) - p, i > 1 ? "\n%d" : "%d", i);
        _make_dropdown_row(s_detail_pane, y, "Bit start", opts_start,
                           (uint16_t)(sig->bit_start <= 63 ? sig->bit_start : 0), DD_BIT_START);
        y += 34;
        _make_dropdown_row(s_detail_pane, y, "Bit length", opts_len,
                           (uint16_t)((sig->bit_length >= 1 && sig->bit_length <= 64) ? sig->bit_length - 1 : 0), DD_BIT_LEN);
        y += 34;

        snprintf(db, sizeof(db), "%g", sig->scale);
        _make_textbox_row(s_detail_pane, y, "Scale", db, KP_SCALE);
        y += 34;
        snprintf(db, sizeof(db), "%g", sig->offset);
        _make_textbox_row(s_detail_pane, y, "Offset", db, KP_OFFSET);
        y += 34;

        _make_dropdown_row(s_detail_pane, y, "Signed", "Unsigned\nSigned",
                           sig->is_signed ? 1 : 0, DD_SIGNED);
        y += 34;
        _make_dropdown_row(s_detail_pane, y, "Endian", "Big (Motorola)\nLittle (Intel)",
                           sig->endian ? 1 : 0, DD_ENDIAN);
        y += 38;
    }

    /* Bottom spacer so the scrollable extent passes the last row by a
     * comfortable margin (otherwise the auto-scrollbar shows over the
     * last button and the LVGL scroll snap clips it). */
    lv_obj_t *spacer = lv_obj_create(s_detail_pane);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, 1, 8);
    lv_obj_align(spacer, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_CLICKABLE);

    s_channels_changed = true;
}

/* Update the hero stats line ("N bound, M unbound"). Called after any
 * bind so the user sees progress. Now counts ACTIVE bound channels
 * separately from total list rows because ghost rows aren't really
 * "unbound" — they're "not yet activated". */
static void _refresh_hero_stats(void) {
    if (!s_channels_stats_lbl || !lv_obj_is_valid(s_channels_stats_lbl)) return;
    char ecu_make[24] = {0};
    char ecu_ver[24]  = {0};
    (void)config_store_load_ecu(ecu_make, sizeof(ecu_make),
                                ecu_ver,  sizeof(ecu_ver));
    uint16_t n_active = 0, n_bound = 0;
    for (uint16_t i = 0; i < s_channels_count; i++) {
        const char *id = s_channels_ids[i];
        if (!id) continue;
        const channel_t *c = channel_manager_get(id);
        if (!c) continue;        /* ghost row */
        n_active++;
        if (c->signal_index >= 0) n_bound++;
    }
    char stat_buf[96];
    if (ecu_make[0]) {
        snprintf(stat_buf, sizeof(stat_buf),
                 "%s %s  -  %u bound of %u active  -  tap any channel",
                 ecu_make, ecu_ver, n_bound, n_active);
    } else {
        snprintf(stat_buf, sizeof(stat_buf),
                 "%u bound of %u active  -  tap any channel",
                 n_bound, n_active);
    }
    lv_label_set_text(s_channels_stats_lbl, stat_buf);
}

static void _select_channel_by_id(const char *id) {
    if (!id || !id[0]) return;
    char prev_id[32];
    strncpy(prev_id, s_selected_ch_id, sizeof(prev_id) - 1);
    prev_id[sizeof(prev_id) - 1] = '\0';

    strncpy(s_selected_ch_id, id, sizeof(s_selected_ch_id) - 1);
    s_selected_ch_id[sizeof(s_selected_ch_id) - 1] = '\0';

    /* Flip selection highlight on the old + new rows only — no list
     * rebuild. */
    for (uint16_t i = 0; i < s_channels_count; i++) {
        const char *rid = s_channels_ids[i];
        if (!rid) continue;
        if ((prev_id[0] && strcmp(rid, prev_id) == 0) ||
            strcmp(rid, s_selected_ch_id) == 0) {
            _rebuild_channel_row(i);
        }
    }
    _render_detail_pane();
    _channels_refresh_cb(NULL);
}

/* Append one row to s_channels_list_box. `id` must be a stable pointer
 * (canonical_def.id from flash, or channel.id from a live channel — the
 * channel struct's id field stays valid for the channel's lifetime).
 * `label` may be temporary; LVGL copies it. */
static void _append_channel_row(const char *id, const char *label) {
    if (s_channels_count >= WIZ_CH_MAX_ROWS) return;

    lv_obj_t *row = lv_obj_create(s_channels_list_box);
    const channel_t *c = channel_manager_get(id);
    bool ghost     = (c == NULL);
    bool inactive  = ghost || (c->signal_index < 0);
    bool selected  = s_selected_ch_id[0] && strcmp(s_selected_ch_id, id) == 0;
    _style_channel_row(row, inactive, selected);
    lv_obj_add_event_cb(row, _row_clicked_cb, LV_EVENT_CLICKED, (void *)id);
    _add_row_accent_strip(row, inactive);
    _apply_bound_widget_highlight(row, id);

    const lv_coord_t inner_w = CH_ROW_W - 24;
    const lv_coord_t val_w   = 70;
    const lv_coord_t name_w  = inner_w - val_w - 6;

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, label);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, name_w);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_font(name, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(name,
        ghost ? THEME_COLOR_TEXT_MUTED : THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, ghost ? "Inactive" : "...");
    lv_obj_set_width(val, val_w);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_text_font(val, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(val, THEME_COLOR_TEXT_DISABLED, 0);

    s_channels_rows[s_channels_count]       = row;
    s_channels_value_lbls[s_channels_count] = val;
    s_channels_ids[s_channels_count]        = id;
    s_channels_count++;
}

/* Add a section divider row. `title` is copied by LVGL into the label. */
static void _append_group_header(const char *title, bool first) {
    lv_obj_t *hdr = lv_obj_create(s_channels_list_box);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, CH_ROW_W, 22);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_top(hdr, first ? 2 : 8, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *htxt = lv_label_create(hdr);
    lv_label_set_text(htxt, title);
    lv_obj_align(htxt, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    lv_obj_set_style_text_font(htxt, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(htxt, THEME_COLOR_ACCENT_BLUE, 0);

    lv_obj_t *hrule = lv_obj_create(hdr);
    lv_obj_remove_style_all(hrule);
    lv_obj_set_size(hrule, CH_ROW_W - 4, 1);
    lv_obj_align(hrule, LV_ALIGN_BOTTOM_LEFT, 4, 0);
    lv_obj_set_style_bg_color(hrule, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(hrule, LV_OPA_30, 0);
}

/* Build the full channel list: every canonical channel as either a
 * ghost (activate-on-tap) or an active row, then any custom channels
 * the user added via "+ Add custom", then the "+ Add custom" button.
 *
 * Safe to call repeatedly — clears s_channels_list_box first. The
 * selection (s_selected_ch_id) is preserved so rebuilds keep the
 * detail pane focused on the right channel. */
/* Scroll the channel list so the SELECTED channel's row is visible — used on
 * an apply-to-widget open so the editor lands on the widget's bound channel
 * instead of the top of the list. */
static void _scroll_to_selected(void) {
    if (!s_selected_ch_id[0] || !s_channels_list_box ||
        !lv_obj_is_valid(s_channels_list_box)) return;
    lv_obj_update_layout(s_channels_list_box);
    for (uint16_t i = 0; i < s_channels_count; i++) {
        if (s_channels_ids[i] && s_channels_rows[i] &&
            lv_obj_is_valid(s_channels_rows[i]) &&
            strcmp(s_channels_ids[i], s_selected_ch_id) == 0) {
            lv_obj_scroll_to_view(s_channels_rows[i], LV_ANIM_OFF);
            return;
        }
    }
}

static void _populate_channels_list(void) {
    if (!s_channels_list_box || !lv_obj_is_valid(s_channels_list_box)) return;
    lv_obj_clean(s_channels_list_box);
    s_channels_count = 0;
    for (uint16_t i = 0; i < WIZ_CH_MAX_ROWS; i++) {
        s_channels_rows[i]       = NULL;
        s_channels_value_lbls[i] = NULL;
        s_channels_ids[i]        = NULL;
    }

    /* Canonical catalog — grouped by channel_group_t. Stable order
     * inside each group (catalog order = curated). */
    bool any_emitted = false;
    for (channel_group_t g = 0; g < CHGRP__COUNT; g++) {
        bool first_in_group = true;
        for (size_t i = 0; i < CANONICAL_CHANNEL_COUNT; i++) {
            const canonical_channel_def_t *def = &CANONICAL_CHANNELS[i];
            if (def->group != g) continue;
            if (s_channels_count >= WIZ_CH_MAX_ROWS - 4) goto done_canonical;
            if (first_in_group) {
                _append_group_header(channel_group_name(g), !any_emitted);
                first_in_group = false;
                any_emitted = true;
            }
            /* Prefer the live channel's label (user may have renamed)
             * but fall back to canonical default for ghost rows. */
            const channel_t *c = channel_manager_get(def->id);
            _append_channel_row(c ? c->id : def->id,
                                c ? c->label : def->label);
        }
    }
done_canonical: ;

    /* Custom channels — walk channel_manager for any id with the
     * "custom_" prefix and surface them in a Custom section. */
    bool custom_header_added = false;
    size_t mgr_count = channel_manager_count();
    for (size_t i = 0; i < mgr_count; i++) {
        const channel_t *c = channel_manager_at(i);
        if (!c || !channel_id_is_custom(c->id)) continue;
        if (s_channels_count >= WIZ_CH_MAX_ROWS - 2) break;
        if (!custom_header_added) {
            _append_group_header("Custom", !any_emitted);
            custom_header_added = true;
        }
        _append_channel_row(c->id, c->label);
    }

    /* Bottom sticky-ish "+ Add custom channel" button row. Lives in
     * the scroll list so it scrolls with content — the user finds it
     * by scrolling to the bottom, same as the web modal's footer. */
    lv_obj_t *add_btn = lv_btn_create(s_channels_list_box);
    lv_obj_set_size(add_btn, CH_ROW_W, 36);
    lv_obj_set_style_bg_color(add_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_border_color(add_btn, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_border_width(add_btn, 1, 0);
    lv_obj_set_style_border_opa(add_btn, LV_OPA_50, 0);
    lv_obj_set_style_radius(add_btn, 6, 0);
    lv_obj_set_style_shadow_width(add_btn, 0, 0);
    lv_obj_set_style_pad_all(add_btn, 0, 0);
    lv_obj_add_event_cb(add_btn, _add_custom_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *al = lv_label_create(add_btn);
    lv_label_set_text(al, LV_SYMBOL_PLUS "  Add custom channel");
    lv_obj_center(al);
    lv_obj_set_style_text_font(al, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(al, THEME_COLOR_ACCENT_BLUE, 0);

    /* On an apply-to-widget open, jump the list to the bound channel. */
    if (s_apply_to_widget_mode) _scroll_to_selected();
}

/* ── OBD2 gap-fill ─────────────────────────────────────────────────────── */

static void _obd2_chip_update(void);
static void _obd2_begin_probe(void);

/* Recompute the gap-set: ACTIVE channels with no source that have a
 * standard OBD2 PID equivalent. Returns the count. */
static uint8_t _obd2_compute_gaps(void) {
    s_obd2_gap_count = 0;
    size_t n = channel_manager_count();
    for (size_t i = 0; i < n && s_obd2_gap_count < WIZ_OBD2_MAX_GAPS; i++) {
        const channel_t *c = channel_manager_at(i);
        if (!c) continue;
        if (c->signal_index >= 0) continue;            /* already sourced */
        const canonical_obd2_map_t *m = canonical_channel_obd2_for(c->id);
        if (!m) continue;                              /* no OBD2 equivalent */
        s_obd2_gap_ids[s_obd2_gap_count]      = c->id;
        s_obd2_gap_signames[s_obd2_gap_count] = m->obd2_signal_name;
        s_obd2_gap_pids[s_obd2_gap_count]     = obd2_encode_pid(m->service, m->pid);
        s_obd2_gap_count++;
    }
    return s_obd2_gap_count;
}

/* Restore OBD2 polling to a given PID set (stop if empty). */
static void _obd2_apply_polling(const uint32_t *pids, uint8_t count) {
    if (count == 0) {
        if (obd2_is_running()) obd2_stop();
    } else {
        obd2_start(pids, count);
    }
}

static void _obd2_kill_timer(void) {
    if (s_obd2_probe_timer) {
        lv_timer_del(s_obd2_probe_timer);
        s_obd2_probe_timer = NULL;
    }
}

static void _obd2_close_modal(void) {
    /* Abort path: if a probe is still pending, the trial PIDs are live
     * on the bus — unwind to the original polled set before tearing
     * down so we never leave dead polls running. (On success/no-response
     * the timer is already dead and polling is the resolved set, so this
     * is a no-op.) */
    if (s_obd2_probe_timer) {
        _obd2_kill_timer();
        _obd2_apply_polling(s_obd2_orig_pids, s_obd2_orig_count);
    }
    if (s_obd2_modal && lv_obj_is_valid(s_obd2_modal)) {
        lv_obj_del(s_obd2_modal);
    }
    s_obd2_modal = s_obd2_status_lbl = s_obd2_hint_lbl = NULL;
    s_obd2_rescan_btn = s_obd2_spinner = NULL;
}

/* Show/hide the spinner + rescan button for the current modal phase. */
static void _obd2_modal_set_busy(bool busy) {
    if (s_obd2_spinner && lv_obj_is_valid(s_obd2_spinner)) {
        if (busy) lv_obj_clear_flag(s_obd2_spinner, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(s_obd2_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_obd2_rescan_btn && lv_obj_is_valid(s_obd2_rescan_btn)) {
        if (busy) lv_obj_add_flag(s_obd2_rescan_btn, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_clear_flag(s_obd2_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* End of the probe window: check which gap PIDs responded, bind those
 * channels, persist successes, unwind the rest. */
static void _obd2_probe_tick(lv_timer_t *t) {
    (void)t;
    _obd2_kill_timer();

    char layout[64];
    bool have_layout = (layout_manager_get_active(layout, sizeof(layout)) == ESP_OK);

    /* Collect successes: gap signal exists AND is currently fresh (the
     * PID is actively responding — polling is continuous so a live PID
     * never lapses to stale within a poll cycle). */
    uint32_t keep_pids[OBD2_MAX_ENABLED];
    uint8_t  keep_count = 0;
    /* Seed keep[] with the original set so we never drop pre-existing PIDs. */
    for (uint8_t i = 0; i < s_obd2_orig_count && keep_count < OBD2_MAX_ENABLED; i++)
        keep_pids[keep_count++] = s_obd2_orig_pids[i];

    uint8_t filled = 0;
    char names_buf[120];
    names_buf[0] = '\0';
    for (uint8_t i = 0; i < s_obd2_gap_count; i++) {
        const char *signame = s_obd2_gap_signames[i];
        if (!signame) continue;
        int16_t idx = signal_find_by_name(signame);
        if (idx < 0) continue;
        signal_t *s = signal_get_by_index((uint16_t)idx);
        if (!s || s->is_stale) continue;               /* no live response */

        /* Bind the channel to the OBD2 signal. */
        channel_t *c = channel_manager_get(s_obd2_gap_ids[i]);
        if (!c) c = channel_manager_activate(s_obd2_gap_ids[i]);
        if (c) {
            channel_manager_set_signal(c, signame);
            /* Keep this PID polling + persisted. */
            bool dup = false;
            for (uint8_t k = 0; k < keep_count; k++)
                if (keep_pids[k] == s_obd2_gap_pids[i]) { dup = true; break; }
            if (!dup && keep_count < OBD2_MAX_ENABLED)
                keep_pids[keep_count++] = s_obd2_gap_pids[i];
            /* Append the human label to the result line. */
            if (names_buf[0]) strncat(names_buf, ", ",
                                      sizeof(names_buf) - strlen(names_buf) - 1);
            strncat(names_buf, c->label,
                    sizeof(names_buf) - strlen(names_buf) - 1);
            filled++;
        }
    }

    if (filled > 0) {
        channel_manager_resolve_signals();
        /* Persist the surviving PID set so polling resumes on reboot. */
        _obd2_apply_polling(keep_pids, keep_count);
        if (have_layout) ecu_preset_save_obd2_pids(layout, keep_pids, keep_count);
        s_channels_changed = true;

        /* Refresh the channels list + hero + chip. */
        _populate_channels_list();
        _refresh_hero_stats();
        _channels_refresh_cb(NULL);

        _obd2_modal_set_busy(false);
        if (s_obd2_rescan_btn && lv_obj_is_valid(s_obd2_rescan_btn))
            lv_obj_add_flag(s_obd2_rescan_btn, LV_OBJ_FLAG_HIDDEN);
        if (s_obd2_status_lbl && lv_obj_is_valid(s_obd2_status_lbl)) {
            char msg[160];
            snprintf(msg, sizeof(msg), "Filled %u channel%s from OBD2:\n%s",
                     filled, filled == 1 ? "" : "s", names_buf);
            lv_label_set_text(s_obd2_status_lbl, msg);
            lv_obj_set_style_text_color(s_obd2_status_lbl,
                                        THEME_COLOR_GREEN, 0);
        }
        if (s_obd2_hint_lbl && lv_obj_is_valid(s_obd2_hint_lbl))
            lv_obj_add_flag(s_obd2_hint_lbl, LV_OBJ_FLAG_HIDDEN);
        _obd2_chip_update();
        ESP_LOGI(TAG, "OBD2 gap-fill: bound %u channels", filled);
    } else {
        /* No responses — unwind to the original polled set so we don't
         * leave dead polls running. */
        _obd2_apply_polling(s_obd2_orig_pids, s_obd2_orig_count);
        _obd2_modal_set_busy(false);
        if (s_obd2_status_lbl && lv_obj_is_valid(s_obd2_status_lbl)) {
            lv_label_set_text(s_obd2_status_lbl, "No OBD2 connection");
            lv_obj_set_style_text_color(s_obd2_status_lbl,
                                        THEME_COLOR_STATUS_WARN, 0);
        }
        if (s_obd2_hint_lbl && lv_obj_is_valid(s_obd2_hint_lbl)) {
            lv_obj_clear_flag(s_obd2_hint_lbl, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_obd2_hint_lbl,
                s_obd2_attempt >= 2
                  ? "Still nothing. Try again with the engine running -\n"
                    "some cars only answer OBD2 when running."
                  : "Make sure the ignition is ON, then Rescan.\n"
                    "Standalone ECUs (Haltech/MaxxECU) usually don't\n"
                    "support OBD2 - you can skip this.");
        }
        ESP_LOGI(TAG, "OBD2 gap-fill: no response (attempt %u)", s_obd2_attempt);
    }
}

/* Start (or restart) a probe: snapshot the current polled set, enable
 * the gap PIDs on top, and arm the response-window timer. */
static void _obd2_begin_probe(void) {
    _obd2_kill_timer();
    s_obd2_attempt++;

    /* Snapshot the current polled set once (first attempt only) so a
     * Rescan doesn't fold the trial PIDs into the "original". */
    if (s_obd2_attempt == 1) {
        char layout[64];
        s_obd2_orig_count = 0;
        if (layout_manager_get_active(layout, sizeof(layout)) == ESP_OK) {
            ecu_preset_read_obd2_pids(layout, s_obd2_orig_pids,
                                      OBD2_MAX_ENABLED, &s_obd2_orig_count);
        }
    }

    /* Trial set = original ∪ gap PIDs. */
    uint32_t trial[OBD2_MAX_ENABLED];
    uint8_t  tc = 0;
    for (uint8_t i = 0; i < s_obd2_orig_count && tc < OBD2_MAX_ENABLED; i++)
        trial[tc++] = s_obd2_orig_pids[i];
    for (uint8_t i = 0; i < s_obd2_gap_count && tc < OBD2_MAX_ENABLED; i++) {
        bool dup = false;
        for (uint8_t k = 0; k < tc; k++)
            if (trial[k] == s_obd2_gap_pids[i]) { dup = true; break; }
        if (!dup) trial[tc++] = s_obd2_gap_pids[i];
    }
    _obd2_apply_polling(trial, tc);

    _obd2_modal_set_busy(true);
    if (s_obd2_status_lbl && lv_obj_is_valid(s_obd2_status_lbl)) {
        lv_label_set_text(s_obd2_status_lbl, "Checking for OBD2...");
        lv_obj_set_style_text_color(s_obd2_status_lbl,
                                    THEME_COLOR_TEXT_PRIMARY, 0);
    }
    if (s_obd2_hint_lbl && lv_obj_is_valid(s_obd2_hint_lbl))
        lv_obj_add_flag(s_obd2_hint_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Infinite-repeat timer that deletes itself on first fire (via
     * _obd2_kill_timer at the top of _obd2_probe_tick). NOT repeat_count=1
     * — that would have LVGL auto-delete it too, double-freeing against
     * our own lv_timer_del. Single-threaded, so the cb completes its
     * self-delete before the timer could fire again. */
    s_obd2_probe_timer = lv_timer_create(_obd2_probe_tick, WIZ_OBD2_PROBE_MS, NULL);
}

static void _obd2_rescan_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _obd2_begin_probe();
}

static void _obd2_close_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _obd2_close_modal();  /* unwinds mid-probe polling internally */
}

static void _obd2_chip_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (_obd2_compute_gaps() == 0) return;
    if (s_obd2_modal) return;  /* already open */
    s_obd2_attempt = 0;

    /* Backdrop over the wizard. Owned by s_overlay (wizard lifecycle). */
    s_obd2_modal = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_obd2_modal);
    lv_obj_set_size(s_obd2_modal, lv_pct(100), lv_pct(100));
    lv_obj_center(s_obd2_modal);
    lv_obj_set_style_bg_color(s_obd2_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_obd2_modal, LV_OPA_70, 0);
    lv_obj_clear_flag(s_obd2_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *card = lv_obj_create(s_obd2_modal);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 400, 250);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, THEME_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(card, 18, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Fill gaps from OBD2");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    s_obd2_spinner = lv_spinner_create(card, 1000, 60);
    lv_obj_set_size(s_obd2_spinner, 34, 34);
    lv_obj_align(s_obd2_spinner, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_set_style_arc_color(s_obd2_spinner, THEME_COLOR_SECTION_BG,
                               LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_obd2_spinner, THEME_COLOR_ACCENT_BLUE,
                               LV_PART_INDICATOR);

    s_obd2_status_lbl = lv_label_create(card);
    lv_label_set_long_mode(s_obd2_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_obd2_status_lbl, 364);
    lv_obj_align(s_obd2_status_lbl, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_text_font(s_obd2_status_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_obd2_status_lbl, THEME_COLOR_TEXT_PRIMARY, 0);

    s_obd2_hint_lbl = lv_label_create(card);
    lv_label_set_long_mode(s_obd2_hint_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_obd2_hint_lbl, 364);
    lv_obj_align(s_obd2_hint_lbl, LV_ALIGN_TOP_LEFT, 0, 88);
    lv_obj_set_style_text_font(s_obd2_hint_lbl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_obd2_hint_lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_flag(s_obd2_hint_lbl, LV_OBJ_FLAG_HIDDEN);

    /* Rescan (left) + Close/Done (right) along the bottom. */
    s_obd2_rescan_btn = lv_btn_create(card);
    lv_obj_set_size(s_obd2_rescan_btn, 120, 36);
    lv_obj_align(s_obd2_rescan_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_obd2_rescan_btn, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(s_obd2_rescan_btn, 4, 0);
    lv_obj_set_style_shadow_width(s_obd2_rescan_btn, 0, 0);
    lv_obj_t *rl = lv_label_create(s_obd2_rescan_btn);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH "  Rescan");
    lv_obj_center(rl);
    lv_obj_set_style_text_font(rl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(rl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(s_obd2_rescan_btn, _obd2_rescan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_obd2_rescan_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 120, 36);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_radius(close_btn, 4, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_t *cl = lv_label_create(close_btn);
    lv_label_set_text(cl, "Done");
    lv_obj_center(cl);
    lv_obj_set_style_text_font(cl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(cl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(close_btn, _obd2_close_cb, LV_EVENT_CLICKED, NULL);

    _obd2_begin_probe();
}

/* Create / refresh / hide the "Fill N from OBD2" chip in the hero based
 * on the current gap-set. The chip only exists when there are gaps OBD2
 * could plausibly fill — a fully-bound layout never shows it. */
static void _obd2_chip_update(void) {
    if (!s_step_channels || !lv_obj_is_valid(s_step_channels)) return;
    uint8_t gaps = _obd2_compute_gaps();

    if (gaps == 0) {
        if (s_obd2_chip && lv_obj_is_valid(s_obd2_chip)) {
            lv_obj_del(s_obd2_chip);
        }
        s_obd2_chip = NULL;
        return;
    }

    if (!s_obd2_chip || !lv_obj_is_valid(s_obd2_chip)) {
        s_obd2_chip = lv_btn_create(s_step_channels);
        /* Match the established solid-button language ("Change" / "Apply to
         * widget"): accent-blue fill, white label, radius 4, no shadow. The
         * old ghost-outline pill (thin 60%-opacity border + blue tiny text)
         * read as washed-out next to them. */
        lv_obj_set_height(s_obd2_chip, 26);
        lv_obj_set_width(s_obd2_chip, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(s_obd2_chip, THEME_COLOR_ACCENT_BLUE, 0);
        lv_obj_set_style_bg_opa(s_obd2_chip, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_obd2_chip, 0, 0);
        lv_obj_set_style_radius(s_obd2_chip, 4, 0);
        lv_obj_set_style_shadow_width(s_obd2_chip, 0, 0);
        lv_obj_set_style_pad_hor(s_obd2_chip, 12, 0);
        lv_obj_set_style_pad_ver(s_obd2_chip, 0, 0);
        lv_obj_add_event_cb(s_obd2_chip, _obd2_chip_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(s_obd2_chip);
        lv_obj_center(lbl);
        lv_obj_set_style_text_font(lbl, THEME_FONT_TINY, 0);
        lv_obj_set_user_data(s_obd2_chip, lbl);
    }

    lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(s_obd2_chip);
    if (lbl && lv_obj_is_valid(lbl)) {
        char buf[40];
        snprintf(buf, sizeof(buf), "Fill %u from OBD2", gaps);
        lv_label_set_text(lbl, buf);
    }

    /* Sit in the header row, just left of the "Pick a channel…" text (the
     * chip's content width changes with the gap count, so re-align after
     * every label update; update_layout settles SIZE_CONTENT first). */
    lv_obj_update_layout(s_obd2_chip);
    if (s_step_chip_lbl && lv_obj_is_valid(s_step_chip_lbl))
        lv_obj_align_to(s_obd2_chip, s_step_chip_lbl, LV_ALIGN_OUT_LEFT_MID, -14, 0);
    else
        lv_obj_align(s_obd2_chip, LV_ALIGN_TOP_RIGHT, 0, 32);
}

/* Top-right "×" close — same teardown as the footer cancel/skip path. Used by
 * the on-device channels editor (both standalone and apply-to-widget mode) so
 * there's always a visible way out at the top of the modal. */
static void _btn_channels_close_cb(lv_event_t *e) {
    (void)e;
    _close_wizard(false);
}

/* Safety net: if the channels overlay is ever torn down by a path that does
 * NOT route through _close_wizard (e.g. its parent screen is deleted out from
 * under it during a dashboard reload), make sure the 500 ms refresh timer dies
 * with it. The timer pokes screen-owned label pointers every tick — letting it
 * outlive the screen is the classic freed-but-non-NULL use-after-free the
 * device_settings timer-lifecycle note warns about. _close_wizard still deletes
 * the timer up-front on the normal path; this only covers the abnormal one.
 * The matching NULL of the pointer keeps a later lv_timer_del from double-free. */
static void _channels_overlay_delete_cb(lv_event_t *e) {
    /* Only act for the overlay that is still the live one. If a fresh overlay
     * was already opened (its own timer created) while this one was pending an
     * async delete, s_overlay now points at the NEW overlay — leave its timer
     * alone. _close_wizard nulls the timer up-front on the normal path, so this
     * almost always finds nothing to do; it exists purely for the path that
     * bypasses _close_wizard. */
    if (lv_event_get_target(e) != s_overlay) return;
    if (s_channels_refresh_timer) {
        lv_timer_del(s_channels_refresh_timer);
        s_channels_refresh_timer = NULL;
    }
}

static void _show_step_channels(void) {
    can_bus_test_set_ui_callback(NULL);

    /* Tear down the ECU detect step (if we got here from there). */
    if (s_ecu_probe_timer) {
        lv_timer_del(s_ecu_probe_timer);
        s_ecu_probe_timer = NULL;
    }
    if (s_step_ecu && lv_obj_is_valid(s_step_ecu)) {
        lv_obj_del(s_step_ecu);
    }
    s_step_ecu = s_ecu_progress = s_ecu_status = s_ecu_result_card = NULL;
    s_ecu_picker_sheet = NULL;

    /* Widen the wizard card to fit the split-pane layout. Restored when
     * leaving for the WiFi step (see _show_step3). */
    lv_obj_set_size(s_card, CH_CARD_W, CH_CARD_H);
    lv_obj_center(s_card);

    s_step_channels = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_step_channels);
    lv_obj_set_size(s_step_channels, CH_CARD_W - 40, CH_CARD_H - 40);
    lv_obj_align(s_step_channels, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(s_step_channels, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Hero header (full width) ─────────────────────────────────── */
    lv_obj_t *title = lv_label_create(s_step_channels);
    lv_label_set_text(title, "Channels");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    /* Top-right "×" close — always present so the editor has a visible exit at
     * the top (matches other modals' close-btn pattern in device_settings.c).
     * Routes through the same _close_wizard(false) path the footer uses. */
    lv_obj_t *close_x = lv_btn_create(s_step_channels);
    lv_obj_set_size(close_x, 28, 28);
    lv_obj_align(close_x, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(close_x, THEME_COLOR_BTN_CLOSE, 0);
    lv_obj_set_style_bg_color(close_x, THEME_COLOR_BTN_CLOSE_PRESSED,
                              LV_STATE_PRESSED);
    lv_obj_set_style_radius(close_x, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_shadow_width(close_x, 0, 0);
    lv_obj_set_style_pad_all(close_x, 0, 0);
    lv_obj_t *close_x_lbl = lv_label_create(close_x);
    lv_label_set_text(close_x_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_x_lbl);
    lv_obj_set_style_text_font(close_x_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(close_x_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(close_x, _btn_channels_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *step_chip = lv_label_create(s_step_channels);
    lv_label_set_text(step_chip,
                      s_apply_to_widget_mode ? "Pick a channel for this widget"
                      : s_standalone_channels ? "Changes save automatically"
                                              : "Step 3 of 4");
    /* Pushed left of the close "×" (28 px + 8 px gap) so the two never overlap. */
    lv_obj_align(step_chip, LV_ALIGN_TOP_RIGHT, -(28 + 8), 4);
    lv_obj_set_style_text_font(step_chip, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(step_chip, THEME_COLOR_TEXT_MUTED, 0);
    /* The "Fill N from OBD2" chip anchors to this label's left edge. */
    s_step_chip_lbl = step_chip;

    s_channels_stats_lbl = lv_label_create(s_step_channels);
    lv_label_set_text(s_channels_stats_lbl, "");
    /* Cap width + dot-truncate so a long stats line never runs under the
     * OBD2 "Fill from..." chip pinned to the hero's right edge. */
    lv_label_set_long_mode(s_channels_stats_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_channels_stats_lbl, CH_CARD_W - 40 - 150);
    lv_obj_align(s_channels_stats_lbl, LV_ALIGN_TOP_LEFT, 0, 32);
    lv_obj_set_style_text_font(s_channels_stats_lbl, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_channels_stats_lbl,
                                THEME_COLOR_TEXT_MUTED, 0);

    /* Hairline rule across the full card under the hero. */
    lv_obj_t *rule = lv_obj_create(s_step_channels);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, CH_CARD_W - 40, 1);
    lv_obj_align(rule, LV_ALIGN_TOP_LEFT, 0, CH_HERO_H - 4);
    lv_obj_set_style_bg_color(rule, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_50, 0);

    /* ── LEFT pane: scrollable channel list ───────────────────────── */
    s_channels_list_box = lv_obj_create(s_step_channels);
    lv_obj_remove_style_all(s_channels_list_box);
    lv_obj_set_size(s_channels_list_box, CH_LIST_W, CH_BODY_H);
    lv_obj_align(s_channels_list_box, LV_ALIGN_TOP_LEFT, 0, CH_BODY_TOP);
    lv_obj_set_style_bg_opa(s_channels_list_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_channels_list_box, 0, 0);
    lv_obj_set_style_pad_row(s_channels_list_box, 4, 0);
    lv_obj_set_scroll_dir(s_channels_list_box, LV_DIR_VER);
    lv_obj_set_flex_flow(s_channels_list_box, LV_FLEX_FLOW_COLUMN);

    /* ── RIGHT pane: detail (built by _render_detail_pane) ────────── */
    s_detail_pane = lv_obj_create(s_step_channels);
    lv_obj_remove_style_all(s_detail_pane);
    lv_obj_set_size(s_detail_pane, CH_DETAIL_W, CH_BODY_H);
    lv_obj_align(s_detail_pane, LV_ALIGN_TOP_LEFT,
                 CH_LIST_W + CH_PANE_GAP, CH_BODY_TOP);
    lv_obj_set_style_bg_color(s_detail_pane, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(s_detail_pane, LV_OPA_30, 0);
    lv_obj_set_style_border_color(s_detail_pane, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_detail_pane, 1, 0);
    lv_obj_set_style_border_opa(s_detail_pane, LV_OPA_50, 0);
    lv_obj_set_style_radius(s_detail_pane, 6, 0);
    lv_obj_set_style_pad_all(s_detail_pane, 10, 0);
    /* Right-pad bumped so steppers don't sit under the scrollbar. */
    lv_obj_set_style_pad_right(s_detail_pane, 14, 0);
    lv_obj_set_scroll_dir(s_detail_pane, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_detail_pane, LV_SCROLLBAR_MODE_AUTO);
    /* Slim accent-blue scrollbar that doesn't compete with content. */
    lv_obj_set_style_bg_color(s_detail_pane, THEME_COLOR_ACCENT_BLUE,
                              LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_detail_pane, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(s_detail_pane, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(s_detail_pane, 14, LV_PART_SCROLLBAR);

    /* ── Populate the list (canonical catalog + customs + Add button) ── */
    _populate_channels_list();

    /* Auto-select the first BOUND channel so the detail pane isn't
     * empty on entry. Falls back to the first active row, then to
     * the first row (which may be a ghost). */
    if (!s_selected_ch_id[0] && s_channels_count > 0) {
        const char *pick = NULL;
        for (uint16_t i = 0; i < s_channels_count; i++) {
            const char *id = s_channels_ids[i];
            if (!id) continue;
            const channel_t *c = channel_manager_get(id);
            if (c && c->signal_index >= 0) { pick = id; break; }
        }
        if (!pick) {
            for (uint16_t i = 0; i < s_channels_count; i++) {
                const char *id = s_channels_ids[i];
                if (!id) continue;
                if (channel_manager_get(id)) { pick = id; break; }
            }
        }
        if (!pick) pick = s_channels_ids[0];
        if (pick) {
            strncpy(s_selected_ch_id, pick,
                    sizeof(s_selected_ch_id) - 1);
            s_selected_ch_id[sizeof(s_selected_ch_id) - 1] = '\0';
            for (uint16_t i = 0; i < s_channels_count; i++) {
                if (s_channels_ids[i] &&
                    strcmp(s_channels_ids[i], pick) == 0) {
                    _rebuild_channel_row(i);
                    break;
                }
            }
        }
    } else if (s_selected_ch_id[0]) {
        /* Preselected (apply-to-widget opens on the widget's current
         * channel) — highlight that row so the list reflects the detail. */
        for (uint16_t i = 0; i < s_channels_count; i++) {
            if (s_channels_ids[i] &&
                strcmp(s_channels_ids[i], s_selected_ch_id) == 0) {
                _rebuild_channel_row(i);
                break;
            }
        }
    }

    _render_detail_pane();
    _refresh_hero_stats();

    if (s_apply_to_widget_mode) {
        /* "Widget settings" (legacy config modal) only when the target has
         * quick-settings content — meter/text/arc are channels-only, so the
         * button is hidden and "Apply to widget" takes the full width. */
        bool has_qs = config_modal_has_content(s_apply_target_widget);

        if (has_qs) {
            lv_obj_t *adv = lv_btn_create(s_step_channels);
            lv_obj_set_size(adv, 150, BTN_H);
            lv_obj_align(adv, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            lv_obj_set_style_bg_color(adv, THEME_COLOR_SECTION_BG, 0);
            lv_obj_set_style_border_color(adv, THEME_COLOR_BORDER, 0);
            lv_obj_set_style_border_width(adv, 1, 0);
            lv_obj_set_style_radius(adv, THEME_RADIUS_NORMAL, 0);
            lv_obj_set_style_shadow_width(adv, 0, 0);
            lv_obj_t *adv_lbl = lv_label_create(adv);
            lv_label_set_text(adv_lbl, "Widget settings");
            lv_obj_center(adv_lbl);
            lv_obj_set_style_text_font(adv_lbl, THEME_FONT_SMALL, 0);
            lv_obj_set_style_text_color(adv_lbl, THEME_COLOR_TEXT_MUTED, 0);
            lv_obj_add_event_cb(adv, _btn_widget_settings_cb, LV_EVENT_CLICKED, NULL);
        }

        lv_obj_t *cont = lv_btn_create(s_step_channels);
        lv_obj_set_size(cont, has_qs ? (CH_CARD_W - 40 - 150 - 8)
                                     : (CH_CARD_W - 40), BTN_H);
        lv_obj_align(cont, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_set_style_bg_color(cont, THEME_COLOR_ACCENT_BLUE, 0);
        lv_obj_set_style_radius(cont, THEME_RADIUS_NORMAL, 0);
        lv_obj_set_style_shadow_width(cont, 0, 0);
        lv_obj_t *cont_lbl = lv_label_create(cont);
        lv_label_set_text(cont_lbl, "Apply to widget");
        lv_obj_center(cont_lbl);
        lv_obj_set_style_text_font(cont_lbl, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(cont_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
        lv_obj_add_event_cb(cont, _btn_apply_to_widget_cb, LV_EVENT_CLICKED, NULL);
    } else {
        /* Continue button — full-width along the bottom of the card. */
        lv_obj_t *cont = lv_btn_create(s_step_channels);
        lv_obj_set_size(cont, CH_CARD_W - 40, BTN_H);
        lv_obj_align(cont, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_bg_color(cont, THEME_COLOR_ACCENT_BLUE, 0);
        lv_obj_set_style_radius(cont, THEME_RADIUS_NORMAL, 0);
        lv_obj_set_style_shadow_width(cont, 0, 0);
        lv_obj_t *cont_lbl = lv_label_create(cont);
        lv_label_set_text(cont_lbl, s_standalone_channels ? "Done" : "Continue");
        lv_obj_center(cont_lbl);
        lv_obj_set_style_text_font(cont_lbl, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(cont_lbl, THEME_COLOR_TEXT_ON_ACCENT, 0);
        lv_obj_add_event_cb(cont, _btn_channels_continue_cb, LV_EVENT_CLICKED,
                            NULL);
    }

    /* Optional OBD2 gap-fill chip — only appears when there are active,
     * unbound channels with a standard OBD2 PID. Created last so it sits
     * above the list/detail in the hero's top-right. */
    s_obd2_chip = NULL;
    _obd2_chip_update();

    /* Live-value timer — refreshes both list values + detail hero. */
    if (s_channels_refresh_timer) lv_timer_del(s_channels_refresh_timer);
    s_channels_refresh_timer = lv_timer_create(_channels_refresh_cb, 500, NULL);
    _channels_refresh_cb(NULL);
}

/* ── Binding sheet — embedded preset_picker overlay ───────────────────── */

/* "+ Add custom channel" — opens the same preset picker overlay the
 * Change-source flow uses, but flagged so the picker's apply callback
 * creates a new custom_xxx channel instead of re-binding an existing
 * one. The picker UX is identical to a normal source pick — no
 * "creation form" needed. */
static void _add_custom_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_overlay || !lv_obj_is_valid(s_overlay)) return;
    s_creating_custom = true;
    /* Open the bind sheet with a "synthetic target" so the existing
     * code path is reused. The target field is ignored when
     * s_creating_custom is true (see _bind_sheet_apply_cb). */
    static const channel_t synth_target = {
        .id    = "(new custom)",
        .label = "(new custom channel)",
    };
    _open_bind_sheet(&synth_target);
}

/* Locate the row index for a channel id so we can refresh it after a
 * bind. Linear scan over the small array. */
static int _find_row_for_id(const char *id) {
    if (!id) return -1;
    for (uint16_t i = 0; i < s_channels_count; i++) {
        if (s_channels_ids[i] && strcmp(s_channels_ids[i], id) == 0)
            return (int)i;
    }
    return -1;
}

/* Derive a custom channel id from a preset's human label:
 *   "ANTI-LAG ACTIVE" → "custom_anti_lag_active"
 * Appends a digit suffix if needed to avoid collision with an existing
 * channel. Writes into out[out_sz]. Returns false if the derived base
 * would be empty. */
static bool _derive_custom_id(const char *label, char *out, size_t out_sz) {
    if (!label || !out || out_sz < 12) return false;
    char base[20] = {0};
    size_t j = 0;
    for (size_t i = 0; label[i] && j < sizeof(base) - 1; i++) {
        char ch = label[i];
        if (isalnum((unsigned char)ch)) {
            base[j++] = (char)tolower((unsigned char)ch);
        } else if (j > 0 && base[j - 1] != '_') {
            base[j++] = '_';
        }
    }
    while (j > 0 && base[j - 1] == '_') j--;
    base[j] = '\0';
    if (!base[0]) return false;

    snprintf(out, out_sz, "custom_%s", base);
    /* Suffix collisions: custom_xyz_2, custom_xyz_3, ... */
    if (!channel_manager_get(out)) return true;
    for (int n = 2; n < 100; n++) {
        snprintf(out, out_sz, "custom_%s_%d", base, n);
        if (!channel_manager_get(out)) return true;
    }
    return false;
}

/* preset_apply_cb_t — fires when the user picks a row in the embedded
 * picker. Two modes:
 *   1. Normal: re-bind s_bind_target_chan (canonical or pre-existing
 *      custom channel) to the picked preset signal.
 *   2. Custom-create (s_creating_custom): generate a new custom_xxx
 *      channel, bind it to the picked preset. Used by the "+ Add custom
 *      channel" flow.
 *
 * Both paths funnel through channel_apply_preconfig() — the same
 * source-of-truth shared with /api/channels/bind-source. */
static void _bind_sheet_apply_cb(const preconfig_item_t *item, void *ctx) {
    (void)ctx;
    if (!item) { _close_bind_sheet(); return; }

    channel_t *mut = NULL;
    if (s_creating_custom) {
        char custom_id[32];
        if (!_derive_custom_id(item->label, custom_id, sizeof(custom_id))) {
            ESP_LOGW(TAG, "couldn't derive a custom id from '%s'", item->label);
            _close_bind_sheet();
            return;
        }
        /* Group = DIAGNOSTIC so it groups into a sensible bucket; the
         * user can re-label / re-categorise via Web Studio. */
        mut = channel_manager_create_custom(custom_id, item->label,
                                            CHGRP_DIAGNOSTIC, CHCARD_SCALAR,
                                            "", "", 0, 0, 100);
        if (!mut) {
            ESP_LOGW(TAG, "create_custom('%s') failed", custom_id);
            _close_bind_sheet();
            return;
        }
        ESP_LOGI(TAG, "created custom channel '%s' (label '%s')",
                 custom_id, item->label);
    } else {
        const channel_t *target = s_bind_target_chan;
        if (!target) { _close_bind_sheet(); return; }
        /* channel_manager_activate is idempotent for already-active
         * canonical channels — returns the existing instance. */
        mut = channel_manager_activate(target->id);
        if (!mut) mut = channel_manager_get(target->id);
    }
    if (mut) {
        esp_err_t e = channel_apply_preconfig(mut, item);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "channel_apply_preconfig failed: %d", e);
        }
        s_channels_changed = true;
        if (s_creating_custom) {
            /* New channel needs a fresh row → rebuild the whole list.
             * Cheap because the list is just label+value rows. */
            _populate_channels_list();
            /* Auto-select the just-created custom channel. */
            strncpy(s_selected_ch_id, mut->id,
                    sizeof(s_selected_ch_id) - 1);
            s_selected_ch_id[sizeof(s_selected_ch_id) - 1] = '\0';
        } else {
            int idx = _find_row_for_id(mut->id);
            if (idx >= 0) {
                _rebuild_channel_row((uint16_t)idx);
            }
        }
        /* Refresh the detail pane + hero stats so the just-bound row
         * shows live values and the bound counter ticks up. */
        if (s_selected_ch_id[0] && strcmp(s_selected_ch_id, mut->id) == 0) {
            _render_detail_pane();
        }
        _refresh_hero_stats();
        _channels_refresh_cb(NULL);
        ESP_LOGI(TAG, "Bound channel '%s' via preset (%s/%s/%s)",
                 mut->id, item->ecu ? item->ecu : "?",
                 item->version ? item->version : "?",
                 item->label ? item->label : "?");
    }
    _close_bind_sheet();
}

static void _bind_sheet_close_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _close_bind_sheet();
}

static void _close_bind_sheet(void) {
    if (s_bind_sheet && lv_obj_is_valid(s_bind_sheet)) {
        lv_obj_del(s_bind_sheet);
    }
    s_bind_sheet = NULL;
    s_bind_target_chan = NULL;
    s_creating_custom = false;
    /* Resume the channels-list live-value timer that was paused when
     * the picker opened. */
    if (!s_channels_refresh_timer && s_step_channels &&
        lv_obj_is_valid(s_step_channels)) {
        s_channels_refresh_timer =
            lv_timer_create(_channels_refresh_cb, 500, NULL);
    }
}

static void _open_bind_sheet(const channel_t *c) {
    if (!c || !s_overlay || !lv_obj_is_valid(s_overlay)) return;
    s_bind_target_chan = c;

    /* Pause the channels-list live-value timer while the picker is
     * open. With ~90 ghost rows underneath, even no-op refresh ticks
     * cost ~90 dirty rects (style writes always invalidate in LVGL v8)
     * and that crushes the picker overlay's framerate. */
    if (s_channels_refresh_timer) {
        lv_timer_del(s_channels_refresh_timer);
        s_channels_refresh_timer = NULL;
    }

    /* Full-overlay backdrop over the wizard card. Owned by s_overlay so
     * it inherits the wizard's lifecycle (closes automatically when the
     * wizard closes). LV_OPA_COVER (was 80) so the channels list
     * behind it doesn't get redrawn every frame for the alpha blend. */
    s_bind_sheet = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_bind_sheet);
    lv_obj_set_size(s_bind_sheet, lv_pct(100), lv_pct(100));
    lv_obj_center(s_bind_sheet);
    lv_obj_set_style_bg_color(s_bind_sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_bind_sheet, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_bind_sheet, LV_OBJ_FLAG_SCROLLABLE);

    /* Sheet card — matches the widened channels card so the picker
     * has room for the 3-column ECU/Version/Signal browser. */
    lv_obj_t *card = lv_obj_create(s_bind_sheet);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, CH_CARD_W, CH_CARD_H);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, THEME_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Header: "Pick a source for <channel>" + close X */
    lv_obj_t *title = lv_label_create(card);
    char header[64];
    snprintf(header, sizeof(header), "Pick a source for %s", c->label);
    lv_label_set_text(title, header);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_MEDIUM, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *close_btn = lv_btn_create(card);
    lv_obj_set_size(close_btn, 32, 28);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, 0, -4);
    lv_obj_set_style_bg_color(close_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_radius(close_btn, 4, 0);
    lv_obj_set_style_shadow_width(close_btn, 0, 0);
    lv_obj_t *xlbl = lv_label_create(close_btn);
    lv_label_set_text(xlbl, LV_SYMBOL_CLOSE);
    lv_obj_center(xlbl);
    lv_obj_set_style_text_color(xlbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(close_btn, _bind_sheet_close_cb, LV_EVENT_CLICKED, NULL);

    /* Embedded preset picker — fills the rest of the card. Calls back
     * with the picked preconfig_item_t. */
    lv_obj_t *picker_host = lv_obj_create(card);
    lv_obj_remove_style_all(picker_host);
    lv_obj_set_size(picker_host, CH_CARD_W - 32, CH_CARD_H - 64);
    lv_obj_align(picker_host, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_clear_flag(picker_host, LV_OBJ_FLAG_SCROLLABLE);
    build_preset_picker_embedded(picker_host, CH_CARD_W - 32,
                                 CH_CARD_H - 64,
                                 _bind_sheet_apply_cb, NULL);
}

/* ── Step 4: WiFi info ────────────────────────────────────────────────── */

static void _show_step3(void) {
    can_bus_test_set_ui_callback(NULL);

    /* Tear down the channels review (timer + container) so the WiFi step
     * isn't rendered on top of a stale list with a still-ticking timer. */
    if (s_channels_refresh_timer) {
        lv_timer_del(s_channels_refresh_timer);
        s_channels_refresh_timer = NULL;
    }
    /* OBD2 gap-fill modal + probe timer (unwinds any in-flight poll). */
    _obd2_close_modal();
    s_obd2_chip = NULL;
    if (s_step_channels && lv_obj_is_valid(s_step_channels)) {
        lv_obj_del(s_step_channels);
    }
    s_step_channels      = NULL;
    s_channels_list_box  = NULL;

    /* Restore the wizard card to its original CARD_W/H — the WiFi step
     * is laid out around BTN_W=500 inside CARD_W=560. */
    if (s_card && lv_obj_is_valid(s_card)) {
        lv_obj_set_size(s_card, CARD_W, CARD_H);
        lv_obj_center(s_card);
    }
    for (int i = 0; i < WIZ_CH_MAX_ROWS; i++) {
        s_channels_value_lbls[i] = NULL;
        s_channels_rows[i]       = NULL;
        s_channels_ids[i]        = NULL;
    }
    s_channels_count = 0;
    s_creating_custom = false;

    /* Step 4 container */
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
    lv_label_set_text(sub, "Step 4 of 4  -  pick how to connect");
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
    /* Show the unit's ACTUAL hotspot password — per-device since 2026-06,
     * so the old hardcoded "rdm7dash" string would mislead every new unit. */
    rdm_ap_config_t ap_cfg;
    config_store_load_ap_config(&ap_cfg);
    lv_label_set_text_fmt(opt2_sub,
        "Connect to \"%s\"  /  password: %s\n"
        "Then open http://%s in a browser",
        ap_ssid ? ap_ssid : "RDM7-????",
        ap_cfg.password,
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
    lv_label_set_text(sub, "Step 1 of 4  -  Scanning all bitrates...");
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
    s_standalone_channels = false;  /* full onboarding flow, not channels-only */

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
    /* Kill the channels refresh timer if the overlay is deleted without going
     * through _close_wizard (see _channels_overlay_delete_cb). */
    lv_obj_add_event_cb(s_overlay, _channels_overlay_delete_cb,
                        LV_EVENT_DELETE, NULL);

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

/* Open ONLY the channels editor (the wizard's Step 3), standalone, as a
 * modal over whatever screen is active. Launched from Device Settings →
 * "Channels" card. Reuses the exact split-pane editor the wizard uses —
 * no CAN scan, no ECU detect, no Wi-Fi step. The footer reads "Done" and
 * closes the overlay; binds persist live as they're made (the shared
 * channel_apply_preconfig path), so there's nothing to "save". */
/* Shared overlay + card scaffold for the standalone channels editor. The
 * caller sets the mode flags (s_standalone_channels / s_apply_to_widget_mode
 * / s_apply_target_widget) and any preselection first; this builds the modal
 * over the active screen and jumps straight to the split-pane channels step. */
static void _build_channels_overlay(void) {
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
    /* Kill the refresh timer if the overlay is ever deleted without going
     * through _close_wizard (see _channels_overlay_delete_cb). */
    lv_obj_add_event_cb(s_overlay, _channels_overlay_delete_cb,
                        LV_EVENT_DELETE, NULL);

    s_card = lv_obj_create(s_overlay);
    lv_obj_set_size(s_card, CARD_W, CARD_H);  /* _show_step_channels widens it */
    lv_obj_center(s_card);
    lv_obj_set_style_bg_color(s_card, THEME_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_radius(s_card, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(s_card, 20, 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);

    _show_step_channels();
}

void first_run_wizard_open_channels(void) {
    if (s_overlay && lv_obj_is_valid(s_overlay)) return;
    s_standalone_channels  = true;
    s_apply_to_widget_mode = false;
    s_apply_target_widget  = NULL;
    ESP_LOGI(TAG, "Channels editor opened (standalone)");
    _build_channels_overlay();
}

/* Resolve the channel id a widget is effectively bound to: its explicit
 * channel_id if set, otherwise the channel whose signal_name matches the
 * widget's signal (legacy signal-bound widgets). Returns NULL if neither
 * resolves. */
static const char *_widget_effective_channel_id(widget_t *w) {
    if (!w) return NULL;
    const char *cid = widget_get_channel_id_buf(w);
    if (cid && cid[0]) return cid;
    const char *sig = widget_get_signal_name_buf(w);
    if (!sig || !sig[0]) return NULL;
    size_t n = channel_manager_count();
    for (size_t i = 0; i < n; i++) {
        const channel_t *c = channel_manager_at(i);
        if (c && c->signal_name[0] && strcmp(c->signal_name, sig) == 0)
            return c->id;
    }
    return NULL;
}

void first_run_wizard_open_channels_for_widget(widget_t *w) {
    if (!w) return;
    if (s_overlay && lv_obj_is_valid(s_overlay)) return;
    s_standalone_channels  = true;   /* reuse standalone scaffold + teardown */
    s_apply_to_widget_mode = true;
    s_apply_target_widget  = w;
    /* Cache the widget's effective channel and PRESELECT it so the editor
     * opens on that channel (detail pane populated, list scrolled to it,
     * orange "applied" highlight on its row). Works for signal-bound widgets
     * too, via the signal->channel fallback. */
    s_apply_widget_chid[0] = '\0';
    s_selected_ch_id[0]    = '\0';
    const char *eff = _widget_effective_channel_id(w);
    if (eff && eff[0]) {
        strncpy(s_apply_widget_chid, eff, sizeof(s_apply_widget_chid) - 1);
        s_apply_widget_chid[sizeof(s_apply_widget_chid) - 1] = '\0';
        strncpy(s_selected_ch_id, eff, sizeof(s_selected_ch_id) - 1);
        s_selected_ch_id[sizeof(s_selected_ch_id) - 1] = '\0';
    }
    ESP_LOGI(TAG, "Channels editor opened (apply-to-widget, ch='%s')",
             s_apply_widget_chid[0] ? s_apply_widget_chid : "(none)");
    _build_channels_overlay();
}
