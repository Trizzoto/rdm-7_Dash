/* first_run_wizard.c - four-step onboarding: CAN scan, ECU auto-detect,
 * Channels review, WiFi options.
 *
 * Runs CAN bitrate scan inline (step 1), auto-detects the ECU by matching
 * frames against the preconfig catalog (step 2; "My car uses OBD2" branches
 * to an OBD2 screen there, and a preset that reads part of the car over OBD2
 * sets that up in the background — ADR-0073), shows the channels that
 * preset just set up — the car's own first, then the rest of the catalogue
 * (step 3, ADR-0071) — then connection options (step 4). User can skip at any point — the same screens are reachable
 * from Device Settings later. NVS flag marks completion on Finish Setup
 * only; plain Skip lets the wizard return on the next boot.
 *
 * The legacy `ecu_presets[]` static table has been replaced everywhere
 * the wizard touches it: ECU auto-detect uses `preconfig_items[]` (same
 * one the web Studio uses, dynamically extended when DBCs are imported)
 * and the channels picker uses it too. New signal libraries land
 * everywhere without code changes.
 *
 * Look: a kit page (ADR-0075). The overlay is opaque on the palette
 * background with the kit's brand bar across the top — the step's name as
 * the title, "Step N of M" in the status strip, Close where that step has a
 * way out — and each step lays itself out in the body under it. Sheets
 * (ECU picker, source picker, Calculate, OBD2 scan) are kit-styled cards
 * over a dimmed backdrop, but stay CHILDREN OF THE OVERLAY rather than
 * uk_popup()s on lv_layer_top(): the keypad/keyboard dialogs they open are
 * built on lv_scr_act() and would land underneath a top-layer popup, and
 * hiding the overlay for the WiFi screen has to hide them too. */

#include "first_run_wizard.h"
#include "esp_attr.h"

#include "esp_log.h"
#include <ctype.h>
#include <string.h>

#include "../theme.h"
#include "kit/ui_kit.h"
#include "../../system/rdm_lv_async.h"
#include "../../storage/config_store.h"
#include "../../can/can_bus_test.h"
#include "../../can/can_id_tracker.h"
#include "../../can/can_manager.h"
#include "../../can/obd2.h"
#include "../../data/channel_manager.h"
#include "../../data/channel_math.h"
#include "../../data/channel_source_apply.h"
#include "../../data/obd2_autosetup.h"
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
/* The glass is a fixed 800x480. The body (s_card) is the area under the kit
 * bar, inset by the kit's body padding; every step lays out inside it. */
#define WIZ_SCR_W    800
#define WIZ_SCR_H    480
#define WIZ_BODY_W   (WIZ_SCR_W - 2 * UK_PAD)              /* 776 */
#define WIZ_BODY_H   (WIZ_SCR_H - UK_BAR_H - 2 * UK_PAD)   /* 404 */
#define BTN_W    500
#define BTN_H    UK_BTN_H

/* Sheets (ECU picker, source picker) — kit popup cards nearly the size of
 * the glass so the picker has room for its 3-column browser. */
#define SHEET_W      760
#define SHEET_H      460
#define SHEET_PAD     18   /* the kit popup's padding */

/* Channels step: split pane (list left, detail right) filling the body.
 *
 * Layout — mirrors the web Studio's Channels modal:
 *   ┌─ Hero (stats line + OBD2 chip) ───────────────────────────────┐
 *   │ ┌── List (left) ────┐ ┌── Detail (right) ─────────────────────┐│
 *   │ │ scrollable rows   │ │ hero value, source pill, range edits  ││
 *   │ └───────────────────┘ └───────────────────────────────────────┘│
 *   │ ┌── Continue (full width) ────────────────────────────────────┐│
 *   └────────────────────────────────────────────────────────────────┘
 * The title and step chip live in the kit bar above.
 *
 * Row width was 720 (full card). Compressing to ~336 halves the dirty
 * area per scroll/repaint — the framerate cost of the old design was
 * unacceptable on the dash. */
#define CH_W           WIZ_BODY_W
#define CH_H           WIZ_BODY_H
#define CH_HERO_H       34   /* stats line + the OBD2 chip */
#define CH_BODY_TOP    (CH_HERO_H + 6)
#define CH_FOOT_H      (BTN_H + 10)
#define CH_BODY_H      (CH_H - CH_BODY_TOP - CH_FOOT_H)
#define CH_PANE_GAP     12
#define CH_LIST_W      336
#define CH_DETAIL_W    (CH_W - CH_LIST_W - CH_PANE_GAP)
#define CH_ROW_W       (CH_LIST_W - 8)  /* clear of the list's scrollbar */
#define CH_ROW_H        40              /* compressed — label + value, no subtitle */

/* ── State ────────────────────────────────────────────────────────────── */
static lv_obj_t  *s_overlay    = NULL;
static lv_obj_t  *s_card       = NULL;   /* the body under the bar */
/* The kit brand bar and the parts of it each step repaints. Children of
 * s_overlay, so they go with it. */
static lv_obj_t  *s_bar        = NULL;
static lv_obj_t  *s_bar_title  = NULL;
static lv_obj_t  *s_bar_status = NULL;
static lv_obj_t  *s_bar_close  = NULL;

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
/* Wizard-OWNED copies of every row's channel id. Rows used to point at
 * channel_t::id — memory that dies with the record. That held while
 * records were immortal; channel removal (ADR-0034) ends that, and a
 * freed record would have left every one of its row references dangling
 * (the rpm-bar stale-globals bug class). 144 × 32 B of .bss buys ids
 * that outlive any record. */
static EXT_RAM_BSS_ATTR char       s_channels_id_pool[WIZ_CH_MAX_ROWS][32];
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

/* ── OBD2 scan state ───────────────────────────────────────────────────
 * Ask the car what it supports (obd2_discovery_start — Mode 01 bitmask
 * chain with bitrate + addressing auto-search), resolve the answers into
 * channels through the shared channel_obd2_matches(), and offer them.
 * On a "Scan for OBD2" chip tap from the channels editor. The wizard's own
 * OBD2 setup goes through data/obd2_autosetup.c instead (ADR-0073). */
static EXT_RAM_BSS_ATTR obd2_channel_match_t s_obd2_matches[CH_OBD2_MATCH_MAX];
static bool        s_obd2_pick[CH_OBD2_MATCH_MAX] = {false};
static size_t      s_obd2_match_count   = 0;
static lv_obj_t   *s_obd2_chip          = NULL;   /* "Scan for OBD2" pill */
static lv_obj_t   *s_obd2_modal         = NULL;   /* scan/result sheet (its backdrop) */
static lv_obj_t   *s_obd2_status_lbl    = NULL;
static lv_obj_t   *s_obd2_list          = NULL;   /* offer list */
static lv_obj_t   *s_obd2_add_btn       = NULL;
static lv_obj_t   *s_obd2_rescan_btn    = NULL;
static lv_obj_t   *s_obd2_spinner       = NULL;
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

/* Step 2: ECU auto-detect — match seen CAN frames against the preconfig
 * catalog, scored by (matched frames) / (total frames for that ECU). A frame
 * is (can_id, mux_value): for the usual single-payload ECU that is just its
 * id, but a multiplexed one puts every payload on one id and tags each with a
 * frame index, so ids alone can't measure how much of it is present. The top
 * (ECU, version) wins and is offered to the user with a "Use this" CTA.
 * Per-(ECU, version) state lives in s_ecu_matches; UI handles live in
 * s_step_ecu_*. */
#define WIZ_ECU_MAX           16
/* Auto-pick floor — *absolute* matched frames rather than percentage.
 * Percentage-floor gating drops wide presets whose IDs aren't all
 * broadcast: MaxxECU covers 21 IDs but a real unit might only emit 5 of
 * them (5/21 = 24%), so a pct-floor at 30% would kill it before the
 * matched-count tiebreaker can run — and any narrower preset sharing
 * those 5 IDs would score higher on percentage alone.
 *
 * Three distinct 11-bit frames as the gate is statistically robust (CAN
 * ID coincidence ≈ (tracked_n/2048)^3 — under 1e-6 for typical bus
 * snapshots; a multiplexed preset additionally needs three specific frame
 * indices on that id, which is stricter still) and lets the tiered
 * (matched, pct, total) comparator decide the winner.
 * WIZ_ECU_MATCH_PCT_MIN stays defined for the picker chip's
 * "strong vs weak" colouring. */
#define WIZ_ECU_MIN_MATCHED    3
#define WIZ_ECU_MATCH_PCT_MIN 30   /* Picker chip colour threshold */
#define WIZ_ECU_PROBE_MS      4500 /* Long enough for slow 1 Hz broadcasters */

/* One distinct frame a preset expects on the bus. mux_len == 0 means the id
 * carries a single fixed payload, in which case mux_val is meaningless. */
typedef struct {
    uint32_t can_id;
    uint8_t  mux_len;
    uint16_t mux_val;
} wiz_frame_t;

typedef struct {
    char    ecu[24];
    char    version[24];
    uint8_t matched;
    uint8_t total;
} wiz_ecu_match_t;

static EXT_RAM_BSS_ATTR wiz_ecu_match_t s_ecu_matches[WIZ_ECU_MAX];
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

/* OBD2 screen — a branch of the ECU step for a car with no preset, not a
 * step every customer sees (ADR-0073). See _show_step_obd2. */
static lv_obj_t  *s_step_obd2            = NULL;
static lv_obj_t  *s_obd2_step_status     = NULL;
static lv_obj_t  *s_obd2_step_spinner    = NULL;
static lv_obj_t  *s_obd2_step_rescan_btn = NULL;

/* Step 4: Channels review */
/* (state already declared above — ECU detect + OBD2 steps inserted
 * between scan and channels.) */

/* Step 5: WiFi info */
static lv_obj_t  *s_step3         = NULL;

/* Sub-overlay for the per-channel source picker (Step 3). Holds the
 * preset_picker_embedded preconfig catalog. Owned by the wizard. */
static lv_obj_t  *s_bind_sheet            = NULL;
static const channel_t *s_bind_target_chan = NULL;

/* Calculate (math channel) sheet — declared here so the wizard teardown can
 * null the pointers; the builder + callbacks live further down. */
static lv_obj_t *s_math_sheet     = NULL;
static lv_obj_t *s_math_a_dd      = NULL;
static lv_obj_t *s_math_op_dd     = NULL;
static lv_obj_t *s_math_b_dd      = NULL;
static lv_obj_t *s_math_bval_lbl  = NULL;  /* constant value shown on its button */
static lv_obj_t *s_math_op2_dd    = NULL;  /* third term: operator */
static lv_obj_t *s_math_c_dd      = NULL;  /* third term: operand (or "none") */
static lv_obj_t *s_math_cval_lbl  = NULL;

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
    s_bar = s_bar_title = s_bar_status = s_bar_close = NULL;
    s_step_ecu = NULL;
    s_step_obd2 = NULL;
    s_obd2_step_status = s_obd2_step_spinner = s_obd2_step_rescan_btn = NULL;
    /* A setup still running carries on without an audience — it binds what
     * the car answers either way — but must not call into freed screens. */
    obd2_autosetup_listen(NULL, NULL);
    s_ecu_progress = s_ecu_status = s_ecu_result_card = NULL;
    s_ecu_picker_sheet = NULL;
    s_channels_list_box = NULL;
    s_bind_sheet = NULL;
    s_bind_target_chan = NULL;
    s_math_sheet = s_math_a_dd = s_math_op_dd = s_math_b_dd = s_math_bval_lbl = NULL;
    s_math_op2_dd = s_math_c_dd = s_math_cval_lbl = NULL;
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
static void _show_step_obd2(void);
static void _close_bind_sheet(void);

/* A full-width (BTN_W) kit button centred at @y in @parent. The label is
 * uk_btn_label()'s child 1 (child 0 is the icon slot) — change it with
 * uk_btn_set_text(), never lv_obj_get_child(btn, 0). */
static lv_obj_t *_make_btn(lv_obj_t *parent, uk_icon_t icon, const char *text,
                           uk_btn_kind_t kind, lv_coord_t y,
                           lv_event_cb_t cb) {
    lv_obj_t *btn = uk_btn(parent, icon, text, kind, cb, NULL);
    lv_obj_set_size(btn, BTN_W, BTN_H);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, y);
    return btn;
}

/* ── Kit shell helpers ────────────────────────────────────────────────── */

/* The condensed face is drawn in capitals (uk_label does this at creation;
 * a relabel has to do it again). */
static void _wiz_set_caps(lv_obj_t *label, const char *text) {
    char buf[64];
    size_t i = 0;
    for (; text && text[i] && i < sizeof(buf) - 1; i++)
        buf[i] = (char)toupper((unsigned char)text[i]);
    buf[i] = '\0';
    lv_label_set_text(label, buf);
}

/* Repaint the bar for the step being built: its name, "Step N of M" (or
 * NULL for none) in the status strip, and whether Close is offered. */
static void _wiz_bar_set(const char *title, const char *status, bool can_close) {
    if (s_bar_title) _wiz_set_caps(s_bar_title, title);
    if (s_bar_status) {
        lv_obj_t *item = lv_obj_get_parent(s_bar_status);
        if (status && status[0]) {
            lv_label_set_text(s_bar_status, status);
            if (item) lv_obj_clear_flag(item, LV_OBJ_FLAG_HIDDEN);
        } else if (item) {
            lv_obj_add_flag(item, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_bar_close) {
        if (can_close) lv_obj_clear_flag(s_bar_close, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_bar_close, LV_OBJ_FLAG_HIDDEN);
    }
}

/* The bar's Close — the same teardown the old top-right "×" used. Shown on
 * the channels step (standalone, apply-to-widget and the full flow alike),
 * so the editor always has a visible way out at the top. */
static void _bar_close_cb(lv_event_t *e) {
    (void)e;
    _close_wizard(false);
}

/* Thin kit progress track: raised track, accent fill, round ends. */
static void _wiz_style_bar(lv_obj_t *bar) {
    lv_obj_set_style_bg_color(bar, ui_pal->raised_hi, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, THEME_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
}

static void _wiz_style_spinner(lv_obj_t *sp) {
    lv_obj_set_style_arc_color(sp, ui_pal->raised_hi, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sp, THEME_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 4, LV_PART_INDICATOR);
}

/* Kit dropdown in the small face the 30 px rows are sized for. Call after
 * lv_dropdown_create (its option list exists from creation). */
static void _wiz_style_dropdown(lv_obj_t *dd) {
    uk_style_dropdown(dd);
    lv_obj_set_style_text_font(dd, THEME_FONT_SMALL, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) lv_obj_set_style_text_font(list, THEME_FONT_SMALL, 0);
}

/* A tap-to-edit value box (a button that opens the keypad) drawn as a kit
 * text field. The default theme's button look is removed first so the kit
 * field style is all there is. */
static void _wiz_style_textbox(lv_obj_t *box) {
    lv_obj_remove_style_all(box);
    uk_style_textarea(box);
    lv_obj_set_style_bg_color(box, THEME_COLOR_SECTION_BG, LV_STATE_PRESSED);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
}

/* A sheet: a kit popup card (surface, strong hairline, radius 14, pad 18,
 * caps title top-left, square X top-right) over a backdrop — both children
 * of s_overlay, see the note at the top of the file. Returns the backdrop,
 * which is what the callers keep and delete; @card_out gets the card.
 * Children go at y >= UK_POPUP_BODY_Y inside the card's padding.
 * @opaque paints the backdrop solid so nothing under a heavy sheet redraws. */
static lv_obj_t *_wiz_sheet_open(lv_coord_t w, lv_coord_t h, const char *title,
                                 lv_event_cb_t close_cb, bool opaque,
                                 lv_obj_t **card_out) {
    lv_obj_t *bd = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(bd);
    lv_obj_set_size(bd, lv_pct(100), lv_pct(100));
    lv_obj_center(bd);
    if (opaque) {
        lv_obj_set_style_bg_color(bd, THEME_COLOR_BG, 0);
        lv_obj_set_style_bg_opa(bd, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_color(bd, lv_color_black(), 0);   /* the kit backdrop */
        lv_obj_set_style_bg_opa(bd, 150, 0);
    }
    lv_obj_clear_flag(bd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bd, LV_OBJ_FLAG_CLICKABLE);   /* taps behind the card stop here */

    lv_obj_t *card = lv_obj_create(bd);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, THEME_COLOR_BORDER_MED, 0);   /* line_strong */
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, UK_R_POPUP, 0);
    lv_obj_set_style_pad_all(card, SHEET_PAD, 0);
    lv_obj_set_style_text_color(card, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = uk_label(card, title, UK_FONT_HEAD, UK_TONE_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, w - 2 * SHEET_PAD - 56);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_t *x = uk_btn(card, UK_ICON_CLOSE, NULL, UK_BTN_NEUTRAL, close_cb, NULL);
    lv_obj_set_size(x, 40, 36);
    lv_obj_set_style_pad_hor(x, 0, 0);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 4, -4);
    lv_obj_set_ext_click_area(x, 12);

    if (card_out) *card_out = card;
    return bd;
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
            char abuf[40];
            snprintf(abuf, sizeof(abuf), "Apply %s & Continue", BR_NAMES[bi]);
            uk_btn_set_text(s_btn_apply, abuf);
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
            uk_btn_set_text(s_btn_start, "Re-scan");
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
        uk_btn_set_text(s_btn_start, "Start Scan");
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
                "Already retrying - please wait...");
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
    /* ECU detect next. OBD2 is no longer a step of its own: a Falcon's
     * preset sets it up in the background, and a car with no preset reaches
     * it from the ECU step (ADR-0073). */
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
        s_bar = s_bar_title = s_bar_status = s_bar_close = NULL;
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

    /* Pass 2: for each pair, collect the distinct FRAMES it expects, then
     * check how many are present in the live tracker.
     *
     * A "frame" is (can_id, mux_value) rather than just can_id, because a
     * multiplexed ECU puts its whole stream on one id and distinguishes the
     * payloads by a frame index inside the message. Scoring such a preset by
     * id alone caps it at 1 match no matter how much of its data is on the
     * bus, which is exactly why a Link never got detected. For every
     * non-multiplexed preset a frame IS its id, so the scores are unchanged. */
    uint16_t tracker_n = can_id_tracker_count();
    for (uint8_t j = 0; j < s_ecu_match_count; j++) {
        wiz_ecu_match_t *m = &s_ecu_matches[j];
        wiz_frame_t frames[48];
        uint8_t frames_n = 0;
        for (int i = 0; i < preconfig_items_count; i++) {
            const preconfig_item_t *it = &preconfig_items[i];
            if (!it->ecu || !it->version || !it->can_id) continue;
            if (it->obd2_pid) continue;
            if (strcmp(it->ecu, m->ecu) != 0) continue;
            if (strcmp(it->version, m->version) != 0) continue;
            wiz_frame_t f = {
                .can_id  = (uint32_t)strtol(it->can_id, NULL, 16),
                .mux_len = it->mux_bit_length,
                .mux_val = it->mux_value,
            };
            bool dup = false;
            for (uint8_t k = 0; k < frames_n; k++) {
                if (frames[k].can_id == f.can_id &&
                    frames[k].mux_len == f.mux_len &&
                    frames[k].mux_val == f.mux_val) { dup = true; break; }
            }
            if (!dup && frames_n < 48) frames[frames_n++] = f;
        }
        m->total = frames_n;
        m->matched = 0;
        for (uint8_t i = 0; i < frames_n; i++) {
            for (uint16_t t = 0; t < tracker_n; t++) {
                const can_id_entry_t *e = can_id_tracker_get(t);
                /* Extended flag is part of a frame's identity: an 11-bit
                 * 0x3E8 and a 29-bit 0x3E8 are different frames from
                 * different devices. Every catalogued preset is 11-bit, so
                 * comparing the id alone let a 29-bit bus score matches it
                 * had not earned. */
                if (!e || e->can_id != frames[i].can_id || e->extended) continue;
                /* Multiplexed: the id being present proves nothing — require
                 * that this specific frame index has actually been seen. The
                 * tracker only records indices 0-15, so a higher one can't be
                 * confirmed and doesn't count (rather than being masked down
                 * into a false match). */
                if (frames[i].mux_len) {
                    if (frames[i].mux_val > 15) continue;
                    if (!(e->mux_seen & (uint16_t)(1u << frames[i].mux_val)))
                        continue;
                }
                m->matched++;
                break;
            }
        }
    }

    /* Ranking — tiered descending by (matched, pct, total).
     *
     * Why matched first: when one preset's CAN-ID set is a strict
     * superset of another's (the retired MaxxECU 1.2/1.3 pair was the
     * worked example: same base IDs, extra ones for new features), the
     * smaller preset hits 100% on its own IDs even when the device is the
     * larger one. Pure-percentage ranking then picks the subset — wrong.
     * That pair is now a single preset, but the hazard is generic (any
     * OEM stream that grew between model years), so the rule stays.
     *
     * Counting matched IDs in absolute terms breaks that tie correctly:
     *   - bus with the wide stream (20 of 21): wide matched=20 wins over a
     *     narrow preset's matched=11
     *   - bus with only the narrow stream (11 of 11): both match 11
     *                                   → secondary key (pct) picks the
     *                                     narrow one (100% > 52%)
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
        channel_obd2_prune();   /* nothing wants those PIDs now */
        ESP_LOGI(TAG, "resetup: cleared %u vehicle channel binding(s)",
                 (unsigned)cleared);
    }
}

/* Retire the decodes a previous ECU preset installed.
 *
 * Applying a preset UPSERTS its signals into the registry and the layout's
 * signals[] — it never removes any. Clearing the car's channel bindings
 * (above) left every decode the old ECU wrote in place, and each layout load
 * re-registers them. Seen on the dash, 2026-09-14: after Haltech -> Ford
 * Falcon FG, thirty Haltech decodes were still live and survived a reboot
 * (THROTTLE and MAP on 0x360, IGNITION 0x362, OIL_PRESSURE 0x361, ...). Any
 * channel later bound by name — by the new preset's channel resolution, a
 * source pick, or OBD2 — landed on the old ECU's bit layout, which on a
 * different car's bus is a confident wrong number. Worse for OBD2: a slot
 * with a frame id counts as owned by a CAN broadcast, so its PID was never
 * polled and the Falcon's intake temp read 0x3E0 instead.
 *
 * Only decodes that are still exactly what that preset wrote (name AND frame
 * id) are retired, so a signal the user re-aimed by hand is left alone. The
 * registry slot is made idle in place (subscribers kept) rather than
 * deleted; the layout entry goes, so the next load doesn't bring it back. The
 * new preset's own apply, which runs after this, re-installs anything it
 * shares. Returns how many were retired. */
static int _wiz_retire_ecu_decodes(const char *make, const char *version,
                                   ecu_layout_writer_t *lw) {
    if (!make || !make[0] || !version || !version[0]) return 0;
    int retired = 0;
    for (int i = 0; i < preconfig_items_count; i++) {
        const preconfig_item_t *it = &preconfig_items[i];
        if (!it->ecu || !it->version || !it->label || !it->can_id) continue;
        if (it->obd2_pid) continue;
        if (strcmp(it->ecu, make) != 0 || strcmp(it->version, version) != 0) continue;

        char sname[32];
        _wiz_derive_signal_name(it->label, sname, sizeof(sname));
        if (!sname[0]) continue;
        _wiz_canonicalize_signal_name(sname, sizeof(sname));
        uint32_t cid = (uint32_t)strtol(it->can_id, NULL, 16);

        int16_t idx = signal_find_by_name(sname);
        signal_t *s = (idx >= 0) ? signal_get_by_index((uint16_t)idx) : NULL;
        if (!s || (signal_source_t)s->source != SIGNAL_SOURCE_CAN || s->can_id != cid)
            continue;
        char unit[sizeof(s->unit)];
        memcpy(unit, s->unit, sizeof(unit));   /* not its own buffer as source */
        unit[sizeof(unit) - 1] = '\0';
        signal_register_with_source(sname, 0, 0, 0, 1.0f, 0.0f, false, 1,
                                    unit, SIGNAL_SOURCE_CAN);
        signal_set_mux(idx, 0, 0, 0);
        ecu_layout_writer_remove(lw, sname);
        retired++;
    }
    if (retired)
        ESP_LOGI(TAG, "resetup: retired %d decode(s) left by %s %s",
                 retired, make, version);
    return retired;
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
                                           uint8_t endian,
                                           uint8_t mux_bit_start,
                                           uint8_t mux_bit_length,
                                           uint16_t mux_value) {
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
    channel_manager_set_mux(ch, mux_bit_start, mux_bit_length, mux_value, false);
    channel_manager_set_decode(ch, can_id, bit_start, bit_length, scale, offset,
                               is_signed, endian, NULL, false);
    return true;
}

/* True when `label` is one of the n entries in `labels`. A NULL list means
 * "no filter" — every signal in the set is wanted. */
static bool _label_wanted(const char *const *labels, int n, const char *label) {
    if (!labels) return true;
    if (!label) return false;
    for (int i = 0; i < n; i++)
        if (labels[i] && strcmp(labels[i], label) == 0) return true;
    return false;
}

/* Apply preconfig items belonging to (ecu, version) to BOTH the layout JSON
 * (persistent) AND the live signal registry (immediate), then trigger channel
 * resolution so channels rebind to the new signals. Returns number of items
 * written. See first_run_wizard.h for the labels/replace contract. */
int first_run_wizard_apply_ecu(const char *ecu, const char *version,
                               const char *const *labels, int n_labels,
                               bool replace) {
    if (!ecu || !version) return 0;

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
     * would survive and show as prepopulated on a re-run.
     *
     * Skipped for an additive import (Studio's "+ Add channels > From a
     * pre-defined ECU"): the user is adding to a setup, not declaring a
     * new one, so wiping their other bindings would be a destructive
     * surprise from a menu item that says "Add". */
    if (replace) _wiz_clear_vehicle_channel_bindings();

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

    /* A new car's ECU: the previous one's decodes go first (see
     * _wiz_retire_ecu_decodes). Re-applying the same ECU needs nothing
     * retired — its upserts below overwrite its own entries. */
    if (replace) {
        char pm[32] = {0}, pv[32] = {0};
        if (config_store_load_ecu(pm, sizeof(pm), pv, sizeof(pv)) == ESP_OK &&
            pm[0] && (strcmp(pm, ecu) != 0 || strcmp(pv, version) != 0))
            _wiz_retire_ecu_decodes(pm, pv, lw);
        /* The layout, not just NVS, has to carry the car's ECU: each layout
         * load copies its "ecu" over the NVS one, and this path used to write
         * only NVS — so the wizard's (and Studio import's) choice was
         * forgotten at the next boot (seen on the dash, 2026-09-14). */
        ecu_layout_writer_set_ecu(lw, ecu, version);
        layout_manager_set_ecu_context(ecu, version);
    }

    int applied = 0;
    for (int i = 0; i < preconfig_items_count; i++) {
        const preconfig_item_t *it = &preconfig_items[i];
        if (!it->ecu || !it->version || !it->label || !it->can_id) continue;
        if (it->obd2_pid) continue;
        if (strcmp(it->ecu, ecu) != 0) continue;
        if (strcmp(it->version, version) != 0) continue;
        if (!_label_wanted(labels, n_labels, it->label)) continue;

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
            idx = signal_register(sname, cid,
                it->bit_start, it->bit_length,
                it->scale, it->value_offset,
                it->is_signed, it->endianess, "");
        }
        /* Multiplexed ECUs (Link Generic Dash) put every payload on one id and
         * pick between them with a frame index inside the message. Set the
         * gate on both paths — including the cleared form for normal presets,
         * so a slot previously claimed by a Link doesn't keep dropping frames
         * after the user re-runs the wizard for a different ECU. */
        signal_set_mux(idx, it->mux_bit_start, it->mux_bit_length,
                       it->mux_value);

        /* Guarantee this signal has a channel — canonical where it maps,
         * custom otherwise. This both activates canonical channels the preset
         * provides (the old code only rebound already-active ones) and gives a
         * home to non-canonical signals (the old code dropped them). */
        _wiz_ensure_channel_for_signal(sname, it->label, it->decimals,
                                       cid, it->bit_start, it->bit_length,
                                       it->scale, it->value_offset,
                                       it->is_signed, it->endianess,
                                       it->mux_bit_start, it->mux_bit_length,
                                       it->mux_value);

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

    /* Save the picked ECU so the dashboard remembers it across reboots.
     * Only for a full setup — importing a handful of Haltech signals on
     * top of a MaxxECU car must not relabel the car's ECU. */
    if (replace) config_store_save_ecu(ecu, version);
    s_channels_changed = true;
    ESP_LOGI(TAG, "Applied %d preconfigs from %s/%s into layout '%s' (%s)",
             applied, ecu, version, active_layout,
             replace ? "replace" : "additive");
    /* Restore a signal-derived filter — the apply just registered new
     * signals so the rebuild will use them. */
    can_set_promiscuous_mode(false);
    /* A car whose ECU broadcasts only part of the picture (a Falcon) gets
     * the rest from OBD2 without being asked; any other ECU cancels a setup
     * still owed from a previous car (ADR-0073). After the filter is back,
     * so the scan's replies are filtered the way polling will see them. */
    if (replace) obd2_autosetup_for_ecu(ecu, version);
    return applied;
}

/* The wizard's own semantics: this ECU IS the car's ECU, so every signal
 * comes in and prior vehicle bindings are cleared first. */
static int _apply_ecu_preconfigs(const char *ecu, const char *version) {
    return first_run_wizard_apply_ecu(ecu, version, NULL, 0, true);
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

/* True while the wizard was opened by show_first_run_wizard_rerun(). */
static bool s_rerun = false;

static void _ecu_btn_skip_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    /* User declined to bind anything — restore the signal-derived filter
     * we replaced at probe start. */
    can_set_promiscuous_mode(false);
    /* First run: skipping ECU detection means "I'll bind manually", so the
     * channels step starts from a blank slate, as the apply paths do.
     * A re-run from This dash keeps what is there: its confirm box promises
     * the current settings are kept, and on 2026-09-14 skipping here wiped
     * 51 working channel sources off a bench dash. */
    if (!s_rerun) _wiz_clear_vehicle_channel_bindings();
    /* ...and nothing is owed any more to the car that setup was for. */
    obd2_autosetup_cancel();
    _show_step_channels();
}

/* The name a person knows the preset by — "Ford Falcon FG", not the
 * catalogue's make and version pair "Ford  FG" — from ECU_PRESETS, falling
 * back to make + version for a preconfig set (a DBC import) with no entry. */
static void _ecu_display_name(const char *make, const char *version,
                              char *out, size_t sz) {
    const ecu_preset_t *p = ecu_preset_find(make, version);
    if (p && p->display && p->display[0]) snprintf(out, sz, "%s", p->display);
    else snprintf(out, sz, "%s %s", make ? make : "", version ? version : "");
}

/* The no-match card's two buttons, so the OBD2 check can re-weight them. */
static lv_obj_t *s_ecu_pick_btn = NULL;
static lv_obj_t *s_ecu_obd2_btn = NULL;
static lv_obj_t *s_ecu_obd2_lbl = NULL;

/* The check behind "No ECU detected" came back. If the car answers OBD2,
 * OBD2 is the answer to put first: the button turns primary and says what
 * was found, and "Pick an ECU manually" steps back. If it doesn't, the
 * button just loses its "(checking...)" and the card is as it was. */
static void _ecu_obd2_check_cb(const obd2_autosetup_result_t *r, void *user) {
    (void)user;
    if (!r || r->will_retry) return;     /* a later attempt will report */
    /* Only while those buttons still belong to the card on screen: the card
     * is nulled when the step goes, and a freed pointer can be handed to a
     * new object that lv_obj_is_valid() would happily vouch for. */
    if (!s_ecu_result_card || !lv_obj_is_valid(s_ecu_result_card) ||
        !s_ecu_obd2_btn || lv_obj_get_parent(s_ecu_obd2_btn) != s_ecu_result_card ||
        !s_ecu_obd2_lbl || lv_obj_get_parent(s_ecu_obd2_lbl) != s_ecu_obd2_btn) return;
    if (!r->answered) {
        lv_label_set_text(s_ecu_obd2_lbl, "My car uses OBD2");
        return;
    }
    char b[64];
    snprintf(b, sizeof(b), "Use OBD2  -  your car answers %u readings",
             (unsigned)r->readings);
    lv_label_set_text(s_ecu_obd2_lbl, b);
    uk_btn_set_kind(s_ecu_obd2_btn, UK_BTN_PRIMARY);
    if (s_ecu_pick_btn && lv_obj_get_parent(s_ecu_pick_btn) == s_ecu_result_card) {
        uk_btn_set_kind(s_ecu_pick_btn, UK_BTN_NEUTRAL);
        /* Primary goes on top. */
        lv_obj_align(s_ecu_obd2_btn, LV_ALIGN_TOP_LEFT, 0, 84);
        lv_obj_align(s_ecu_pick_btn, LV_ALIGN_TOP_LEFT, 0, 132);
    }
}

/* "My car uses OBD2" — from the no-match card or the picker sheet. */
static void _ecu_btn_obd2_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (s_ecu_picker_sheet && lv_obj_is_valid(s_ecu_picker_sheet)) {
        lv_obj_del(s_ecu_picker_sheet);
        s_ecu_picker_sheet = NULL;
    }
    _show_step_obd2();
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

    /* Kit sheet over the whole wizard (bar included). */
    lv_obj_t *card = NULL;
    s_ecu_picker_sheet = _wiz_sheet_open(SHEET_W, SHEET_H, "Pick your ECU",
                                         _ecu_picker_close_cb, false, &card);
    const lv_coord_t inner_w = SHEET_W - 2 * SHEET_PAD;
    const lv_coord_t inner_h = SHEET_H - 2 * SHEET_PAD;

    lv_obj_t *sub = lv_label_create(card);
    lv_label_set_text(sub,
        "Tap your ECU and every channel it sends is set up for you.");
    lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_obj_set_style_text_font(sub, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(sub, THEME_COLOR_TEXT_MUTED, 0);

    /* Scrollable list of (ECU, Version) rows. Same look as the channel
     * rows in Step 3 so the wizard reads as one coherent UI. */
    lv_obj_t *list = lv_obj_create(card);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, inner_w, inner_h - UK_POPUP_BODY_Y);
    lv_obj_align(list, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_right(list, 8, 0);   /* rows clear of the scrollbar */
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    uk_style_scrollbar(list);

    /* OBD2 first, set apart from the presets below it: it is the answer
     * for a car with a factory ECU the catalogue has no preset for, and it
     * opens its own screen rather than applying anything (ADR-0073). */
    {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        /* The sheet's full width. Rows were CH_ROW_W — the channels list's
         * 336 px, borrowed — so names wrapped in half a card (seen on glass). */
        lv_obj_set_size(row, lv_pct(100), 52);
        lv_obj_set_style_bg_color(row, THEME_COLOR_CONTROL_BG, 0);          /* raised */
        lv_obj_set_style_bg_color(row, THEME_COLOR_BTN_GRAY, LV_STATE_PRESSED); /* raised_hi */
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        /* The accent edge sets it apart from the presets below. */
        lv_obj_set_style_border_color(row, THEME_COLOR_ACCENT, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_opa(row, LV_OPA_60, 0);
        lv_obj_set_style_radius(row, UK_R_BTN, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, _ecu_btn_obd2_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = uk_label(row, "OBD2", UK_FONT_LABEL, UK_TONE_TEXT);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 6);

        lv_obj_t *hint = lv_label_create(row);
        lv_label_set_text(hint, "Factory ECU with no preset - read through the diagnostic port");
        lv_label_set_long_mode(hint, LV_LABEL_LONG_DOT);
        lv_obj_set_width(hint, SHEET_W - 2 * SHEET_PAD - 80);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 0, -7);
        lv_obj_set_style_text_font(hint, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(hint, THEME_COLOR_TEXT_MUTED, 0);

        lv_obj_t *arrow = uk_icon(row, UK_ICON_RIGHT, UK_ICON_MD, UK_TONE_MUTED);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    for (uint8_t i = 0; i < s_pick_count; i++) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 44);
        lv_obj_set_style_bg_color(row, THEME_COLOR_CONTROL_BG, 0);          /* raised */
        lv_obj_set_style_bg_color(row, THEME_COLOR_BTN_GRAY, LV_STATE_PRESSED); /* raised_hi */
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, UK_R_BTN, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, _ecu_pick_row_clicked_cb,
                            LV_EVENT_CLICKED, (void *)(intptr_t)i);

        char buf[64];
        _ecu_display_name(s_pick_ecus[i], s_pick_versions[i], buf, sizeof(buf));
        lv_obj_t *lbl = lv_label_create(row);
        lv_label_set_text(lbl, buf);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, SHEET_W - 2 * SHEET_PAD - 180);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_text_font(lbl, THEME_FONT_BODY, 0);
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
        lv_obj_align(chip, LV_ALIGN_RIGHT_MID, -30, 0);
        lv_obj_set_style_text_font(chip, THEME_FONT_SMALL, 0);
        lv_obj_set_style_pad_hor(chip, 9, 0);
        lv_obj_set_style_pad_ver(chip, 3, 0);
        lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
        if (strong) {
            /* Strong match — the kit's "on" tint. */
            lv_obj_set_style_bg_color(chip, ui_pal->accent_soft, 0);
            lv_obj_set_style_text_color(chip, ui_pal->accent_ink, 0);
        } else if (has_live) {
            /* Some IDs match but below 30% — show muted to discourage
             * picking it but still give the user the data point. */
            lv_obj_set_style_bg_color(chip, ui_pal->raised_hi, 0);
            lv_obj_set_style_text_color(chip, THEME_COLOR_TEXT_MUTED, 0);
        } else {
            /* No live frames for this ECU — clearly mark as "not seen". */
            lv_obj_set_style_bg_color(chip, THEME_COLOR_STATUS_WARN, 0);
            lv_obj_set_style_bg_opa(chip, LV_OPA_20, 0);
            lv_obj_set_style_text_color(chip, THEME_COLOR_TEXT_MUTED, 0);
        }

        lv_obj_t *arrow = uk_icon(row, UK_ICON_RIGHT, UK_ICON_MD, UK_TONE_MUTED);
        lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, 0, 0);
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

    /* A kit card (card fill, hairline, radius 10, pad 14); the fixed size
     * keeps the buttons' absolute positions inside it. */
    s_ecu_result_card = uk_card(s_step_ecu);
    lv_obj_set_size(s_ecu_result_card, BTN_W, 220);
    lv_obj_align(s_ecu_result_card, LV_ALIGN_TOP_MID, 0, 40);

    if (s_ecu_top_match >= 0) {
        const wiz_ecu_match_t *m = &s_ecu_matches[s_ecu_top_match];
        lv_obj_t *head = uk_section(s_ecu_result_card, "Detected");
        lv_obj_align(head, LV_ALIGN_TOP_LEFT, 0, -4);

        char title_buf[64];
        _ecu_display_name(m->ecu, m->version, title_buf, sizeof(title_buf));
        lv_obj_t *t = uk_label(s_ecu_result_card, title_buf, UK_FONT_TITLE, UK_TONE_TEXT);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, BTN_W - 32);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 16);

        /* Counts, not a percentage. "23% confidence" was what a correct
         * detection of a wide preset printed — a MaxxECU can send 21 frames
         * and a real one may only broadcast five — so the one screen whose
         * job is "yes, that's your ECU" read as a shrug. The ranking never
         * trusted the percentage either (see _compute_ecu_matches). */
        char sub[72];
        snprintf(sub, sizeof(sub), "Heard %u of the %u frames this ECU can send",
                 m->matched, m->total);
        lv_obj_t *s = lv_label_create(s_ecu_result_card);
        lv_label_set_text(s, sub);
        lv_obj_align(s, LV_ALIGN_TOP_LEFT, 0, 52);
        lv_obj_set_style_text_font(s, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(s, THEME_COLOR_TEXT_MUTED, 0);

        /* Use this ECU — the step's one primary action */
        lv_obj_t *use_btn = uk_btn(s_ecu_result_card, UK_ICON_CHECK, "Use this ECU",
                                   UK_BTN_PRIMARY, _ecu_btn_use_cb, NULL);
        lv_obj_set_size(use_btn, BTN_W - 32, BTN_H);
        lv_obj_align(use_btn, LV_ALIGN_TOP_LEFT, 0, 84);

        /* Pick different — secondary */
        lv_obj_t *pick_btn = uk_btn(s_ecu_result_card, UK_ICON_LIST, "Pick a different ECU",
                                    UK_BTN_NEUTRAL, _ecu_btn_pick_cb, NULL);
        lv_obj_set_size(pick_btn, BTN_W - 32, BTN_H);
        lv_obj_align(pick_btn, LV_ALIGN_TOP_LEFT, 0, 132);
    } else {
        /* No match — let the user pick from the catalog or skip. */
        lv_obj_t *head = uk_label(s_ecu_result_card, "No ECU detected",
                                  UK_FONT_HEAD, UK_TONE_TEXT);
        lv_obj_align(head, LV_ALIGN_TOP_LEFT, 0, -2);

        /* Say what we actually heard. A bare "nothing matched" sends the user
         * hunting through Device Settings for the CAN list to find out
         * whether the problem is the wiring, the bitrate or the catalogue —
         * the IDs and the closest preset answer that on the spot, and are
         * what a support conversation asks for first. */
        char heard[192];
        int hn = snprintf(heard, sizeof(heard),
            "Nothing on the bus matched the preset catalog.\n");
        uint16_t seen = can_id_tracker_count();
        if (seen == 0) {
            hn += snprintf(heard + hn, sizeof(heard) - hn,
                           "No CAN frames were heard at all - check wiring,\n"
                           "ignition and bitrate.");
        } else {
            hn += snprintf(heard + hn, sizeof(heard) - hn, "Heard %u ID%s:",
                           (unsigned)seen, seen == 1 ? "" : "s");
            for (uint16_t i = 0; i < seen && i < 6 && hn < (int)sizeof(heard) - 12; i++) {
                const can_id_entry_t *e = can_id_tracker_get(i);
                if (!e) break;
                hn += snprintf(heard + hn, sizeof(heard) - hn, " 0x%lX",
                               (unsigned long)e->can_id);
            }
            if (seen > 6) hn += snprintf(heard + hn, sizeof(heard) - hn, " ...");
            /* Best partial match, even though it fell under the floor — "Link
             * ECU 1/14" tells the user (and us) far more than silence. */
            uint8_t best = 0; const wiz_ecu_match_t *bm = NULL;
            for (uint8_t j = 0; j < s_ecu_match_count; j++) {
                if (s_ecu_matches[j].matched > best) {
                    best = s_ecu_matches[j].matched; bm = &s_ecu_matches[j];
                }
            }
            if (bm) {
                snprintf(heard + hn, sizeof(heard) - hn,
                         "\nClosest: %s %s (%u/%u frames)",
                         bm->ecu, bm->version, bm->matched, bm->total);
            }
        }

        lv_obj_t *sub = lv_label_create(s_ecu_result_card);
        lv_label_set_text(sub, heard);
        lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, 28);
        lv_obj_set_style_text_font(sub, THEME_FONT_TINY, 0);
        lv_obj_set_style_text_color(sub, THEME_COLOR_TEXT_MUTED, 0);
        lv_label_set_long_mode(sub, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(sub, BTN_W - 32);

        lv_obj_t *pick_btn = uk_btn(s_ecu_result_card, UK_ICON_LIST, "Pick an ECU manually",
                                    UK_BTN_PRIMARY, _ecu_btn_pick_cb, NULL);
        s_ecu_pick_btn = pick_btn;
        lv_obj_set_size(pick_btn, BTN_W - 32, BTN_H);
        lv_obj_align(pick_btn, LV_ALIGN_TOP_LEFT, 0, 84);

        /* Nothing matched is what a factory ECU without a preset looks like
         * — the car OBD2 is for. Say it here, where that car ends up. */
        lv_obj_t *obd_btn = uk_btn(s_ecu_result_card, UK_ICON_OBD,
                                   "My car uses OBD2   (checking...)",
                                   UK_BTN_NEUTRAL, _ecu_btn_obd2_cb, NULL);
        lv_obj_set_size(obd_btn, BTN_W - 32, BTN_H);
        lv_obj_align(obd_btn, LV_ALIGN_TOP_LEFT, 0, 132);
        s_ecu_obd2_btn = obd_btn;
        s_ecu_obd2_lbl = uk_btn_label(obd_btn);
        /* Don't make them guess: ask the car whether it answers OBD2 while
         * the card is up, and say so on the button (ADR-0074). */
        obd2_autosetup_check(_ecu_obd2_check_cb, NULL);
    }

    /* Skip — always available, regardless of detection result. */
    lv_obj_t *skip_btn = uk_btn(s_step_ecu, UK_ICON_NONE, "Skip ECU setup",
                                UK_BTN_GHOST, _ecu_btn_skip_cb, NULL);
    lv_obj_set_size(skip_btn, BTN_W, BTN_H);
    lv_obj_align(skip_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
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

/* ── OBD2 — a branch of the ECU step, not a step of its own (ADR-0073) ─────
 *
 * This was "Step 2 of 5" and every customer saw it, most of them on a
 * standalone ECU that broadcasts everything and has nothing to gain. OBD2 is
 * the answer for two kinds of car, and neither needs the question put to
 * everyone:
 *
 *  - A car whose preset broadcasts only part of the picture (a Falcon). The
 *    preset's apply sets OBD2 up by itself in the background
 *    (obd2_autosetup_for_ecu) — no screen at all.
 *  - A car with a factory ECU the catalogue has no preset for. Its owner
 *    says so at the ECU step ("My car uses OBD2"), and lands here.
 *
 * Here, the scan starts on arrival — they chose OBD2, so asking them to press
 * Scan as well would be a second question with one answer — and whatever the
 * car reports becomes channels without a further tap. The work is the shared
 * autosetup module, so a car that doesn't answer yet is also still set up
 * later, the first time it does. */

static void _obd2_step_result_cb(const obd2_autosetup_result_t *r, void *user);

static void _obd2_step_advance(void) {
    _show_step_channels();   /* takes this screen down on the way in */
}

static void _btn_obd2_continue_cb(lv_event_t *e) {
    (void)e;
    /* Leaving mid-scan is fine: the scan finishes in the background, binds
     * what the car answers, and the channels step refreshes when it lands. */
    _obd2_step_advance();
}

static void _btn_obd2_back_cb(lv_event_t *e) {
    (void)e;
    /* Changed their mind — nothing is owed to a car they are about to give a
     * preset instead. */
    obd2_autosetup_forget(_obd2_step_result_cb);
    obd2_autosetup_cancel();
    if (s_step_obd2 && lv_obj_is_valid(s_step_obd2)) lv_obj_del(s_step_obd2);
    s_step_obd2 = NULL;
    s_obd2_step_status = s_obd2_step_spinner = s_obd2_step_rescan_btn = NULL;
    _show_step_ecu_detect();
}

static void _obd2_step_set_busy(bool busy) {
    if (s_obd2_step_spinner && lv_obj_is_valid(s_obd2_step_spinner)) {
        if (busy) lv_obj_clear_flag(s_obd2_step_spinner, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(s_obd2_step_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_obd2_step_rescan_btn && lv_obj_is_valid(s_obd2_step_rescan_btn)) {
        if (busy) lv_obj_add_flag(s_obd2_step_rescan_btn, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_clear_flag(s_obd2_step_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (busy && s_obd2_step_status && lv_obj_is_valid(s_obd2_step_status)) {
        lv_label_set_text(s_obd2_step_status,
            "Asking the car which readings it has...");
        lv_obj_set_style_text_color(s_obd2_step_status,
                                    THEME_COLOR_TEXT_PRIMARY, 0);
    }
}

/* Every attempt's outcome, for as long as this screen is up. */
static void _obd2_step_result_cb(const obd2_autosetup_result_t *r, void *user) {
    (void)user;
    if (r && r->bound) s_channels_changed = true;
    if (!s_step_obd2 || !lv_obj_is_valid(s_step_obd2)) return;
    _obd2_step_set_busy(false);
    if (!r || !s_obd2_step_status || !lv_obj_is_valid(s_obd2_step_status)) return;

    char msg[240];
    lv_color_t col = THEME_COLOR_TEXT_MUTED;
    if (!r->answered) {
        snprintf(msg, sizeof(msg),
            "The car didn't answer. Turn the ignition on and\n"
            "scan again - or carry on: the dash keeps asking,\n"
            "and sets OBD2 up the first time the car answers.");
    } else if (r->bound && r->err == ESP_ERR_NO_MEM) {
        snprintf(msg, sizeof(msg),
            "%u channels set up from OBD2. The dash can poll\n"
            "48 readings at most, so some were left out.",
            (unsigned)r->bound);
        col = THEME_COLOR_STATUS_CONNECTED;
    } else if (r->bound && r->err == ESP_FAIL) {
        snprintf(msg, sizeof(msg),
            "%u channels set up from OBD2 - working now, but\n"
            "they could not be saved, so check them after a restart.",
            (unsigned)r->bound);
    } else if (r->bound) {
        snprintf(msg, sizeof(msg),
            "Done - your car reports %u readings, and %u of\n"
            "them are now channels. You'll see them next.",
            (unsigned)r->readings, (unsigned)r->bound);
        col = THEME_COLOR_STATUS_CONNECTED;
    } else if (r->offered) {
        snprintf(msg, sizeof(msg),
            "Your car reports %u readings, and everything it\n"
            "offers is already set up.", (unsigned)r->readings);
        col = THEME_COLOR_STATUS_CONNECTED;
    } else {
        snprintf(msg, sizeof(msg),
            "Your car answered, but nothing it reports is a\n"
            "reading the dash has a channel for.");
    }
    lv_label_set_text(s_obd2_step_status, msg);
    lv_obj_set_style_text_color(s_obd2_step_status, col, 0);
}

static void _obd2_step_scan(void) {
    _obd2_step_set_busy(true);
    obd2_autosetup_start(_obd2_step_result_cb, NULL);
    /* A scan someone else started holds the bus; ours queues behind it and
     * the callback still arrives. Nothing more to do here. */
}

static void _btn_obd2_rescan_cb(lv_event_t *e) {
    (void)e;
    if (obd2_autosetup_running()) return;
    _obd2_step_scan();
}

static void _show_step_obd2(void) {
    can_bus_test_set_ui_callback(NULL);

    /* Arriving from the ECU step (or its picker sheet): take that down the
     * same way the channels step does. */
    if (s_ecu_probe_timer) {
        lv_timer_del(s_ecu_probe_timer);
        s_ecu_probe_timer = NULL;
    }
    if (s_step_ecu && lv_obj_is_valid(s_step_ecu)) lv_obj_del(s_step_ecu);
    s_step_ecu = s_ecu_progress = s_ecu_status = s_ecu_result_card = NULL;
    s_ecu_picker_sheet = NULL;   /* a child of the overlay, deleted by the caller */

    /* "This car has no ECU preset": start the channels from a blank slate,
     * as skipping the ECU does, and stop remembering a previous car's ECU. */
    can_set_promiscuous_mode(false);
    _wiz_clear_vehicle_channel_bindings();
    {
        /* ...and the previous ECU's decodes with them, or OBD2 binds onto
         * them by name (see _wiz_retire_ecu_decodes). */
        char pm[32] = {0}, pv[32] = {0};
        if (config_store_load_ecu(pm, sizeof(pm), pv, sizeof(pv)) == ESP_OK && pm[0]) {
            char active[LAYOUT_MAX_NAME] = "default";
            if (layout_manager_get_active(active, sizeof(active)) != ESP_OK || !active[0])
                strncpy(active, "default", sizeof(active) - 1);
            ecu_layout_writer_t *lw = ecu_layout_writer_open(active);
            _wiz_retire_ecu_decodes(pm, pv, lw);
            /* No preset now — and the layout must say so too, or its old
             * "ecu" is copied back over NVS at the next load. */
            ecu_layout_writer_set_ecu(lw, "", "");
            if (lw) ecu_layout_writer_commit(lw);
        }
        layout_manager_set_ecu_context("", "");
    }
    config_store_save_ecu("", "");
    config_store_save_ecu_base_id(0);

    /* The body under the kit bar is a fixed size; nothing to resize. */
    _wiz_bar_set("OBD2", "Step 2 of 4", false);

    s_step_obd2 = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_step_obd2);
    lv_obj_set_size(s_step_obd2, lv_pct(100), lv_pct(100));
    lv_obj_center(s_step_obd2);
    lv_obj_clear_flag(s_step_obd2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *body = lv_label_create(s_step_obd2);
    lv_label_set_text(body,
        "The dash asks your car, through its diagnostic port, which\n"
        "readings it has and turns each one into a channel.\n"
        "Keep the ignition on.");
    lv_obj_set_width(body, BTN_W);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_text_font(body, THEME_FONT_BODY, 0);
    lv_obj_set_style_text_color(body, THEME_COLOR_TEXT_MUTED, 0);

    s_obd2_step_spinner = lv_spinner_create(s_step_obd2, 1000, 60);
    lv_obj_set_size(s_obd2_step_spinner, 28, 28);
    lv_obj_align(s_obd2_step_spinner, LV_ALIGN_TOP_MID, 0, 72);
    _wiz_style_spinner(s_obd2_step_spinner);

    s_obd2_step_status = lv_label_create(s_step_obd2);
    lv_label_set_text(s_obd2_step_status, "");
    lv_obj_set_width(s_obd2_step_status, BTN_W);
    lv_obj_set_style_text_align(s_obd2_step_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_obd2_step_status, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_style_text_font(s_obd2_step_status, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_obd2_step_status, THEME_COLOR_TEXT_MUTED, 0);

    s_obd2_step_rescan_btn = _make_btn(s_step_obd2, UK_ICON_RESET, "Scan again",
                                       UK_BTN_NEUTRAL, 206, _btn_obd2_rescan_cb);

    _make_btn(s_step_obd2, UK_ICON_NONE, "Continue",
              UK_BTN_PRIMARY, 258, _btn_obd2_continue_cb);

    _make_btn(s_step_obd2, UK_ICON_LEFT, "Back - pick an ECU instead",
              UK_BTN_GHOST, 310, _btn_obd2_back_cb);

    _obd2_step_scan();
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

    /* The body under the kit bar is a fixed size; nothing to resize. */
    _wiz_bar_set("Your ECU", "Step 2 of 4", false);

    s_step_ecu = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_step_ecu);
    lv_obj_set_size(s_step_ecu, lv_pct(100), lv_pct(100));
    lv_obj_center(s_step_ecu);
    lv_obj_clear_flag(s_step_ecu, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sub = lv_label_create(s_step_ecu);
    lv_label_set_text(sub,
        "Matching what is on the CAN bus against the ECU presets");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_style_text_font(sub, THEME_FONT_BODY, 0);
    lv_obj_set_style_text_color(sub, THEME_COLOR_TEXT_MUTED, 0);

    /* Probe progress + status line, hidden once the probe completes. */
    s_ecu_status = lv_label_create(s_step_ecu);
    lv_label_set_text(s_ecu_status, "Listening to the bus...");
    lv_obj_align(s_ecu_status, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_set_style_text_font(s_ecu_status, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_ecu_status, THEME_COLOR_TEXT_PRIMARY, 0);

    s_ecu_progress = lv_bar_create(s_step_ecu);
    lv_obj_set_size(s_ecu_progress, BTN_W, 6);
    lv_obj_align(s_ecu_progress, LV_ALIGN_TOP_MID, 0, 86);
    lv_bar_set_range(s_ecu_progress, 0, 100);
    lv_bar_set_value(s_ecu_progress, 0, LV_ANIM_OFF);
    _wiz_style_bar(s_ecu_progress);

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
static void _calc_btn_cb(lv_event_t *e);

/* ── List rows ───────────────────────────────────────────────────────── */

/* Compressed row — no subtitle, no per-row source label. Just a label
 * left and a value right, plus a thin left strip indicating bound/unbound.
 * Halves the per-row paint count vs. the previous design, which is the
 * difference between scrollable and not on the dash.
 *
 * Kit look: a neutral raised row (radius 8); selected is the accent tint
 * with an accent edge; a row with no source is drawn muted. */
static void _style_channel_row(lv_obj_t *row, bool inactive, bool selected) {
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, CH_ROW_W, CH_ROW_H);
    lv_obj_set_style_bg_color(row,
        selected ? THEME_COLOR_ACCENT_DIM : THEME_COLOR_CONTROL_BG, 0);   /* accent_soft : raised */
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, THEME_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(row, selected ? 1 : 0, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UK_R_BTN, 0);
    lv_obj_set_style_pad_left(row, 14, 0);
    lv_obj_set_style_pad_right(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 4, 0);
    lv_obj_set_style_opa(row, (inactive && !selected) ? LV_OPA_70 : LV_OPA_COVER, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
}

/* Name-label colour for a row: accent ink on the selection tint, muted for
 * a catalogue channel not set up on this dash, else primary. */
static lv_color_t _row_name_color(bool ghost, bool selected) {
    if (selected) return ui_pal->accent_ink;
    return ghost ? THEME_COLOR_TEXT_MUTED : THEME_COLOR_TEXT_PRIMARY;
}

/* Strip colour: a channel with a source reads as live (the status "ok"
 * colour), one without as a plain strong hairline. */
static lv_color_t _row_strip_color(bool inactive) {
    return inactive ? THEME_COLOR_BORDER_MED : THEME_COLOR_STATUS_CONNECTED;
}

/* In apply-to-widget mode, outline the row of the channel the target widget
 * is CURRENTLY bound to (s_apply_widget_chid) in the warn colour so it's
 * obvious what is applied — distinct from the accent tap-selection.
 * Re-applied on EVERY (re)style (append + rebuild) so it survives selection
 * changes / row rebuilds that reset the border. */
static void _apply_bound_widget_highlight(lv_obj_t *row, const char *id) {
    if (!s_apply_to_widget_mode || !id || !s_apply_widget_chid[0]) return;
    if (strcmp(id, s_apply_widget_chid) != 0) return;
    lv_obj_set_style_border_color(row, THEME_COLOR_ACCENT_ORANGE, 0);
    lv_obj_set_style_border_width(row, 3, 0);
    lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);
}

/* 3 px left strip — bound = live colour, unbound = hairline grey. */
static void _add_row_accent_strip(lv_obj_t *row, bool inactive) {
    lv_obj_t *strip = lv_obj_create(row);
    lv_obj_remove_style_all(strip);
    lv_obj_set_size(strip, 3, CH_ROW_H - 12);
    /* align is relative to the padded box; -11 = -(pad_left 14) + 3. */
    lv_obj_align(strip, LV_ALIGN_LEFT_MID, -11, 0);
    lv_obj_set_style_bg_color(strip, _row_strip_color(inactive), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(strip, 2, 0);
    lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE);
}

static void _row_clicked_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *id = (const char *)lv_event_get_user_data(e);
    if (!id || !id[0]) return;

    /* Tapping NEVER creates. This used to activate a ghost row on the
     * spot — the exact trap ADR-0032 closed in the web editor, and worse
     * here because a scroll flick on glass lands as taps. The tap now
     * only selects; the detail pane renders a preview for ghost rows
     * with an explicit "Add this channel" button (ADR-0034 brings the
     * two editors to the same rule). */
    _select_channel_by_id(id);
    _channels_refresh_cb(NULL);
}

/* "Add this channel" in the ghost preview — the deliberate act. Runs the
 * activation + canonical auto-bind that the row tap used to fire. */
static void _ghost_add_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    const char *id = s_selected_ch_id;
    if (!id[0] || channel_manager_get(id)) return;
    if (!canonical_channel_exists(id)) return;
    channel_t *c = channel_manager_activate(id);
    if (!c) return;   /* registry full — hero stats stay honest */
    channel_manager_resolve_signals();
    _wiz_canonical_bind_channel(c);
    s_channels_changed = true;
    for (uint16_t i = 0; i < s_channels_count; i++) {
        if (s_channels_ids[i] && strcmp(s_channels_ids[i], id) == 0) {
            _rebuild_channel_row(i);
            break;
        }
    }
    _refresh_hero_stats();
    _render_detail_pane();     /* ghost preview → full editor */
    _channels_refresh_cb(NULL);
}

/* "Remove from this dash" — two taps, no timer. The first arms; the
 * second (same pane render, same channel) executes. Any pane rebuild
 * disarms, so selecting elsewhere or editing a field resets it. */
static bool s_remove_armed = false;

static void _remove_channel_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_selected_ch_id[0]) return;
    if (!s_remove_armed) {
        s_remove_armed = true;
        lv_obj_t *btn = lv_event_get_target(e);
        uk_btn_set_text(btn, "Tap again to remove");
        /* Armed: the danger button fills solid (what it shows when pressed).
         * The kit has no "armed" kind; the pane re-render that disarms
         * builds a fresh button, so these local styles never linger. */
        lv_obj_set_style_bg_color(btn, THEME_COLOR_BTN_DANGER, 0);
        lv_obj_set_style_text_color(btn, THEME_COLOR_TEXT_ON_ACCENT, 0);
        return;
    }
    char removed_id[32];
    strncpy(removed_id, s_selected_ch_id, sizeof(removed_id) - 1);
    removed_id[sizeof(removed_id) - 1] = '\0';
    if (!channel_manager_remove(removed_id)) return;
    channel_obd2_prune();   /* give back its OBD2 PID, if it had one */
    s_channels_changed = true;
    s_selected_ch_id[0] = '\0';
    ESP_LOGI(TAG, "Removed channel '%s' (returns to catalogue if canonical)",
             removed_id);
    /* Full list rebuild: a canonical id keeps its (now ghost) row; a
     * custom id's row disappears. Both fall out of re-population. */
    _populate_channels_list();
    _refresh_hero_stats();
    _render_detail_pane();
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
        if (!lbl || !s_channels_ids[i]) continue;
        const channel_t *c = channel_manager_get(s_channels_ids[i]);
        /* Ghosts are skipped BEFORE lv_obj_is_valid(): in LVGL v8 that call
         * walks every object on every screen, and it ran for all ~100
         * catalogue rows twice a second only to reach the `continue` below. */
        if (c && !lv_obj_is_valid(lbl)) continue;
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
    /* Recolor the left strip (first child with width 3) and the name label
     * (the first label — the value label after it is the refresh timer's). */
    uint32_t n = lv_obj_get_child_cnt(row);
    bool strip_done = false, name_done = false;
    for (uint32_t i = 0; i < n && !(strip_done && name_done); i++) {
        lv_obj_t *ch = lv_obj_get_child(row, i);
        if (!ch) continue;
        if (!strip_done && lv_obj_get_width(ch) == 3) {
            lv_obj_set_style_bg_color(ch, _row_strip_color(inactive), 0);
            strip_done = true;
        } else if (!name_done && lv_obj_check_type(ch, &lv_label_class)) {
            lv_obj_set_style_text_color(ch, _row_name_color(c == NULL, selected), 0);
            name_done = true;
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

/* Helper: section heading inside the detail pane — the kit's muted caps
 * section label. y_offset relative to the pane origin. Returns the label so
 * the caller can stash it if needed (none currently do). */
#define CH_SECTION_H 26   /* a uk_section label and the gap under it */
static lv_obj_t *_detail_section_label(lv_obj_t *parent, const char *text,
                                       lv_coord_t y) {
    lv_obj_t *l = uk_section(parent, text);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, y);
    return l;
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
    char     ch_id[32];
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
    /* ADR-0005: the CHANNEL owns the decode (channels.json is authoritative);
     * the layout no longer carries it. Write the edit onto the bound channel via
     * the same path the web /api/channels/update {decode} handler uses — this
     * re-registers the live signal AND persists channels.json. Writing only the
     * live signal + layout (the old behaviour) left c->is_signed/etc. stale, so
     * register_decoded_signals() clobbered the edit back on the next resolve or
     * reboot — e.g. a Signed toggle silently reverted to Unsigned. */
    channel_t *c = channel_manager_get(w->ch_id);
    if (c) {
        channel_manager_set_decode(c, w->can_id, w->bit_start, w->bit_length,
            w->scale, w->offset, w->is_signed, w->endian,
            w->has_unit ? w->unit : NULL, /* persist_now */ true);
    }
    free(w);
}

/* Persist an (already mutated) signal's decode onto its bound channel so the
 * edit survives reboot. The live effect of the in-place registry mutation is
 * immediate; this schedules the authoritative channel write + channels.json
 * flush (deferred — see _wiz_persist_decode_async). Decimals are omitted —
 * those are set separately via channel_manager_set_decimals. */
static void _wiz_persist_signal_decode(const signal_t *s) {
    if (!s || !s->name[0] || !s_selected_ch_id[0]) return;
    wiz_decode_write_t *w = calloc(1, sizeof(*w));
    if (!w) return;
    strncpy(w->ch_id, s_selected_ch_id, sizeof(w->ch_id) - 1);
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

typedef enum { DD_DECIMALS = 1, DD_BIT_START, DD_BIT_LEN, DD_SIGNED, DD_ENDIAN, DD_UNITS, DD_SOURCE_UNITS } dd_field_t;

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

/* Source-unit (units_native = "what the ECU sends") dropdown: the canonical
 * BASE native first, then the full convertible family, so e.g. °C/°F/K all
 * appear regardless of the channel's current native. @sel_out = index of the
 * channel's current native. */
static const char *_source_dd_base(const channel_t *c) {
    const canonical_channel_def_t *def = canonical_channel_find(c->id);
    return (def && def->units_native[0]) ? def->units_native : c->units_native;
}
static uint16_t _source_dd_options(const channel_t *c, char *buf, size_t cap,
                                   uint16_t *sel_out) {
    const char *base = _source_dd_base(c);
    const char *targets[8];
    size_t n = unit_convert_targets(base, targets, 8);
    size_t pos = (size_t)snprintf(buf, cap, "%s", base);
    uint16_t sel = 0;
    for (size_t i = 0; i < n && pos < cap; i++) {
        if (strcmp(c->units_native, targets[i]) == 0) sel = (uint16_t)(i + 1);
        pos += (size_t)snprintf(buf + pos, cap - pos, "\n%s", targets[i]);
    }
    if (sel_out) *sel_out = sel;
    return (uint16_t)(n + 1);
}
static const char *_source_dd_index_to_unit(const channel_t *c, uint16_t idx) {
    const char *base = _source_dd_base(c);
    if (idx == 0) return base;
    const char *targets[8];
    size_t n = unit_convert_targets(base, targets, 8);
    return ((size_t)(idx - 1) < n) ? targets[idx - 1] : base;
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
    if (f == DD_SOURCE_UNITS) {
        /* Change the native (received) unit — re-expresses range + thresholds.
         * Safe in VALUE_CHANGED (no synchronous layout write; mark_dirty defers
         * the channels.json save). Defer the pane rebuild for the same reason
         * as DD_UNITS. */
        channel_manager_set_units_native(c, _source_dd_index_to_unit(c, idx));
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

/* The key on the left of an input row: body-sized, muted — the kit row's
 * key. The control sits right-aligned on the same line. */
static void _make_row_key(lv_obj_t *parent, lv_coord_t y, const char *label_text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, label_text);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, y + 7);
    lv_obj_set_style_text_font(l, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(l, THEME_COLOR_TEXT_MUTED, 0);
}

/* Label + a kit text field; tap opens the keypad/keyboard. No glyph — the
 * filled input field reads as editable, and the absence of a chevron
 * distinguishes it from the dropdown rows. */
static void _make_textbox_row(lv_obj_t *parent, lv_coord_t y, const char *label_text,
                              const char *value_text, kp_field_t field) {
    _make_row_key(parent, y, label_text);

    lv_obj_t *box = lv_btn_create(parent);
    _wiz_style_textbox(box);
    lv_obj_set_size(box, CH_INPUT_W, CH_INPUT_H);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, CH_DETAIL_W - 20 - CH_INPUT_W, y);
    lv_obj_set_user_data(box, (void *)(intptr_t)field);
    lv_obj_add_event_cb(box, _kp_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(box);
    lv_label_set_text(bl, value_text);
    lv_label_set_long_mode(bl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(bl, CH_INPUT_W - 22);
    lv_obj_set_style_text_font(bl, THEME_FONT_SMALL, 0);
    lv_obj_align(bl, LV_ALIGN_LEFT_MID, 11, 0);
}

/* Label + an lv_dropdown (newline-separated options) in the kit dropdown
 * look; its built-in chevron signals "pick from a list". */
static void _make_dropdown_row(lv_obj_t *parent, lv_coord_t y, const char *label_text,
                               const char *options, uint16_t sel, dd_field_t field) {
    _make_row_key(parent, y, label_text);

    lv_obj_t *dd = lv_dropdown_create(parent);
    lv_dropdown_set_options(dd, options);
    lv_dropdown_set_selected(dd, sel);
    _wiz_style_dropdown(dd);
    lv_obj_set_size(dd, CH_INPUT_W, CH_INPUT_H);
    lv_obj_align(dd, LV_ALIGN_TOP_LEFT, CH_DETAIL_W - 20 - CH_INPUT_W, y);
    lv_obj_set_user_data(dd, (void *)(intptr_t)field);
    lv_obj_add_event_cb(dd, _dropdown_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
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
    s_remove_armed = false;   /* any re-render disarms the remove confirm */
    channel_t *c = channel_manager_get(s_selected_ch_id);
    if (!c) {
        /* Ghost — a catalogue channel this dash hasn't set up. Preview what
         * it is and make adding an explicit act (ADR-0032/0034 parity with
         * the web editor). The pane used to just bail here, which is why
         * the row tap HAD to activate; now the preview carries the button. */
        const canonical_channel_def_t *def =
            canonical_channel_find(s_selected_ch_id);
        if (!def) return;

        lv_obj_t *lbl = uk_label(s_detail_pane, def->label, UK_FONT_HEAD, UK_TONE_TEXT);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, CH_DETAIL_W - 20);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

        lv_obj_t *sub = lv_label_create(s_detail_pane);
        lv_label_set_text(sub, "Not set up on this dash");
        lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 0, 28);
        lv_obj_set_style_text_font(sub, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(sub, THEME_COLOR_TEXT_MUTED, 0);

        /* Key / value rows in the kit's row look (hairline under each). */
        lv_coord_t gy = 50;
        const lv_coord_t row_h = 36;
        char line[48];
        lv_obj_t *v = uk_row(s_detail_pane, "Units",
                             def->units_display_def[0] ? def->units_display_def : "-");
        lv_obj_align(lv_obj_get_parent(v), LV_ALIGN_TOP_LEFT, 0, gy);
        gy += row_h;
        snprintf(line, sizeof(line), "%g to %g",
                 (double)def->min_default, (double)def->max_default);
        v = uk_row(s_detail_pane, "Typical range", line);
        lv_obj_align(lv_obj_get_parent(v), LV_ALIGN_TOP_LEFT, 0, gy);
        gy += row_h;
        v = uk_row(s_detail_pane, "Group", channel_group_name(def->group));
        lv_obj_align(lv_obj_get_parent(v), LV_ALIGN_TOP_LEFT, 0, gy);
        gy += row_h + 10;

        if (def->notes) {
            lv_obj_t *n = lv_label_create(s_detail_pane);
            lv_label_set_text(n, def->notes);
            lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(n, CH_DETAIL_W - 20);
            lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, gy);
            lv_obj_set_style_text_font(n, THEME_FONT_SMALL, 0);
            lv_obj_set_style_text_color(n, THEME_COLOR_TEXT_MUTED, 0);
            lv_obj_update_layout(n);
            gy += lv_obj_get_height(n) + 10;
        }

        /* The pane's one forward action. */
        lv_obj_t *add = uk_btn(s_detail_pane, UK_ICON_PLUS, "Add this channel",
                               UK_BTN_PRIMARY, _ghost_add_cb, NULL);
        lv_obj_set_size(add, CH_DETAIL_W - 20, BTN_H);
        lv_obj_align(add, LV_ALIGN_TOP_LEFT, 0, gy);
        return;
    }

    /* ── Hero (label + giant live value) ─────────────────────────────── */
    lv_obj_t *lbl = uk_label(s_detail_pane, c->label, UK_FONT_HEAD, UK_TONE_TEXT);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, CH_DETAIL_W - 20);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    /* The big value in the kit's condensed face, which (unlike Montserrat)
     * has a degree sign for the unit. Its colour is data — the refresh
     * timer paints it (disabled / primary / warn). */
    s_detail_value_lbl = lv_label_create(s_detail_pane);
    lv_label_set_text(s_detail_value_lbl, "...");
    lv_obj_set_width(s_detail_value_lbl, CH_DETAIL_W - 20);
    lv_obj_set_style_text_align(s_detail_value_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_detail_value_lbl, LV_ALIGN_TOP_LEFT, 0, 26);
    lv_obj_set_style_text_font(s_detail_value_lbl, uk_font(UK_FONT_TITLE), 0);
    lv_obj_set_style_text_color(s_detail_value_lbl,
        THEME_COLOR_TEXT_DISABLED, 0);

    /* ── Source section ─────────────────────────────────────────────── */
    lv_coord_t y = 64;
    _detail_section_label(s_detail_pane, "Source", y);
    y += CH_SECTION_H;

    s_detail_source_lbl = lv_label_create(s_detail_pane);
    if (c->signal_index >= 0 && c->signal_name[0]) {
        /* Say WHERE it comes from, not which registry slot holds it — the
         * same rule the web table follows (ADR-0035). "via COOLANT_TEMP"
         * under a channel already labelled "Coolant Temp" was plumbing
         * shown to the user. Provenance mirrors the derivation in
         * channel_to_full_json(): registry source when registered, else a
         * decode/OBD2-name fallback. CAN adds its frame id, the one fact
         * that separates two same-named decodes. */
        signal_t *ssig = signal_get_by_index((uint16_t)c->signal_index);
        char buf[64];
        if (ssig && (signal_source_t)ssig->source == SIGNAL_SOURCE_OBD2) {
            snprintf(buf, sizeof(buf), "OBD2");
        } else if (ssig && (signal_source_t)ssig->source == SIGNAL_SOURCE_INTERNAL) {
            snprintf(buf, sizeof(buf), "RDM-7 internal");
        } else if (ssig) {
            snprintf(buf, sizeof(buf), "CAN  0x%lX",
                     (unsigned long)ssig->can_id);
        } else if (c->can_id != 0) {
            snprintf(buf, sizeof(buf), "CAN  0x%lX", (unsigned long)c->can_id);
        } else {
            /* Bound by name but nothing registered this boot — its frames
             * haven't arrived. Name it rather than print a bare "—". */
            snprintf(buf, sizeof(buf), "%s (waiting)", c->signal_name);
        }
        lv_label_set_text(s_detail_source_lbl, buf);
        lv_obj_set_style_text_color(s_detail_source_lbl,
            THEME_COLOR_TEXT_PRIMARY, 0);
    } else {
        lv_label_set_text(s_detail_source_lbl, "No source");
        lv_obj_set_style_text_color(s_detail_source_lbl,
            THEME_COLOR_STATUS_WARN, 0);
    }
    lv_label_set_long_mode(s_detail_source_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_detail_source_lbl, CH_DETAIL_W - 24 - 96 - 10);   /* clear of Change */
    lv_obj_align(s_detail_source_lbl, LV_ALIGN_TOP_LEFT, 0, y + 9);
    lv_obj_set_style_text_font(s_detail_source_lbl, THEME_FONT_BODY, 0);

    lv_obj_t *change = uk_btn(s_detail_pane, UK_ICON_NONE, "Change",
                              UK_BTN_NEUTRAL, _change_source_cb, NULL);
    lv_obj_set_size(change, 96, 34);
    lv_obj_align(change, LV_ALIGN_TOP_RIGHT, 0, y);
    y += 42;

    /* Calculate button — derive this channel from two others (math), the
     * on-device twin of the web editor's Calculate form. When already a
     * math channel, the button shows the current formula (and reads "on"). */
    {
        lv_obj_t *calc = uk_btn(s_detail_pane, UK_ICON_NONE,
                                "Calculate from other channels",
                                c->math_enabled ? UK_BTN_ON : UK_BTN_NEUTRAL,
                                _calc_btn_cb, NULL);
        lv_obj_set_size(calc, CH_DETAIL_W - 20, 34);
        lv_obj_align(calc, LV_ALIGN_TOP_LEFT, 0, y);
        lv_obj_t *cll = uk_btn_label(calc);
        if (c->math_enabled) {
            static const char *ops = "+-*/";
            char ab[80];
            const char *a = c->math_a_is_const ? "#" : c->math_a;
            const char *b = c->math_b_is_const ? "#" : c->math_b;
            if (c->math_c_enabled) {
                /* Parenthesised because it evaluates left to right and a
                 * reader who assumes precedence would read it wrong. */
                const char *cc = c->math_c_is_const ? "#" : c->math_c;
                snprintf(ab, sizeof(ab), "Calc: (%s %c %s) %c %s", a,
                         ops[c->math_op & 3], b, ops[c->math_op2 & 3], cc);
            } else {
                snprintf(ab, sizeof(ab), "Calc: %s %c %s", a,
                         ops[c->math_op & 3], b);
            }
            lv_label_set_text(cll, ab);
        } else {
            lv_label_set_text(cll, "Calculate from other channels");
        }
        lv_label_set_long_mode(cll, LV_LABEL_LONG_DOT);
        /* Inside the kit button's 16 px side padding. */
        lv_obj_set_width(cll, CH_DETAIL_W - 20 - 40);
        lv_obj_set_style_text_align(cll, LV_TEXT_ALIGN_CENTER, 0);
        y += 42;
    }

    /* ── Range section ──────────────────────────────────────────────── */
    _detail_section_label(s_detail_pane, "Range", y);
    y += CH_SECTION_H;

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

    /* Source-unit picker — the NATIVE unit the ECU actually transmits. Change
     * it if the ECU is set to a different unit than the channel default (e.g.
     * MaxxECU switched to °F or psi); re-expresses range + thresholds so they
     * stay correct. Only shown when the unit has alternatives. */
    {
        char src_opts[96];
        uint16_t src_sel = 0;
        if (_source_dd_options(c, src_opts, sizeof(src_opts), &src_sel) > 1) {
            _make_dropdown_row(s_detail_pane, y, "Received", src_opts,
                               src_sel, DD_SOURCE_UNITS);
            y += 38;
        }
    }

    /* Display-unit picker — only when the conversion table can actually
     * convert this channel's native unit somewhere (kPa→bar/psi/…, °C→°F/K,
     * km/h→mph). Picking a unit rebuilds the pane immediately so the hero,
     * range and thresholds all recalculate on the spot. */
    {
        char units_opts[96];
        uint16_t units_sel = 0;
        if (_units_dd_options(c, units_opts, sizeof(units_opts), &units_sel) > 1) {
            _make_dropdown_row(s_detail_pane, y, "Display", units_opts,
                               units_sel, DD_UNITS);
            y += 38;
        }
    }

    /* ── Thresholds section ─────────────────────────────────────────── */
    _detail_section_label(s_detail_pane, "Warnings", y);
    y += CH_SECTION_H;

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
        lv_obj_t *reset = uk_btn(s_detail_pane, UK_ICON_RESET, "Reset to defaults",
                                 UK_BTN_NEUTRAL, _reset_defaults_cb, NULL);
        lv_obj_set_size(reset, CH_DETAIL_W - 20, 34);
        lv_obj_align(reset, LV_ALIGN_TOP_LEFT, 0, y);
        y += 44;
    }

    /* ── CAN DECODE section ──────────────────────────────────────────────
     * Only for channels whose bound signal is a real CAN broadcast — OBD2
     * and internal signals have no user-editable bit decode. Edits the
     * SIGNAL (not the channel); applied live + persisted to the layout. */
    signal_t *sig = (c->signal_index >= 0)
                    ? signal_get_by_index((uint16_t)c->signal_index) : NULL;
    if (sig && sig->source == SIGNAL_SOURCE_CAN) {
        _detail_section_label(s_detail_pane, "CAN decode", y);
        y += CH_SECTION_H;

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

    /* ── Remove ──────────────────────────────────────────────────────────
     * Any active channel can leave this dash (ADR-0034): custom is gone
     * for good, canonical returns to the catalogue and can be re-added.
     * Two taps — the first arms and re-labels, any pane re-render
     * disarms. The kit's danger button: a soft fill, danger text. */
    lv_obj_t *rm = uk_btn(s_detail_pane, UK_ICON_NONE,
                          c->is_canonical
                              ? "Remove from this dash (returns to list)"
                              : "Remove from this dash",
                          UK_BTN_DANGER, _remove_channel_cb, NULL);
    lv_obj_set_size(rm, CH_DETAIL_W - 20, 34);
    lv_obj_align(rm, LV_ALIGN_TOP_LEFT, 0, y);
    y += 42;

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
    /* A background OBD2 setup (a Falcon's preset, ADR-0073) is news worth
     * the hero line: channels are about to appear that aren't there yet. */
    const char *tail = obd2_autosetup_checking() ? "checking for OBD2..."
                     : obd2_autosetup_running() ? "adding OBD2 readings..."
                     : obd2_autosetup_pending() ? "OBD2 waits for the car"
                     : "tap any channel";
    char stat_buf[112];
    if (ecu_make[0]) {
        snprintf(stat_buf, sizeof(stat_buf),
                 "%s %s  -  %u bound of %u active  -  %s",
                 ecu_make, ecu_ver, n_bound, n_active, tail);
    } else {
        snprintf(stat_buf, sizeof(stat_buf),
                 "%u bound of %u active  -  %s",
                 n_bound, n_active, tail);
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

/* Append one row to s_channels_list_box. `id` is COPIED into the
 * wizard-owned pool — never held as a pointer into a channel record,
 * whose lifetime now ends at removal (ADR-0034). `label` may be
 * temporary; LVGL copies it. */
static void _append_channel_row(const char *id, const char *label) {
    if (s_channels_count >= WIZ_CH_MAX_ROWS) return;
    strncpy(s_channels_id_pool[s_channels_count], id,
            sizeof(s_channels_id_pool[0]) - 1);
    s_channels_id_pool[s_channels_count][sizeof(s_channels_id_pool[0]) - 1] = '\0';
    id = s_channels_id_pool[s_channels_count];

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
    lv_obj_set_style_text_color(name, _row_name_color(ghost, selected), 0);

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

/* Add a group divider — the kit's muted caps section label, straight into
 * the flex list. `title` is copied (uppercased) into the label. */
static void _append_group_header(const char *title, bool first) {
    lv_obj_t *htxt = uk_section(s_channels_list_box, title);
    lv_obj_set_width(htxt, CH_ROW_W);
    lv_label_set_long_mode(htxt, LV_LABEL_LONG_DOT);
    lv_obj_set_style_pad_left(htxt, 4, 0);
    lv_obj_set_style_pad_top(htxt, first ? 2 : 8, 0);
}

/* Section heading — one of the list's two halves (ADR-0071). Primary text
 * and a size up from a group header, because it contains group headers. */
static void _append_section_header(const char *title, bool first) {
    lv_obj_t *htxt = uk_label(s_channels_list_box, title, UK_FONT_HEAD, UK_TONE_TEXT);
    lv_obj_set_width(htxt, CH_ROW_W);
    lv_obj_set_style_pad_left(htxt, 4, 0);
    lv_obj_set_style_pad_top(htxt, first ? 0 : 12, 0);
}

/* Build the full channel list: the channels this car has (by group, custom
 * ones in their own group), "+ Add custom channel", then the catalogue's
 * channels that are not set up, as ghosts (activate-on-tap).
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

    /* Two halves, as on the web page (ADR-0071): the channels this car has,
     * then the rest of the catalogue.
     *
     * This used to be the catalogue in group order with each row either
     * set up or a ghost, and custom channels in a Custom section after all
     * of it. Two things went wrong with that, one of them silently:
     *
     *  - Applying an ECU makes a custom_ channel for every signal with no
     *    canonical home (most of a Haltech's 69, most of a MaxxECU's 100).
     *    The catalogue alone is 135 rows against a 144-row cap, so the
     *    custom section got about seven rows and every other channel the
     *    ECU had just created was simply not in the list. The cap's own
     *    comment still said "~90" canonical channels.
     *  - The step exists to show what the ECU just set up, and that was
     *    spread across fourteen group headings between greyed ghosts.
     *
     * Putting the car first means a full list truncates the catalogue's
     * tail — which says so, and which Studio can still add — never a channel
     * that exists. The cap stays where it was: every row costs an
     * lv_obj_is_valid() tree walk per refresh in LVGL v8, and a longer list
     * is not something to take on without a dash to measure it on. */
    /* No counts in these headings: "Add this channel" updates its row in
     * place (no list rebuild, so nothing jumps under your finger), which
     * would leave a count stale. The hero's "N bound of M active" is live. */
    size_t mgr_count = channel_manager_count();
    _append_section_header("On this car", true);

    uint16_t n_mine = 0;
    for (channel_group_t g = 0; g < CHGRP__COUNT; g++) {
        bool first_in_group = true;
        /* Built-in channels in catalogue order (curated: RPM before
         * Absolute Load), then anything else the car has filed under the
         * same group — custom channels carry their own group. */
        for (size_t i = 0; i < CANONICAL_CHANNEL_COUNT; i++) {
            const canonical_channel_def_t *def = &CANONICAL_CHANNELS[i];
            if (def->group != g) continue;
            const channel_t *c = channel_manager_get(def->id);
            if (!c || s_channels_count >= WIZ_CH_MAX_ROWS) continue;
            if (first_in_group) {
                _append_group_header(channel_group_name(g), false);
                first_in_group = false;
            }
            _append_channel_row(c->id, c->label);
            n_mine++;
        }
        for (size_t i = 0; i < mgr_count; i++) {
            const channel_t *c = channel_manager_at(i);
            if (!c || c->group != g || canonical_channel_exists(c->id)) continue;
            if (s_channels_count >= WIZ_CH_MAX_ROWS) break;
            if (first_in_group) {
                _append_group_header(channel_group_name(g), false);
                first_in_group = false;
            }
            _append_channel_row(c->id, c->label);
            n_mine++;
        }
    }
    /* A channel with no group we know. */
    bool other_header = false;
    for (size_t i = 0; i < mgr_count; i++) {
        const channel_t *c = channel_manager_at(i);
        if (!c || (int)c->group < (int)CHGRP__COUNT || canonical_channel_exists(c->id)) continue;
        if (s_channels_count >= WIZ_CH_MAX_ROWS) break;
        if (!other_header) { _append_group_header("Custom", false); other_header = true; }
        _append_channel_row(c->id, c->label);
        n_mine++;
    }
    if (n_mine == 0) {
        lv_obj_t *none = lv_label_create(s_channels_list_box);
        lv_label_set_text(none,
            "Nothing set up yet. Tap a channel below to\nadd it, or go back and pick your ECU.");
        lv_obj_set_width(none, CH_ROW_W);
        lv_obj_set_style_pad_left(none, 4, 0);
        lv_obj_set_style_text_font(none, THEME_FONT_TINY, 0);
        lv_obj_set_style_text_color(none, THEME_COLOR_TEXT_MUTED, 0);
    }

    /* "+ Add custom channel" belongs with the car it adds to — at the
     * bottom it sat under 135 catalogue rows. */
    lv_obj_t *add_btn = uk_btn(s_channels_list_box, UK_ICON_PLUS, "Add custom channel",
                               UK_BTN_NEUTRAL, _add_custom_btn_cb, NULL);
    lv_obj_set_size(add_btn, CH_ROW_W, 36);

    /* ── Not set up: the rest of the catalogue, tap a ghost to add it. ── */
    uint16_t n_ghosts = 0;
    for (size_t i = 0; i < CANONICAL_CHANNEL_COUNT; i++)
        if (!channel_manager_get(CANONICAL_CHANNELS[i].id)) n_ghosts++;
    uint16_t n_dropped = 0;
    if (n_ghosts) {
        _append_section_header("Not set up", false);
        for (channel_group_t g = 0; g < CHGRP__COUNT; g++) {
            bool first_in_group = true;
            for (size_t i = 0; i < CANONICAL_CHANNEL_COUNT; i++) {
                const canonical_channel_def_t *def = &CANONICAL_CHANNELS[i];
                if (def->group != g || channel_manager_get(def->id)) continue;
                if (s_channels_count >= WIZ_CH_MAX_ROWS) { n_dropped++; continue; }
                if (first_in_group) {
                    _append_group_header(channel_group_name(g), false);
                    first_in_group = false;
                }
                _append_channel_row(def->id, def->label);
            }
        }
    }
    if (n_dropped) {
        lv_obj_t *more = lv_label_create(s_channels_list_box);
        lv_label_set_text_fmt(more,
            "%u more in the standard list - add them\nfrom Channels in RDM Studio.",
            (unsigned)n_dropped);
        lv_obj_set_width(more, CH_ROW_W);
        lv_obj_set_style_pad_left(more, 4, 0);
        lv_obj_set_style_pad_top(more, 6, 0);
        lv_obj_set_style_text_font(more, THEME_FONT_TINY, 0);
        lv_obj_set_style_text_color(more, THEME_COLOR_TEXT_MUTED, 0);
    }

    /* On an apply-to-widget open, jump the list to the bound channel. */
    if (s_apply_to_widget_mode) _scroll_to_selected();
}

/* ── OBD2 scan → channels ─────────────────────────────────────────────────
 *
 * Ask the car what it supports, offer the answers as channels. This
 * replaced a speculative gap-fill probe (enable the PIDs a channel would
 * need, poll for 3.2 s, keep whatever answered): the probe could only ever
 * bind channels that were already active, and it guessed rather than asked.
 * Discovery is the car's own list, and channel_obd2_matches() — the same
 * resolver /api/obd2/scan uses — turns it into channels, so the dash and
 * the web page answer identically. See ADR-0037. */

static void _obd2_chip_update(void);
static void _obd2_open_scan_modal(void);
static void _obd2_begin_scan(void);
static void _obd2_render_results(void);

/* Closing mid-scan is safe: discovery finishes in the background and
 * _obd2_scan_done_cb bails when the modal is gone. Nothing is left polling
 * that wasn't polling before — unlike the probe this replaced, the scan
 * never enables speculative PIDs. */
static void _obd2_close_modal(void) {
    if (s_obd2_modal && lv_obj_is_valid(s_obd2_modal)) {
        lv_obj_del(s_obd2_modal);
    }
    s_obd2_modal = s_obd2_status_lbl = NULL;
    s_obd2_rescan_btn = s_obd2_spinner = NULL;
    s_obd2_list = s_obd2_add_btn = NULL;
    s_obd2_match_count = 0;
}

/* Show/hide the spinner + buttons for the current modal phase. */
static void _obd2_modal_set_busy(bool busy) {
    if (s_obd2_spinner && lv_obj_is_valid(s_obd2_spinner)) {
        if (busy) lv_obj_clear_flag(s_obd2_spinner, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(s_obd2_spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_obd2_rescan_btn && lv_obj_is_valid(s_obd2_rescan_btn)) {
        if (busy) lv_obj_add_flag(s_obd2_rescan_btn, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_clear_flag(s_obd2_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_obd2_add_btn && lv_obj_is_valid(s_obd2_add_btn)) {
        if (busy) lv_obj_add_flag(s_obd2_add_btn, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_clear_flag(s_obd2_add_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* How many offers are still unticked-able (not already sourced). */
static uint8_t _obd2_fresh_count(void) {
    uint8_t n = 0;
    for (size_t i = 0; i < s_obd2_match_count; i++)
        if (!s_obd2_matches[i].bound) n++;
    return n;
}

static uint8_t _obd2_picked_count(void) {
    uint8_t n = 0;
    for (size_t i = 0; i < s_obd2_match_count; i++)
        if (s_obd2_pick[i]) n++;
    return n;
}

/* Bind everything ticked, then report. */
static void _obd2_apply_picks(void) {
    obd2_channel_match_t rows[CH_OBD2_MATCH_MAX];
    size_t n = 0;
    for (size_t i = 0; i < s_obd2_match_count && n < CH_OBD2_MATCH_MAX; i++)
        if (s_obd2_pick[i]) rows[n++] = s_obd2_matches[i];
    if (n == 0) return;

    size_t bound = 0;
    esp_err_t err = channel_apply_obd2(rows, n, &bound);
    if (bound > 0) {
        /* The owner chose from the car's own list: that choice is the OBD2
         * setup now. A background one still owed would otherwise bind the
         * readings they left unticked the next time it ran (ADR-0073). */
        obd2_autosetup_cancel();
        s_channels_changed = true;
        _populate_channels_list();
        _refresh_hero_stats();
        _channels_refresh_cb(NULL);
    }

    if (s_obd2_list && lv_obj_is_valid(s_obd2_list))
        lv_obj_add_flag(s_obd2_list, LV_OBJ_FLAG_HIDDEN);
    if (s_obd2_add_btn && lv_obj_is_valid(s_obd2_add_btn))
        lv_obj_add_flag(s_obd2_add_btn, LV_OBJ_FLAG_HIDDEN);
    if (s_obd2_status_lbl && lv_obj_is_valid(s_obd2_status_lbl)) {
        char msg[200];
        if (bound == 0) {
            snprintf(msg, sizeof(msg), "Nothing could be added.");
        } else if (err == ESP_ERR_NO_MEM) {
            snprintf(msg, sizeof(msg),
                     "Added %u channel%s. The OBD2 list is full (48 max), so\n"
                     "some were left out.", (unsigned)bound, bound == 1 ? "" : "s");
        } else if (err == ESP_FAIL) {
            snprintf(msg, sizeof(msg),
                     "Added %u channel%s - working now, but they could not be\n"
                     "saved, so they won't survive a restart.",
                     (unsigned)bound, bound == 1 ? "" : "s");
        } else {
            snprintf(msg, sizeof(msg), "Added %u channel%s from OBD2.",
                     (unsigned)bound, bound == 1 ? "" : "s");
        }
        lv_label_set_text(s_obd2_status_lbl, msg);
        lv_obj_set_style_text_color(s_obd2_status_lbl,
                                    bound ? THEME_COLOR_STATUS_CONNECTED
                                          : THEME_COLOR_TEXT_MUTED, 0);
    }
    _obd2_chip_update();
    ESP_LOGI(TAG, "OBD2 scan: added %u channel(s)", (unsigned)bound);
}

static void _obd2_add_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _obd2_apply_picks();
}

/* Row tap toggles its tick. The row's user_data carries the match index. */
static void _obd2_row_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t *row = lv_event_get_target(e);
    if (!row) return;
    intptr_t idx = (intptr_t)lv_obj_get_user_data(row);
    if (idx < 0 || (size_t)idx >= s_obd2_match_count) return;
    if (s_obd2_matches[idx].bound) return;          /* already set up */
    s_obd2_pick[idx] = !s_obd2_pick[idx];
    _obd2_render_results();
}

/* Paint the offer list + the Add button's label. */
static void _obd2_render_results(void) {
    if (!s_obd2_list || !lv_obj_is_valid(s_obd2_list)) return;
    lv_obj_clean(s_obd2_list);
    lv_obj_clear_flag(s_obd2_list, LV_OBJ_FLAG_HIDDEN);

    for (size_t i = 0; i < s_obd2_match_count; i++) {
        const obd2_channel_match_t *m = &s_obd2_matches[i];
        bool picked = !m->bound && s_obd2_pick[i];
        lv_obj_t *row = lv_obj_create(s_obd2_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 34);
        lv_obj_set_style_pad_hor(row, 10, 0);
        /* Kit checkable row: neutral raised row, ticked = accent tint. */
        lv_obj_set_style_bg_color(row,
            picked ? THEME_COLOR_ACCENT_DIM : THEME_COLOR_CONTROL_BG, 0);   /* accent_soft : raised */
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, UK_R_BTN, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(row, (void *)(intptr_t)i);
        if (!m->bound) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, _obd2_row_cb, LV_EVENT_CLICKED, NULL);
        }

        /* Tick / empty / "already set up" all in one leading column so
         * the eye reads one column, not three states in three places. */
        lv_obj_t *mark = uk_icon(row, UK_ICON_CHECK, UK_ICON_MD,
                                 m->bound ? UK_TONE_MUTED : UK_TONE_ACCENT);
        lv_obj_align(mark, LV_ALIGN_LEFT_MID, 0, 0);
        if (!m->bound && !picked) lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, m->label);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 30, 0);
        lv_obj_set_style_text_font(name, THEME_FONT_BODY, 0);
        lv_obj_set_style_text_color(name,
            m->bound ? THEME_COLOR_TEXT_MUTED
                     : (picked ? ui_pal->accent_ink : THEME_COLOR_TEXT_PRIMARY), 0);

        lv_obj_t *note = lv_label_create(row);
        if (m->bound) lv_label_set_text(note, "already set up");
        else          lv_label_set_text(note, m->units && m->units[0] ? m->units : "");
        lv_obj_align(note, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_text_font(note, THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(note, THEME_COLOR_TEXT_MUTED, 0);
    }

    if (s_obd2_add_btn && lv_obj_is_valid(s_obd2_add_btn)) {
        uint8_t picked = _obd2_picked_count();
        lv_obj_t *lbl = (lv_obj_t *)lv_obj_get_user_data(s_obd2_add_btn);
        if (lbl && lv_obj_is_valid(lbl)) {
            char b[32];
            snprintf(b, sizeof(b), "Add %u", picked);
            lv_label_set_text(lbl, picked ? b : "Add");
        }
        if (picked) lv_obj_clear_state(s_obd2_add_btn, LV_STATE_DISABLED);
        else        lv_obj_add_state(s_obd2_add_btn, LV_STATE_DISABLED);
        lv_obj_clear_flag(s_obd2_add_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Discovery finished (LVGL task — obd2 calls back from the poll timer). */
static void _obd2_scan_done_cb(const obd2_scan_result_t *r, void *user) {
    (void)user;
    if (!s_obd2_modal || !lv_obj_is_valid(s_obd2_modal)) return;  /* closed */

    _obd2_modal_set_busy(false);
    s_obd2_match_count = r ? channel_obd2_matches(r->pids, r->count,
                                                  s_obd2_matches,
                                                  CH_OBD2_MATCH_MAX) : 0;
    /* What can be added first, what is already set up after it. In the
     * resolver's order the five channels an ECU already feeds (RPM, coolant,
     * speed...) came first and pushed every row that could be added below
     * the fold — the list is there to answer "what could OBD2 give me that I
     * don't have", so that is the top of it (seen on the dash, 2026-09-14).
     * Stable, so each half keeps the resolver's order. */
    {
        static EXT_RAM_BSS_ATTR obd2_channel_match_t tmp[CH_OBD2_MATCH_MAX];
        size_t k = 0;
        for (size_t i = 0; i < s_obd2_match_count; i++)
            if (!s_obd2_matches[i].bound) tmp[k++] = s_obd2_matches[i];
        for (size_t i = 0; i < s_obd2_match_count; i++)
            if (s_obd2_matches[i].bound) tmp[k++] = s_obd2_matches[i];
        memcpy(s_obd2_matches, tmp, s_obd2_match_count * sizeof(tmp[0]));
    }
    memset(s_obd2_pick, 0, sizeof(s_obd2_pick));
    for (size_t i = 0; i < s_obd2_match_count; i++)
        s_obd2_pick[i] = !s_obd2_matches[i].bound;   /* pre-tick the gaps */

    uint8_t fresh = _obd2_fresh_count();
    /* Readings, not scan scaffolding: the supported-PID blocks (0x20, 0x40,
     * ...) are in the answer too, which made this sheet say 67 where the ECU
     * step, counting readings, said 61 for the same car. */
    unsigned readings = 0;
    for (uint8_t i = 0; r && i < r->count; i++)
        if ((r->pids[i] & 0x1F) != 0) readings++;
    if (s_obd2_status_lbl && lv_obj_is_valid(s_obd2_status_lbl)) {
        char msg[200];
        if (!r || r->count == 0) {
            snprintf(msg, sizeof(msg),
                     "The car didn't answer. Check the ignition is on - some\n"
                     "cars only answer with the engine running.");
        } else if (fresh == 0) {
            snprintf(msg, sizeof(msg),
                     "Your car answered %u readings, and everything it offers\n"
                     "is already set up.", readings);
        } else {
            snprintf(msg, sizeof(msg),
                     "Your car answered %u readings. %u can be added:",
                     readings, (unsigned)fresh);
        }
        lv_label_set_text(s_obd2_status_lbl, msg);
        lv_obj_set_style_text_color(s_obd2_status_lbl,
                                    fresh ? THEME_COLOR_TEXT_PRIMARY
                                          : THEME_COLOR_TEXT_MUTED, 0);
    }

    if (s_obd2_match_count == 0) {
        if (s_obd2_list && lv_obj_is_valid(s_obd2_list))
            lv_obj_add_flag(s_obd2_list, LV_OBJ_FLAG_HIDDEN);
        if (s_obd2_add_btn && lv_obj_is_valid(s_obd2_add_btn))
            lv_obj_add_flag(s_obd2_add_btn, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    _obd2_render_results();
}

static void _obd2_begin_scan(void) {
    if (obd2_discovery_in_progress()) return;
    _obd2_modal_set_busy(true);
    if (s_obd2_list && lv_obj_is_valid(s_obd2_list))
        lv_obj_add_flag(s_obd2_list, LV_OBJ_FLAG_HIDDEN);
    if (s_obd2_status_lbl && lv_obj_is_valid(s_obd2_status_lbl)) {
        lv_label_set_text(s_obd2_status_lbl,
            "Asking the car what it can report...\n"
            "Trying both bus speeds and both addressing modes.");
        lv_obj_set_style_text_color(s_obd2_status_lbl,
                                    THEME_COLOR_TEXT_PRIMARY, 0);
    }
    obd2_discovery_start(_obd2_scan_done_cb, NULL);
}

static void _obd2_rescan_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _obd2_begin_scan();
}

static void _obd2_close_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _obd2_close_modal();
}

static void _obd2_open_scan_modal(void) {
    if (s_obd2_modal) return;  /* already open */

    /* Kit sheet over the wizard. Owned by s_overlay (wizard lifecycle);
     * s_obd2_modal is the backdrop, which _obd2_close_modal deletes. */
    #define OBD2_SHEET_W 520
    #define OBD2_SHEET_H 364
    const lv_coord_t inner_w = OBD2_SHEET_W - 2 * SHEET_PAD;   /* 484 */
    lv_obj_t *card = NULL;
    s_obd2_modal = _wiz_sheet_open(OBD2_SHEET_W, OBD2_SHEET_H,
                                   "Add channels from OBD2",
                                   _obd2_close_cb, false, &card);

    /* Busy spinner in the header, left of the X. */
    s_obd2_spinner = lv_spinner_create(card, 1000, 60);
    lv_obj_set_size(s_obd2_spinner, 28, 28);
    lv_obj_align(s_obd2_spinner, LV_ALIGN_TOP_RIGHT, -52, 0);
    _wiz_style_spinner(s_obd2_spinner);

    s_obd2_status_lbl = lv_label_create(card);
    lv_label_set_long_mode(s_obd2_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_obd2_status_lbl, inner_w);
    lv_obj_align(s_obd2_status_lbl, LV_ALIGN_TOP_LEFT, 0, 42);
    lv_obj_set_style_text_font(s_obd2_status_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_obd2_status_lbl, THEME_COLOR_TEXT_PRIMARY, 0);

    /* Scrollable offer list between the message and the buttons. */
    s_obd2_list = lv_obj_create(card);
    lv_obj_remove_style_all(s_obd2_list);
    lv_obj_set_size(s_obd2_list, inner_w, 192);
    lv_obj_align(s_obd2_list, LV_ALIGN_TOP_LEFT, 0, 82);
    lv_obj_set_style_bg_opa(s_obd2_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_obd2_list, 0, 0);
    lv_obj_set_style_pad_right(s_obd2_list, 8, 0);   /* rows clear of the scrollbar */
    lv_obj_set_style_pad_row(s_obd2_list, 4, 0);
    lv_obj_set_flex_flow(s_obd2_list, LV_FLEX_FLOW_COLUMN);
    uk_style_scrollbar(s_obd2_list);
    lv_obj_add_flag(s_obd2_list, LV_OBJ_FLAG_HIDDEN);

    /* Rescan (left) + Add + Done along the bottom. */
    s_obd2_rescan_btn = uk_btn(card, UK_ICON_RESET, "Rescan", UK_BTN_NEUTRAL,
                               _obd2_rescan_cb, NULL);
    lv_obj_set_size(s_obd2_rescan_btn, 120, BTN_H);
    lv_obj_align(s_obd2_rescan_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_flag(s_obd2_rescan_btn, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *close_btn = uk_btn(card, UK_ICON_NONE, "Done", UK_BTN_NEUTRAL,
                                 _obd2_close_cb, NULL);
    lv_obj_set_size(close_btn, 100, BTN_H);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    s_obd2_add_btn = uk_btn(card, UK_ICON_PLUS, "Add", UK_BTN_PRIMARY,
                            _obd2_add_cb, NULL);
    lv_obj_set_size(s_obd2_add_btn, 120, BTN_H);
    lv_obj_align_to(s_obd2_add_btn, close_btn, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    /* _obd2_render_results relabels it ("Add 5") through this pointer. */
    lv_obj_set_user_data(s_obd2_add_btn, uk_btn_label(s_obd2_add_btn));
    lv_obj_add_flag(s_obd2_add_btn, LV_OBJ_FLAG_HIDDEN);

    /* A standing offer is an answer the car already gave (a CHECK after the
     * ECU preset went on, ADR-0074): show it rather than ask again. Rescan
     * is one tap away if the car has changed since. */
    obd2_scan_result_t offer;
    if (obd2_autosetup_offer_scan(&offer)) _obd2_scan_done_cb(&offer, NULL);
    else                                   _obd2_begin_scan();
}

static void _obd2_chip_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    _obd2_open_scan_modal();
}

/* Create / refresh the "Scan for OBD2" chip in the channels hero.
 *
 * It used to appear only when there were active-but-unbound channels with
 * an OBD2 equivalent, which meant the one surface that could answer "what
 * can this car give me" was hidden from anyone whose channels all happened
 * to be bound — including the common case of a fresh dash with nothing set
 * up at all. It is now always offered. */
static void _obd2_chip_update(void) {
    if (!s_step_channels || !lv_obj_is_valid(s_step_channels)) return;

    if (!s_obd2_chip || !lv_obj_is_valid(s_obd2_chip)) {
        /* A neutral kit button sized as a chip; Continue keeps the screen's
         * one red. */
        s_obd2_chip = uk_btn(s_step_channels, UK_ICON_OBD, "Scan for OBD2",
                             UK_BTN_NEUTRAL, _obd2_chip_cb, NULL);
        lv_obj_set_height(s_obd2_chip, 30);
        lv_obj_set_style_pad_hor(s_obd2_chip, 12, 0);
    }

    /* "Your car also answers OBD2": when the check after the ECU preset
     * found readings for channels nothing feeds, the chip counts them and
     * turns "on" (accent tint), so the offer is on the screen where the
     * car's channels are reviewed instead of behind a scan nobody knew to
     * run (ADR-0074). */
    {
        size_t n = obd2_autosetup_offer(NULL, CH_OBD2_MATCH_MAX, NULL);
        char b[32];
        if (n) snprintf(b, sizeof(b), "OBD2 can add %u", (unsigned)n);
        uk_btn_set_text(s_obd2_chip, n ? b : "Scan for OBD2");
        uk_btn_set_kind(s_obd2_chip, n ? UK_BTN_ON : UK_BTN_NEUTRAL);
    }

    /* Top-right of the hero row, level with the stats line (update_layout
     * settles SIZE_CONTENT before aligning). */
    lv_obj_update_layout(s_obd2_chip);
    lv_obj_align(s_obd2_chip, LV_ALIGN_TOP_RIGHT, 0, 0);
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

/* A background OBD2 setup finished an attempt while the channels step is up:
 * show what it connected without waiting for someone to tap. */
static void _channels_obd2_autosetup_cb(const obd2_autosetup_result_t *r, void *user) {
    (void)user;
    if (r && r->bound) s_channels_changed = true;
    if (!s_step_channels || !lv_obj_is_valid(s_step_channels)) return;
    if (r && r->bound) {
        _populate_channels_list();
        _render_detail_pane();
    }
    _refresh_hero_stats();
    _obd2_chip_update();       /* a CHECK may have just made an offer */
    _channels_refresh_cb(NULL);
}

static void _show_step_channels(void) {
    can_bus_test_set_ui_callback(NULL);

    /* Arriving from the OBD2 screen: take it down. */
    if (s_step_obd2 && lv_obj_is_valid(s_step_obd2)) lv_obj_del(s_step_obd2);
    s_step_obd2 = NULL;
    s_obd2_step_status = s_obd2_step_spinner = s_obd2_step_rescan_btn = NULL;

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

    /* The title ("Channels"), the mode line and Close live in the kit bar;
     * the body is already the size the split pane is laid out for. The
     * bar's Close is shown in every mode so the editor always has a visible
     * way out at the top — the same _close_wizard(false) the old "×" ran. */
    _wiz_bar_set("Channels",
                 s_apply_to_widget_mode ? "Pick a channel for this widget"
                 : s_standalone_channels ? "Changes save automatically"
                                         : "Step 3 of 4",
                 true);

    s_step_channels = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_step_channels);
    lv_obj_set_size(s_step_channels, CH_W, CH_H);
    lv_obj_align(s_step_channels, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_clear_flag(s_step_channels, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Hero line (full width) ───────────────────────────────────── */
    s_channels_stats_lbl = lv_label_create(s_step_channels);
    lv_label_set_text(s_channels_stats_lbl, "");
    /* Cap width + dot-truncate so a long stats line never runs under the
     * OBD2 chip pinned to the hero's right edge. */
    lv_label_set_long_mode(s_channels_stats_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_channels_stats_lbl, CH_W - 220);
    lv_obj_align(s_channels_stats_lbl, LV_ALIGN_TOP_LEFT, 2, 7);
    lv_obj_set_style_text_font(s_channels_stats_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_channels_stats_lbl,
                                THEME_COLOR_TEXT_MUTED, 0);

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
    uk_style_scrollbar(s_channels_list_box);

    /* ── RIGHT pane: detail (built by _render_detail_pane) ────────── */
    /* A kit card (card fill, hairline, radius 10) that scrolls. Built from a
     * plain object rather than uk_card() so it keeps the clickable +
     * scrollable flags a drag on empty pane needs. */
    s_detail_pane = lv_obj_create(s_step_channels);
    lv_obj_remove_style_all(s_detail_pane);
    lv_obj_set_size(s_detail_pane, CH_DETAIL_W, CH_BODY_H);
    lv_obj_align(s_detail_pane, LV_ALIGN_TOP_LEFT,
                 CH_LIST_W + CH_PANE_GAP, CH_BODY_TOP);
    lv_obj_set_style_bg_color(s_detail_pane, THEME_COLOR_PANEL, 0);   /* card */
    lv_obj_set_style_bg_opa(s_detail_pane, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_detail_pane, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s_detail_pane, 1, 0);
    lv_obj_set_style_radius(s_detail_pane, UK_R_TILE, 0);
    lv_obj_set_style_pad_all(s_detail_pane, 10, 0);
    /* Right-pad bumped so controls don't sit under the scrollbar. */
    lv_obj_set_style_pad_right(s_detail_pane, 14, 0);
    lv_obj_set_scroll_dir(s_detail_pane, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_detail_pane, LV_SCROLLBAR_MODE_AUTO);
    uk_style_scrollbar(s_detail_pane);

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
    /* Listen for a background OBD2 setup (a Falcon preset just applied, or
     * the OBD2 screen left mid-scan) — cheap, and replaces the OBD2 screen's
     * listener, whose screen is gone. */
    if (obd2_autosetup_running() || obd2_autosetup_pending() ||
        obd2_autosetup_checking())
        obd2_autosetup_listen(_channels_obd2_autosetup_cb, NULL);

    if (s_apply_to_widget_mode) {
        /* "Widget settings" (legacy config modal) only when the target has
         * quick-settings content — meter/text/arc are channels-only, so the
         * button is hidden and "Apply to widget" takes the full width. */
        bool has_qs = config_modal_has_content(s_apply_target_widget);

        if (has_qs) {
            lv_obj_t *adv = uk_btn(s_step_channels, UK_ICON_GEAR, "Widget settings",
                                   UK_BTN_NEUTRAL, _btn_widget_settings_cb, NULL);
            lv_obj_set_size(adv, 180, BTN_H);
            lv_obj_align(adv, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        }

        lv_obj_t *cont = uk_btn(s_step_channels, UK_ICON_CHECK, "Apply to widget",
                                UK_BTN_PRIMARY, _btn_apply_to_widget_cb, NULL);
        lv_obj_set_size(cont, has_qs ? (CH_W - 180 - UK_GAP) : CH_W, BTN_H);
        lv_obj_align(cont, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    } else {
        /* Continue button — full-width along the bottom of the body. */
        lv_obj_t *cont = uk_btn(s_step_channels, UK_ICON_NONE,
                                s_standalone_channels ? "Done" : "Continue",
                                UK_BTN_PRIMARY, _btn_channels_continue_cb, NULL);
        lv_obj_set_size(cont, CH_W, BTN_H);
        lv_obj_align(cont, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    /* The OBD2 chip ("Scan for OBD2" / "OBD2 can add N") — always offered.
     * Created last so it sits above the list/detail in the hero's
     * top-right. */
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

/* ── Calculate (math channel) sheet ────────────────────────────────────────
 * On-device twin of the web editor's Calculate form: operand A (a channel)
 * <op> operand B (a channel OR a typed number) → this channel. Mirrors the
 * /api/channels/update {math:{a,b,op}} contract via channel_math_set. */

/* Upper bound on channels offered in the operand dropdowns. */
#define CHM_MAX_FOR_UI 96

static float     s_math_b_const   = 0.0f;
static float     s_math_c_const   = 0.0f;
/* Index→channel-id map shared by both operand dropdowns. The target
 * channel itself is excluded (no self-reference). */
static EXT_RAM_BSS_ATTR char      s_math_ids[CHM_MAX_FOR_UI][40];
static uint16_t  s_math_id_count  = 0;

static void _close_math_sheet(void) {
    if (s_math_sheet && lv_obj_is_valid(s_math_sheet)) lv_obj_del(s_math_sheet);
    s_math_sheet = s_math_a_dd = s_math_op_dd = s_math_b_dd = s_math_bval_lbl = NULL;
    s_math_op2_dd = s_math_c_dd = s_math_cval_lbl = NULL;
}

static void _math_close_cb(lv_event_t *e) { (void)e; _close_math_sheet(); }

/* Numeric keypad for operand-B-as-constant. */
static void _math_bconst_confirmed(const char *text, void *ud) {
    (void)ud;
    if (text && text[0]) s_math_b_const = strtof(text, NULL);
    if (s_math_bval_lbl && lv_obj_is_valid(s_math_bval_lbl)) {
        char b[24]; snprintf(b, sizeof(b), "%g", s_math_b_const);
        lv_label_set_text(s_math_bval_lbl, b);
    }
}
static void _math_bconst_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    char initial[24]; snprintf(initial, sizeof(initial), "%g", s_math_b_const);
    show_numeric_input_dialog("Number", initial, _math_bconst_confirmed, NULL, NULL);
}

static void _math_cconst_confirmed(const char *text, void *ud) {
    (void)ud;
    if (text && text[0]) s_math_c_const = strtof(text, NULL);
    if (s_math_cval_lbl && lv_obj_is_valid(s_math_cval_lbl)) {
        char b[24]; snprintf(b, sizeof(b), "%g", s_math_c_const);
        lv_label_set_text(s_math_cval_lbl, b);
    }
}
static void _math_cconst_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    char initial[24]; snprintf(initial, sizeof(initial), "%g", s_math_c_const);
    show_numeric_input_dialog("Number", initial, _math_cconst_confirmed, NULL, NULL);
}

static void _math_apply_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    channel_t *c = channel_manager_get(s_selected_ch_id);
    if (!c || !s_math_a_dd) { _close_math_sheet(); return; }

    uint16_t ai   = lv_dropdown_get_selected(s_math_a_dd);
    uint16_t opi  = lv_dropdown_get_selected(s_math_op_dd);
    uint16_t bi   = lv_dropdown_get_selected(s_math_b_dd);
    uint16_t op2i = s_math_op2_dd ? lv_dropdown_get_selected(s_math_op2_dd) : 0;
    uint16_t ci   = s_math_c_dd   ? lv_dropdown_get_selected(s_math_c_dd)   : 0;

    channel_math_operand_t oa = {0}, ob = {0}, oc = {0};
    /* A is always a channel (satisfies "≥1 operand must be a channel"). */
    if (ai >= s_math_id_count) { _close_math_sheet(); return; }
    oa.channel_id = s_math_ids[ai];
    /* B: index 0 = the typed constant, 1.. = channel (index bi-1). */
    if (bi == 0) { ob.is_const = true; ob.value = s_math_b_const; }
    else if ((uint16_t)(bi - 1) < s_math_id_count) ob.channel_id = s_math_ids[bi - 1];
    else { _close_math_sheet(); return; }
    /* C: index 0 = "(none)" — two-operand form. 1 = the typed constant,
     * 2.. = channel (index ci-2). */
    bool has_c = (ci != 0);
    if (ci == 1) { oc.is_const = true; oc.value = s_math_c_const; }
    else if (ci >= 2) {
        if ((uint16_t)(ci - 2) >= s_math_id_count) { _close_math_sheet(); return; }
        oc.channel_id = s_math_ids[ci - 2];
    }

    bool ok = channel_math_set(c, &oa, &ob, (uint8_t)(opi & 3),
                               has_c ? &oc : NULL, (uint8_t)(op2i & 3));
    ESP_LOGI(TAG, "math_set %s: (%s %u %s) %u %s -> %s", c->id,
             oa.channel_id ? oa.channel_id : "#", (unsigned)opi,
             ob.channel_id ? ob.channel_id : "#",
             (unsigned)op2i,
             has_c ? (oc.channel_id ? oc.channel_id : "#") : "-",
             ok ? "ok" : "FAILED");
    _close_math_sheet();
    /* channel_math_set persists + rebinds; re-render so SOURCE/Calc/value
     * all reflect the new derived binding. */
    _render_detail_pane();
}

static void _math_clear_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    channel_t *c = channel_manager_get(s_selected_ch_id);
    if (c) channel_math_clear(c);
    _close_math_sheet();
    _render_detail_pane();
}

/* Build the index→id map + a newline option string of channel labels,
 * excluding @skip_id. Returns option count; writes labels into @opts. */
static uint16_t _math_build_channel_options(const char *skip_id, char *opts, size_t cap) {
    s_math_id_count = 0;
    size_t pos = 0;
    size_t total = channel_manager_count();
    for (size_t i = 0; i < total && s_math_id_count < CHM_MAX_FOR_UI; i++) {
        const channel_t *ch = channel_manager_at(i);
        if (!ch || !ch->id[0]) continue;
        if (skip_id && strcmp(ch->id, skip_id) == 0) continue;  /* no self-ref */
        strncpy(s_math_ids[s_math_id_count], ch->id, sizeof(s_math_ids[0]) - 1);
        s_math_ids[s_math_id_count][sizeof(s_math_ids[0]) - 1] = '\0';
        pos += (size_t)snprintf(opts + pos, pos < cap ? cap - pos : 0,
                                s_math_id_count ? "\n%s" : "%s", ch->label);
        s_math_id_count++;
    }
    return s_math_id_count;
}

/* Find the dropdown index of a channel id in s_math_ids, or 0 if absent. */
static uint16_t _math_id_index(const char *id) {
    for (uint16_t i = 0; i < s_math_id_count; i++)
        if (strcmp(s_math_ids[i], id) == 0) return i;
    return 0;
}

static void _open_math_sheet(const channel_t *c) {
    if (!c || !s_overlay || !lv_obj_is_valid(s_overlay)) return;
    _close_math_sheet();

    /* Operand option strings — A is channels only; B prepends "Number...".
     * Three dots, not an ellipsis character: Montserrat has no glyph for it. */
    static EXT_RAM_BSS_ATTR char a_opts[2048];
    _math_build_channel_options(c->id, a_opts, sizeof(a_opts));
    if (s_math_id_count == 0) return;  /* nothing to derive from */
    static EXT_RAM_BSS_ATTR char b_opts[2048];
    int bp = snprintf(b_opts, sizeof(b_opts), "Number...");
    for (uint16_t i = 0; i < s_math_id_count; i++)
        bp += snprintf(b_opts + bp, sizeof(b_opts) - bp, "\n%s",
                       /* reuse A's labels by re-reading the channel */
                       (channel_manager_get(s_math_ids[i]) ?
                        channel_manager_get(s_math_ids[i])->label : s_math_ids[i]));

    /* C prepends "(none)" as well as "Number...": the third term is optional,
     * and "(none)" IS the two-operand expression that has always been here. */
    static EXT_RAM_BSS_ATTR char c_opts[2048];
    int cp = snprintf(c_opts, sizeof(c_opts), "(none)\nNumber...");
    for (uint16_t i = 0; i < s_math_id_count; i++)
        cp += snprintf(c_opts + cp, sizeof(c_opts) - cp, "\n%s",
                       (channel_manager_get(s_math_ids[i]) ?
                        channel_manager_get(s_math_ids[i])->label : s_math_ids[i]));

    s_math_b_const = c->math_enabled && c->math_b_is_const ? c->math_b_const : 0.0f;
    s_math_c_const = c->math_enabled && c->math_c_is_const ? c->math_c_const : 0.0f;

    /* Kit sheet; s_math_sheet is the backdrop _close_math_sheet deletes. */
    #define MATH_SHEET_W 460
    #define MATH_SHEET_H 400
    lv_obj_t *card = NULL;
    char hdr[64]; snprintf(hdr, sizeof(hdr), "Calculate %s", c->label);
    s_math_sheet = _wiz_sheet_open(MATH_SHEET_W, MATH_SHEET_H, hdr,
                                   _math_close_cb, false, &card);

    lv_coord_t cy = 44;
    /* Operand A (channel). */
    {
        _make_row_key(card, cy - 1, "A");
        s_math_a_dd = lv_dropdown_create(card);
        lv_dropdown_set_options(s_math_a_dd, a_opts);
        _wiz_style_dropdown(s_math_a_dd);
        lv_obj_set_size(s_math_a_dd, 360, 30);
        lv_obj_align(s_math_a_dd, LV_ALIGN_TOP_RIGHT, 0, cy);
        if (c->math_enabled && !c->math_a_is_const)
            lv_dropdown_set_selected(s_math_a_dd, _math_id_index(c->math_a));
        cy += 38;
    }
    /* Operator. */
    {
        _make_row_key(card, cy - 1, "Op");
        s_math_op_dd = lv_dropdown_create(card);
        lv_dropdown_set_options(s_math_op_dd, "A + B\nA - B\nA x B\nA / B");
        _wiz_style_dropdown(s_math_op_dd);
        lv_obj_set_size(s_math_op_dd, 360, 30);
        lv_obj_align(s_math_op_dd, LV_ALIGN_TOP_RIGHT, 0, cy);
        if (c->math_enabled) lv_dropdown_set_selected(s_math_op_dd, c->math_op & 3);
        cy += 38;
    }
    /* Operand B (channel or Number...) + its constant value box. */
    {
        _make_row_key(card, cy - 1, "B");
        s_math_b_dd = lv_dropdown_create(card);
        lv_dropdown_set_options(s_math_b_dd, b_opts);
        _wiz_style_dropdown(s_math_b_dd);
        lv_obj_set_size(s_math_b_dd, 250, 30);
        lv_obj_align(s_math_b_dd, LV_ALIGN_TOP_LEFT, 64, cy);
        if (c->math_enabled && !c->math_b_is_const)
            lv_dropdown_set_selected(s_math_b_dd, (uint16_t)(_math_id_index(c->math_b) + 1));
        /* Number value field (used when B = "Number...", index 0). */
        lv_obj_t *vbtn = lv_btn_create(card);
        _wiz_style_textbox(vbtn);
        lv_obj_set_size(vbtn, 96, 30);
        lv_obj_align(vbtn, LV_ALIGN_TOP_RIGHT, 0, cy);
        s_math_bval_lbl = lv_label_create(vbtn);
        char vb[24]; snprintf(vb, sizeof(vb), "%g", s_math_b_const);
        lv_label_set_text(s_math_bval_lbl, vb);
        lv_obj_center(s_math_bval_lbl);
        lv_obj_set_style_text_font(s_math_bval_lbl, THEME_FONT_SMALL, 0);
        lv_obj_add_event_cb(vbtn, _math_bconst_cb, LV_EVENT_CLICKED, NULL);
        cy += 44;
    }
    /* Second operator. Always enabled, but it only means anything once C is
     * something other than "(none)" — cheaper to read than a control that
     * greys itself out and back. */
    {
        _make_row_key(card, cy - 1, "then");
        s_math_op2_dd = lv_dropdown_create(card);
        lv_dropdown_set_options(s_math_op2_dd, "+ C\n- C\nx C\n/ C");
        _wiz_style_dropdown(s_math_op2_dd);
        lv_obj_set_size(s_math_op2_dd, 360, 30);
        lv_obj_align(s_math_op2_dd, LV_ALIGN_TOP_RIGHT, 0, cy);
        if (c->math_enabled && c->math_c_enabled)
            lv_dropdown_set_selected(s_math_op2_dd, c->math_op2 & 3);
        cy += 38;
    }
    /* Operand C (none / channel / Number...) + its constant value box. */
    {
        _make_row_key(card, cy - 1, "C");
        s_math_c_dd = lv_dropdown_create(card);
        lv_dropdown_set_options(s_math_c_dd, c_opts);
        _wiz_style_dropdown(s_math_c_dd);
        lv_obj_set_size(s_math_c_dd, 250, 30);
        lv_obj_align(s_math_c_dd, LV_ALIGN_TOP_LEFT, 64, cy);
        if (c->math_enabled && c->math_c_enabled)
            lv_dropdown_set_selected(s_math_c_dd,
                c->math_c_is_const ? 1
                                   : (uint16_t)(_math_id_index(c->math_c) + 2));
        lv_obj_t *cbtn = lv_btn_create(card);
        _wiz_style_textbox(cbtn);
        lv_obj_set_size(cbtn, 96, 30);
        lv_obj_align(cbtn, LV_ALIGN_TOP_RIGHT, 0, cy);
        s_math_cval_lbl = lv_label_create(cbtn);
        char cvb[24]; snprintf(cvb, sizeof(cvb), "%g", s_math_c_const);
        lv_label_set_text(s_math_cval_lbl, cvb);
        lv_obj_center(s_math_cval_lbl);
        lv_obj_set_style_text_font(s_math_cval_lbl, THEME_FONT_SMALL, 0);
        lv_obj_add_event_cb(cbtn, _math_cconst_cb, LV_EVENT_CLICKED, NULL);
        cy += 44;
    }

    lv_obj_t *hint = lv_label_create(card);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, MATH_SHEET_W - 2 * SHEET_PAD);
    lv_label_set_text(hint,
        "Runs left to right: (A op B) then op C. \"Number...\" uses the typed "
        "value. Leave C on \"(none)\" for a two-part sum. Litres/100km is "
        "fuel flow / speed, then x 100.");
    lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, cy);
    lv_obj_set_style_text_font(hint, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(hint, THEME_COLOR_TEXT_MUTED, 0);

    /* Footer buttons: Cancel, (Clear if math), Apply. */
    lv_obj_t *apply = uk_btn(card, UK_ICON_CHECK, "Apply", UK_BTN_PRIMARY,
                             _math_apply_cb, NULL);
    lv_obj_set_size(apply, 116, BTN_H);
    lv_obj_align(apply, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_t *cancel = uk_btn(card, UK_ICON_NONE, "Cancel", UK_BTN_GHOST,
                              _math_close_cb, NULL);
    lv_obj_set_size(cancel, 100, BTN_H);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (c->math_enabled) {
        /* Clear wipes the formula: the kit's danger button. */
        lv_obj_t *clr = uk_btn(card, UK_ICON_NONE, "Clear", UK_BTN_DANGER,
                               _math_clear_cb, NULL);
        lv_obj_set_size(clr, 110, BTN_H);
        lv_obj_align(clr, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
}

static void _calc_btn_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    if (!s_selected_ch_id[0]) return;
    const channel_t *c = channel_manager_get(s_selected_ch_id);
    if (c) _open_math_sheet(c);
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

    /* Kit sheet over the wizard. Owned by s_overlay so it inherits the
     * wizard's lifecycle (closes automatically when the wizard closes). The
     * backdrop is OPAQUE (it was 80 % before) so the channels list behind it
     * doesn't get redrawn every frame for the alpha blend. The card is
     * nearly the glass so the picker has room for the 3-column
     * ECU/Version/Signal browser. */
    lv_obj_t *card = NULL;
    char header[64];
    snprintf(header, sizeof(header), "Pick a source for %s", c->label);
    s_bind_sheet = _wiz_sheet_open(SHEET_W, SHEET_H, header,
                                   _bind_sheet_close_cb, true, &card);

    /* Embedded preset picker — fills the rest of the card under the
     * header. Calls back with the picked preconfig_item_t. */
    const lv_coord_t host_w = SHEET_W - 2 * SHEET_PAD;
    const lv_coord_t host_h = SHEET_H - 2 * SHEET_PAD - 48;
    lv_obj_t *picker_host = lv_obj_create(card);
    lv_obj_remove_style_all(picker_host);
    lv_obj_set_size(picker_host, host_w, host_h);
    lv_obj_align(picker_host, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_clear_flag(picker_host, LV_OBJ_FLAG_SCROLLABLE);
    build_preset_picker_embedded(picker_host, host_w, host_h,
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

    /* The body under the bar is a fixed size; the WiFi step lays out around
     * BTN_W=500 centred in it. */
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

    _wiz_bar_set("WiFi", "Step 4 of 4", false);

    /* Each way to connect is a kit card: caps name, one line on what it
     * does, its button. Card inner width (pad 14) is BTN_W. */
    const lv_coord_t opt_w = BTN_W + 28;

    /* ── Option 1 (Recommended): Join WiFi network ─────────────────── */
    lv_obj_t *opt1 = uk_card(s_step3);
    lv_obj_set_size(opt1, opt_w, 114);
    lv_obj_align(opt1, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *opt1_label = uk_label(opt1, "1.  WiFi  (recommended)",
                                    UK_FONT_LABEL, UK_TONE_TEXT);
    lv_obj_align(opt1_label, LV_ALIGN_TOP_LEFT, 0, -2);

    lv_obj_t *opt1_sub = lv_label_create(opt1);
    lv_label_set_text(opt1_sub,
        "Join your home/shop WiFi - dash will show its IP in Device Settings");
    lv_obj_align(opt1_sub, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_obj_set_style_text_font(opt1_sub, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(opt1_sub, THEME_COLOR_TEXT_MUTED, 0);

    _make_btn(opt1, UK_ICON_WIFI, "Join a WiFi Network",
              UK_BTN_NEUTRAL, 46, _btn_wifi_join_cb);

    /* ── Option 2: Hotspot fallback ─────────────────────────────────── */
    const char *ap_ssid = wifi_manager_get_ap_ssid();
    const char *ap_ip   = wifi_manager_get_ap_ip();

    lv_obj_t *opt2 = uk_card(s_step3);
    lv_obj_set_size(opt2, opt_w, 144);
    lv_obj_align(opt2, LV_ALIGN_TOP_MID, 0, 124);

    lv_obj_t *opt2_label = uk_label(opt2, "2.  Hotspot  (fallback, no WiFi needed)",
                                    UK_FONT_LABEL, UK_TONE_TEXT);
    lv_obj_align(opt2_label, LV_ALIGN_TOP_LEFT, 0, -2);

    lv_obj_t *opt2_sub = lv_label_create(opt2);
    lv_label_set_long_mode(opt2_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(opt2_sub, BTN_W);
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
    lv_obj_align(opt2_sub, LV_ALIGN_TOP_LEFT, 0, 20);
    lv_obj_set_style_text_font(opt2_sub, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(opt2_sub, THEME_COLOR_TEXT_MUTED, 0);

    /* Start Hotspot button — flips runtime to AP mode AND persists
     * hotspot-on-boot, then drops the user into the WiFi screen so they
     * can see the SSID/IP/password live. */
    _make_btn(opt2, UK_ICON_HOTSPOT, "Start Hotspot",
              UK_BTN_NEUTRAL, 76, _btn_hotspot_start_cb);

    /* Finish button — the step's one primary action */
    _make_btn(s_step3, UK_ICON_CHECK, "Finish Setup",
              UK_BTN_PRIMARY, 290, _btn_finish_cb);
}

/* ── Step 1: CAN scan ─────────────────────────────────────────────────── */

static void _build_step1(void) {
    s_step1 = lv_obj_create(s_card);
    lv_obj_remove_style_all(s_step1);
    lv_obj_set_size(s_step1, lv_pct(100), lv_pct(100));
    lv_obj_center(s_step1);
    lv_obj_clear_flag(s_step1, LV_OBJ_FLAG_SCROLLABLE);

    _wiz_bar_set("CAN bus", "Step 1 of 4", false);

    /* The scan's read-out in a kit card (inner width BTN_W); the buttons
     * sit under it in the body. */
    lv_obj_t *scan_card = uk_card(s_step1);
    lv_obj_set_size(scan_card, BTN_W + 28, 204);
    lv_obj_align(scan_card, LV_ALIGN_TOP_MID, 0, 0);

    /* Status label */
    s_scan_status = lv_label_create(scan_card);
    lv_label_set_text(s_scan_status, "Starting scan...");
    lv_obj_align(s_scan_status, LV_ALIGN_TOP_MID, 0, -2);
    lv_obj_set_style_text_font(s_scan_status, THEME_FONT_BODY, 0);
    lv_obj_set_style_text_color(s_scan_status, THEME_COLOR_TEXT_PRIMARY, 0);

    /* Progress bar */
    s_scan_bar = lv_bar_create(scan_card);
    lv_obj_set_size(s_scan_bar, BTN_W, 6);
    lv_obj_align(s_scan_bar, LV_ALIGN_TOP_MID, 0, 24);
    lv_bar_set_range(s_scan_bar, 0, 100);
    lv_bar_set_value(s_scan_bar, 0, LV_ANIM_OFF);
    _wiz_style_bar(s_scan_bar);

    /* Progress text */
    s_scan_progress = lv_label_create(scan_card);
    lv_label_set_text(s_scan_progress, "");
    lv_obj_align(s_scan_progress, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_font(s_scan_progress, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_scan_progress, THEME_COLOR_TEXT_MUTED, 0);

    /* Per-bitrate result lines */
    for (int i = 0; i < 4; i++) {
        s_scan_results[i] = lv_label_create(scan_card);
        lv_label_set_text_fmt(s_scan_results[i], "%s  --  ...", BR_NAMES[i]);
        lv_obj_align(s_scan_results[i], LV_ALIGN_TOP_LEFT, 20, 52 + i * 20);
        lv_obj_set_style_text_font(s_scan_results[i], THEME_FONT_SMALL, 0);
        lv_obj_set_style_text_color(s_scan_results[i], THEME_COLOR_TEXT_MUTED, 0);
    }

    /* Detail label */
    s_scan_detail = lv_label_create(scan_card);
    lv_label_set_text(s_scan_detail, "");
    lv_label_set_long_mode(s_scan_detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_scan_detail, lv_pct(100));
    lv_obj_align(s_scan_detail, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_set_style_text_font(s_scan_detail, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(s_scan_detail, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(s_scan_detail, LV_TEXT_ALIGN_CENTER, 0);

    /* Row 1 (Y=214): forward action — Apply (with traffic) OR Continue
     * without CAN (no traffic). Mutually exclusive, share Y. Apply is the
     * step's primary; continuing without CAN is a neutral fallback. */
    s_btn_apply = _make_btn(s_step1, UK_ICON_CHECK, "Apply & Continue",
                            UK_BTN_PRIMARY, 214, _btn_apply_cb);
    lv_obj_add_flag(s_btn_apply, LV_OBJ_FLAG_HIDDEN);

    s_btn_next1 = _make_btn(s_step1, UK_ICON_RIGHT, "Continue without CAN",
                            UK_BTN_NEUTRAL, 214, _btn_next1_cb);
    lv_obj_add_flag(s_btn_next1, LV_OBJ_FLAG_HIDDEN);

    /* Row 2 (Y=262): scan control — Cancel (while running) OR
     * Start/Re-scan (idle/complete/error). Mutually exclusive, share Y. */
    s_btn_cancel = _make_btn(s_step1, UK_ICON_STOP, "Cancel Scan",
                             UK_BTN_NEUTRAL, 262, _btn_cancel_scan_cb);

    s_btn_start = _make_btn(s_step1, UK_ICON_RESET, "Start Scan",
                            UK_BTN_NEUTRAL, 262, _btn_start_scan_cb);
    lv_obj_add_flag(s_btn_start, LV_OBJ_FLAG_HIDDEN);

    /* Row 3 (Y=310): Skip for now — wizard returns on next boot */
    _make_btn(s_step1, UK_ICON_NONE, "Skip for now  (ask again next boot)",
              UK_BTN_GHOST, 310, _btn_skip_cb);

    /* Row 4 (Y=358): Skip for good — never show again.
     * The same setup screens stay reachable from Device Settings, so
     * this is non-destructive — just dismisses the boot prompt forever. */
    _make_btn(s_step1, UK_ICON_NONE, "Skip for good  (don't show again)",
              UK_BTN_GHOST, 358, _btn_skip_forever_cb);
}

/* ── Public entry point ───────────────────────────────────────────────── */

/* The kit page every entry point builds on: an opaque overlay on the
 * palette background over the active screen (nothing behind it redraws),
 * the brand bar across the top, and s_card as the body under it. Each step
 * repaints the bar's title / step / Close via _wiz_bar_set. */
static void _wiz_build_shell(void) {
    lv_obj_t *scr = lv_scr_act();
    s_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_overlay, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(s_overlay, THEME_FONT_BODY, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_overlay);
    /* Kill the channels refresh timer if the overlay is deleted without going
     * through _close_wizard (see _channels_overlay_delete_cb). */
    lv_obj_add_event_cb(s_overlay, _channels_overlay_delete_cb,
                        LV_EVENT_DELETE, NULL);

    /* Brand bar. Created with Close so the button exists; each step shows or
     * hides it. Children: logo 0, title 1, status strip 2, Close 3. */
    s_bar = uk_bar(s_overlay, "Setup", UK_BAR_CLOSE, _bar_close_cb, NULL);
    s_bar_title  = lv_obj_get_child(s_bar, 1);
    s_bar_close  = lv_obj_get_child(s_bar, 3);
    s_bar_status = uk_bar_status(s_bar, false, UK_TONE_MUTED, "");
    if (s_bar_close) lv_obj_add_flag(s_bar_close, LV_OBJ_FLAG_HIDDEN);

    /* Body: transparent, the full width under the bar inset by the kit's
     * padding. Steps position themselves absolutely inside it. */
    s_card = lv_obj_create(s_overlay);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, WIZ_BODY_W, WIZ_BODY_H);
    lv_obj_align(s_card, LV_ALIGN_TOP_LEFT, UK_PAD, UK_BAR_H + UK_PAD);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
}

void show_first_run_wizard_rerun(void) {
    if (s_overlay && lv_obj_is_valid(s_overlay)) return;
    show_first_run_wizard();
    s_rerun = true;
}

void show_first_run_wizard(void) {
    if (s_overlay && lv_obj_is_valid(s_overlay)) return;
    s_rerun = false;
    s_standalone_channels = false;  /* full onboarding flow, not channels-only */

    _wiz_build_shell();

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
    _wiz_build_shell();
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

bool first_run_wizard_is_open(void) {
    return s_overlay && lv_obj_is_valid(s_overlay);
}

void first_run_wizard_open_obd2_scan(void) {
    if (s_overlay && lv_obj_is_valid(s_overlay)) return;
    first_run_wizard_open_channels();
    if (s_overlay && lv_obj_is_valid(s_overlay)) _obd2_open_scan_modal();
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
