#include "ota_update_dialog.h"
#include "system/safe_restart.h"
#include "theme.h"
#include "kit/ui_kit.h"
#include "storage/config_store.h"
#include "net/wifi_manager.h"
#include "version.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ota_dialog";

/* Static variables for the dialog components.
 *
 * Every dialog here is a kit popup (uk_popup) on lv_layer_top. ota_modal and
 * ota_dialog both point at the popup's card: deleting the card also deletes
 * the dimmed backdrop uk_popup made beside it, so close_ota_update_dialog
 * deletes ota_modal once and ota_dialog is never deleted on its own. */
static lv_obj_t *ota_modal = NULL;
static lv_obj_t *ota_dialog = NULL;
static lv_obj_t *progress_bar = NULL;
static lv_obj_t *progress_label = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *install_btn = NULL;
static lv_obj_t *cancel_btn = NULL;
static lv_obj_t *skip_btn = NULL;
static lv_timer_t *progress_timer = NULL;
static lv_timer_t *checking_timeout_timer = NULL;
static bool update_in_progress = false;
/* Version offered by the current dialog. Stashed by show_ota_update_dialog
 * so the "Skip This Version" handler can persist it to NVS without having
 * to re-derive it from the live OTA state (which may have raced by then). */
static char s_offered_version[32] = {0};

// Forward declarations
static void install_btn_event_cb(lv_event_t *e);
static void reboot_and_update_btn_cb(lv_event_t *e);
static void cancel_btn_event_cb(lv_event_t *e);
static void skip_btn_event_cb(lv_event_t *e);
static void progress_timer_cb(lv_timer_t *timer);
void show_ota_check_failed_dialog(void);

// Install button event handler
static void install_btn_event_cb(lv_event_t *e) {
    (void)e;
    ota_update_dialog_begin_install();
}

/* "Reboot and update" — offered only after an install died at task creation.
 *
 * The 6 KB download task needs one CONTIGUOUS internal-RAM block, and the
 * internal heap fragments as the dash runs; the same firmware that fails now
 * usually installs fine on a fresh boot. Writing the version down and
 * restarting turns "reboot and try again", which asks the user to remember
 * to come back, into one tap: the boot OTA check finds the request and
 * starts the install itself, at the point in the dash's life when internal
 * RAM is least fragmented. */
static void reboot_and_update_btn_cb(lv_event_t *e) {
    (void)e;
    if (s_offered_version[0] != '\0') {
        esp_err_t err = config_store_save_ota_resume_version(s_offered_version);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not record the resume request (%s) — not "
                          "rebooting, that would just lose the dialog",
                     esp_err_to_name(err));
            if (status_label && lv_obj_is_valid(status_label))
                lv_label_set_text(status_label,
                    "Could not save the update request.\n"
                    "Update from RDM Studio instead.");
            return;
        }
    }
    ESP_LOGI(TAG, "Rebooting to install %s with a fresh heap", s_offered_version);
    rdm_safe_restart();
}

/* Run the install. Split out of the button handler so the boot-time resume
 * path can do exactly what pressing Install does, rather than a near-copy
 * that drifts. */
void ota_update_dialog_begin_install(void) {
    if (update_in_progress) return;

    ESP_LOGI(TAG, "Starting OTA update...");
    update_in_progress = true;
    
    // Hide install + cancel + skip during the update so the user can't
    // trigger another flow mid-flash.
    if (install_btn && lv_obj_is_valid(install_btn))
        lv_obj_add_flag(install_btn, LV_OBJ_FLAG_HIDDEN);
    if (cancel_btn && lv_obj_is_valid(cancel_btn))
        lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_HIDDEN);
    if (skip_btn && lv_obj_is_valid(skip_btn))
        lv_obj_add_flag(skip_btn, LV_OBJ_FLAG_HIDDEN);
    
    // Update status and show progress bar
    if (status_label && lv_obj_is_valid(status_label)) {
        lv_label_set_text(status_label, "Starting the update...");
        lv_obj_set_style_text_color(status_label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    
    if (progress_bar && lv_obj_is_valid(progress_bar)) {
        lv_obj_clear_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    }
    
    if (progress_label && lv_obj_is_valid(progress_label)) {
        lv_obj_clear_flag(progress_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(progress_label, "0%");
    }
    
    // Start progress monitoring timer
    if (progress_timer) {
        lv_timer_del(progress_timer);
    }
    progress_timer = lv_timer_create(progress_timer_cb, 500, NULL); // Update every 500ms
    
    /* Start the OTA update task. A spawn failure means the install never
     * began (no contiguous internal RAM for the 6 KB stack) — say so on the
     * dash instead of leaving a progress bar parked at 0% forever. */
    if (start_ota_update_task() != ESP_OK) {
        if (progress_timer) {
            lv_timer_del(progress_timer);
            progress_timer = NULL;
        }
        if (progress_bar && lv_obj_is_valid(progress_bar)) {
            lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (progress_label && lv_obj_is_valid(progress_label)) {
            lv_obj_add_flag(progress_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (status_label && lv_obj_is_valid(status_label)) {
            /* Name the cause. "Not enough memory" reads like the dash is out
             * of storage and the update is too big for it, which is what it
             * was taken to mean in the field; the flash slot is fine, it is
             * RAM, and only because it has fragmented while running. */
            lv_label_set_text(status_label,
                "The dash has been running too long to start the download.\n"
                "Reboot and update restarts and installs straight away.");
            lv_obj_set_style_text_color(status_label, THEME_COLOR_STATUS_ERROR,
                                        LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        /* Restore the dismiss/retry buttons hidden above — nothing is
         * flashing, so leaving them hidden would strand the dialog. The
         * install button becomes the reboot-and-install action: pressing
         * Install again from here would fail for exactly the same reason. */
        if (install_btn && lv_obj_is_valid(install_btn)) {
            lv_obj_clear_flag(install_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_event_cb(install_btn, install_btn_event_cb);
            lv_obj_add_event_cb(install_btn, reboot_and_update_btn_cb,
                                LV_EVENT_CLICKED, NULL);
            /* A kit button: child 0 is the icon, so go through the kit. */
            uk_btn_set_text(install_btn, "Reboot and update");
        }
        if (cancel_btn && lv_obj_is_valid(cancel_btn))
            lv_obj_clear_flag(cancel_btn, LV_OBJ_FLAG_HIDDEN);
        if (skip_btn && lv_obj_is_valid(skip_btn))
            lv_obj_clear_flag(skip_btn, LV_OBJ_FLAG_HIDDEN);
        update_in_progress = false;
    }
}

// "Later" button event handler — closes the dialog. The auto-OTA-check
// is gated per-boot, so this version will pop again on the next reboot.
static void cancel_btn_event_cb(lv_event_t *e) {
    if (update_in_progress) {
        // Cannot cancel during update - could brick device
        ESP_LOGW(TAG, "Cannot cancel update in progress");
        return;
    }

    ESP_LOGI(TAG, "OTA update postponed by user (will reappear next boot)");
    close_ota_update_dialog();
}

// "Skip This Version" button event handler — writes the offered version
// string to NVS so the auto-OTA-check stays silent until a *newer*
// release appears upstream.
static void skip_btn_event_cb(lv_event_t *e) {
    if (update_in_progress) return;

    if (s_offered_version[0] != '\0') {
        ESP_LOGI(TAG, "OTA: user skipped version %s", s_offered_version);
        esp_err_t err = config_store_save_ota_skip_version(s_offered_version);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "Failed to persist skip-version: %s", esp_err_to_name(err));
    }
    close_ota_update_dialog();
}

// Progress timer callback
static void progress_timer_cb(lv_timer_t *timer) {
    if (!ota_modal || !lv_obj_is_valid(ota_modal)) {
        if (progress_timer) {
            lv_timer_del(progress_timer);
            progress_timer = NULL;
        }
        return;
    }
    
    ota_status_t status = get_ota_status();
    int progress = get_ota_progress();
    
    // Update progress bar
    if (progress_bar && lv_obj_is_valid(progress_bar)) {
        if (progress >= 0) {
            lv_bar_set_value(progress_bar, progress, LV_ANIM_ON);
        }
    }
    
    // Update progress label
    if (progress_label && lv_obj_is_valid(progress_label)) {
        if (progress >= 0) {
            lv_label_set_text_fmt(progress_label, "%d%%", progress);
        }
    }
    
    // Update status based on OTA status
    if (status_label && lv_obj_is_valid(status_label)) {
        switch (status) {
            case OTA_UPDATE_IN_PROGRESS:
                if (progress > 0) {
                    lv_label_set_text_fmt(status_label, "Downloading firmware... %d%%", progress);
                } else {
                    lv_label_set_text(status_label, "Preparing download...");
                }
                lv_obj_set_style_text_color(status_label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
                break;

            case OTA_UPDATE_COMPLETED:
                lv_label_set_text(status_label, "Update installed. Restarting...");
                lv_obj_set_style_text_color(status_label, THEME_COLOR_STATUS_CONNECTED, LV_PART_MAIN | LV_STATE_DEFAULT);
                // Will reboot automatically, dialog will be destroyed
                break;

            case OTA_UPDATE_FAILED:
                lv_label_set_text(status_label,
                    "The update did not finish. Nothing was changed: the dash "
                    "still runs the version it had. Try again, or close this.");
                lv_obj_set_style_text_color(status_label, THEME_COLOR_STATUS_ERROR, LV_PART_MAIN | LV_STATE_DEFAULT);
                update_in_progress = false;

                /* Every button was hidden for the flash. Bring back a way out
                 * (Close) and a retry, or the only exit is the small X. */
                if (progress_bar && lv_obj_is_valid(progress_bar))
                    lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);
                if (progress_label && lv_obj_is_valid(progress_label))
                    lv_obj_add_flag(progress_label, LV_OBJ_FLAG_HIDDEN);
                if (install_btn && lv_obj_is_valid(install_btn)) {
                    uk_btn_set_text(install_btn, "Try again");
                    lv_obj_clear_flag(install_btn, LV_OBJ_FLAG_HIDDEN);
                }
                if (cancel_btn && lv_obj_is_valid(cancel_btn)) {
                    uk_btn_set_text(cancel_btn, "Close");
                    lv_obj_clear_flag(cancel_btn, LV_OBJ_FLAG_HIDDEN);
                }
                if (skip_btn && lv_obj_is_valid(skip_btn))
                    lv_obj_clear_flag(skip_btn, LV_OBJ_FLAG_HIDDEN);
                break;
                
            default:
                break;
        }
    }
    
    // Stop timer if update completed or failed
    if (status == OTA_UPDATE_COMPLETED || status == OTA_UPDATE_FAILED) {
        if (progress_timer) {
            lv_timer_del(progress_timer);
            progress_timer = NULL;
        }
    }
}

// Show the OTA update dialog
void show_ota_update_dialog(const char* current_version, const char* new_version, float file_size_mb, const char* release_notes) {
    // Close existing dialog if any
    close_ota_update_dialog();

    /* Remember which version we're offering — the "Skip This Version"
     * handler reads this back when the user dismisses, since by then
     * the live get_latest_version() may have raced or returned NULL. */
    if (new_version) {
        strncpy(s_offered_version, new_version, sizeof(s_offered_version) - 1);
        s_offered_version[sizeof(s_offered_version) - 1] = '\0';
    } else {
        s_offered_version[0] = '\0';
    }

    ESP_LOGI(TAG, "Showing OTA update dialog for version %s", new_version);
    
    /* Kit popup, 520 x 400 (484 x 364 inside its padding). Its X is "Later",
     * which refuses to close while an install is flashing. The popup lives on
     * lv_layer_top (it used to be a child of the active screen), so it stays
     * up if the dashboard screen underneath is rebuilt. */
    const lv_coord_t DLG_W = 520, DLG_H = 400;
    const lv_coord_t IN_W = DLG_W - 36, IN_H = DLG_H - 36;
    ota_modal = uk_popup(DLG_W, DLG_H, "Update available", cancel_btn_event_cb);
    ota_dialog = ota_modal;

    // Version and size, as key / value rows
    lv_obj_t *rows = lv_obj_create(ota_dialog);
    lv_obj_remove_style_all(rows);
    lv_obj_set_size(rows, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(rows, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(rows, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    uk_row(rows, "On this dash", current_version ? current_version : "?");
    uk_row(rows, "New version", new_version ? new_version : "?");

    // File size. Integer math only: LVGL's sprintf is built with
    // LV_SPRINTF_USE_FLOAT=0, so "%.1f" renders as the literal text "f" —
    // customers saw "Size: f MB" on the update dialog.
    int size_mb_tenths = (int)(file_size_mb * 10.0f + 0.5f);
    char size_buf[24];
    snprintf(size_buf, sizeof(size_buf), "%d.%d MB",
             size_mb_tenths / 10, size_mb_tenths % 10);
    uk_row(rows, "Download size", size_buf);

    // Release notes (if provided) — clipped to three lines so a long
    // changelog can't push the buttons off the card.
    if (release_notes && strlen(release_notes) > 0) {
        lv_obj_t *notes_label = uk_label(ota_dialog, "", UK_FONT_SMALL, UK_TONE_MUTED);
        lv_label_set_text_fmt(notes_label, "Notes: %s", release_notes);
        lv_label_set_long_mode(notes_label, LV_LABEL_LONG_DOT);
        lv_obj_set_size(notes_label, IN_W, 46);
        lv_obj_align(notes_label, LV_ALIGN_TOP_LEFT, 0, 180);
    }

    // Progress bar (initially hidden) — the kit slider's thin track look
    progress_bar = lv_bar_create(ota_dialog);
    lv_obj_set_size(progress_bar, IN_W - 56, 8);
    lv_obj_align(progress_bar, LV_ALIGN_TOP_LEFT, 0, 243);
    lv_obj_set_style_bg_color(progress_bar, THEME_COLOR_INPUT_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(progress_bar, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(progress_bar, THEME_COLOR_ACCENT, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(progress_bar, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(progress_bar, LV_RADIUS_CIRCLE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);

    // Progress label (initially hidden)
    progress_label = uk_label(ota_dialog, "0%", UK_FONT_BODY, UK_TONE_TEXT);
    lv_obj_align(progress_label, LV_ALIGN_TOP_RIGHT, 0, 238);
    lv_obj_add_flag(progress_label, LV_OBJ_FLAG_HIDDEN);

    // Status label (two lines when the install cannot start)
    status_label = uk_label(ota_dialog, "Ready to install.", UK_FONT_SMALL, UK_TONE_TEXT);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(status_label, IN_W);
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 0, 264);

    /* Three buttons across the footer: Later (postpone, dialog reappears
     * next boot), Skip this version (writes to NVS, silent until a newer
     * release lands), Install (the primary action, at the right; wide
     * enough for its "Reboot and update" relabel). */
    const lv_coord_t BTN_Y = IN_H - UK_BTN_H;

    // "Later" button (postpone until next boot)
    cancel_btn = uk_btn(ota_dialog, UK_ICON_NONE, "Later", UK_BTN_GHOST,
                        cancel_btn_event_cb, NULL);
    lv_obj_set_width(cancel_btn, 100);
    lv_obj_align(cancel_btn, LV_ALIGN_TOP_LEFT, 0, BTN_Y);

    // "Skip this version" button (persisted dismiss until a newer release)
    skip_btn = uk_btn(ota_dialog, UK_ICON_NONE, "Skip this version", UK_BTN_NEUTRAL,
                      skip_btn_event_cb, NULL);
    lv_obj_set_width(skip_btn, 164);
    lv_obj_align(skip_btn, LV_ALIGN_TOP_LEFT, 112, BTN_Y);

    // Install button (primary)
    install_btn = uk_btn(ota_dialog, UK_ICON_UPDATE, "Install", UK_BTN_PRIMARY,
                         install_btn_event_cb, NULL);
    lv_obj_set_width(install_btn, 196);
    lv_obj_align(install_btn, LV_ALIGN_TOP_RIGHT, 0, BTN_Y);
    
    // Reset state
    update_in_progress = false;
}

// Close the OTA update dialog
void close_ota_update_dialog(void) {
    // Stop progress timer
    if (progress_timer) {
        lv_timer_del(progress_timer);
        progress_timer = NULL;
    }

    // Stop checking timeout timer
    if (checking_timeout_timer) {
        lv_timer_del(checking_timeout_timer);
        checking_timeout_timer = NULL;
    }

    // Delete modal and reset pointers
    if (ota_modal && lv_obj_is_valid(ota_modal)) {
        lv_obj_del(ota_modal);
    }

    ota_modal = NULL;
    ota_dialog = NULL;
    progress_bar = NULL;
    progress_label = NULL;
    status_label = NULL;
    install_btn = NULL;
    cancel_btn = NULL;
    skip_btn = NULL;
    update_in_progress = false;

    ESP_LOGI(TAG, "OTA update dialog closed");
}

// Update progress (can be called externally for manual updates)
void update_ota_progress_dialog(void) {
    if (progress_timer) {
        progress_timer_cb(progress_timer);
    }
}

/* ── Simple info dialog (shared by "up to date" and "check failed") ────── */

static void _info_ok_btn_cb(lv_event_t *e) {
    (void)e;
    close_ota_update_dialog();
}

static void _create_info_dialog(const char *title_text, const char *body_text,
                                lv_color_t accent_color) {
    close_ota_update_dialog();

    /* Kit popup, 400 x 210 (364 x 174 inside); its X and OK both close.
     * @p accent_color now tints only the body text (ok / error), since the
     * kit card edge is always the same hairline. */
    ota_modal = uk_popup(400, 210, title_text, _info_ok_btn_cb);
    ota_dialog = ota_modal;

    /* Body */
    lv_obj_t *body = uk_label(ota_dialog, body_text, UK_FONT_BODY, UK_TONE_TEXT);
    lv_obj_set_style_text_color(body, accent_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 364);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);

    /* OK button */
    lv_obj_t *ok_btn = uk_btn(ota_dialog, UK_ICON_NONE, "OK", UK_BTN_PRIMARY,
                              _info_ok_btn_cb, NULL);
    lv_obj_set_width(ok_btn, 120);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

/* ── Checking dialog with spinner ──────────────────────────────────────── */

static uint32_t s_check_token;   /* defined with the manual check below */

static void _checking_cancel_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI(TAG, "OTA check cancelled by user");
    s_check_token++;              /* a late answer must not pop a dialog */
    close_ota_update_dialog();
}

static void _checking_timeout_cb(lv_timer_t *timer) {
    (void)timer;
    checking_timeout_timer = NULL;
    ESP_LOGW(TAG, "OTA check timed out");
    s_check_token++;
    show_ota_check_failed_dialog();
}

void show_ota_checking_dialog(void) {
    close_ota_update_dialog();

    /* Kit popup, 360 x 250 (324 x 214 inside). X and Cancel both stop
     * waiting. */
    ota_modal = uk_popup(360, 250, "Checking for updates", _checking_cancel_cb);
    ota_dialog = ota_modal;

    /* No kit spinner: theme tokens directly, thin like the kit's tracks. */
    lv_obj_t *spinner = lv_spinner_create(ota_dialog, 1000, 60);
    lv_obj_set_size(spinner, 40, 40);
    lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, UK_POPUP_BODY_Y + 4);
    lv_obj_set_style_arc_color(spinner, THEME_COLOR_CONTROL_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, THEME_COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);

    lv_obj_t *msg = uk_label(ota_dialog, "Contacting the update server...",
                             UK_FONT_SMALL, UK_TONE_MUTED);
    lv_obj_align(msg, LV_ALIGN_TOP_MID, 0, UK_POPUP_BODY_Y + 56);

    /* Cancel button */
    cancel_btn = uk_btn(ota_dialog, UK_ICON_NONE, "Cancel", UK_BTN_GHOST,
                        _checking_cancel_cb, NULL);
    lv_obj_set_width(cancel_btn, 120);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* 30-second timeout: auto-close and show failure if check hangs */
    if (checking_timeout_timer) {
        lv_timer_del(checking_timeout_timer);
    }
    checking_timeout_timer = lv_timer_create(_checking_timeout_cb, 30000, NULL);
    lv_timer_set_repeat_count(checking_timeout_timer, 1);

    update_in_progress = false;
    ESP_LOGI(TAG, "Showing OTA checking dialog");
}

/* ── Up to date dialog ─────────────────────────────────────────────────── */

void show_ota_up_to_date_dialog(const char *current_version) {
    char body[80];
    snprintf(body, sizeof(body), "You're running the latest firmware\nv%s",
             current_version ? current_version : "?");
    _create_info_dialog("Up to date", body, THEME_COLOR_STATUS_CONNECTED);
    ESP_LOGI(TAG, "Showing up-to-date dialog (v%s)", current_version ? current_version : "?");
}

/* ── Check failed dialog ───────────────────────────────────────────────── */

static void _retry_check_cb(lv_event_t *e) {
    (void)e;
    ota_update_dialog_check_now();
}

/* A problem dialog with the two ways forward side by side: Close, and a
 * retry that runs the whole check again. */
static void _create_retry_dialog(const char *title_text, const char *body_text) {
    close_ota_update_dialog();
    ota_modal = uk_popup(440, 250, title_text, _info_ok_btn_cb);
    ota_dialog = ota_modal;

    lv_obj_t *body = uk_label(ota_dialog, body_text, UK_FONT_BODY, UK_TONE_MUTED);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 404);
    lv_obj_set_style_text_line_space(body, 4, 0);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);

    cancel_btn = uk_btn(ota_dialog, UK_ICON_NONE, "Close", UK_BTN_GHOST,
                        _info_ok_btn_cb, NULL);
    lv_obj_set_width(cancel_btn, 120);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    lv_obj_t *retry = uk_btn(ota_dialog, UK_ICON_RESET, "Try again", UK_BTN_PRIMARY,
                             _retry_check_cb, NULL);
    lv_obj_set_width(retry, 160);
    lv_obj_align(retry, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

void show_ota_check_failed_dialog(void) {
    _create_retry_dialog("Update check failed",
        "The dash could not reach the update server. It needs a WiFi network "
        "with internet, not just its own hotspot.");
    ESP_LOGI(TAG, "Showing OTA check-failed dialog");
}

/* ── Manual check ──────────────────────────────────────────────────────── */

/* Bumped by every new check and by Cancel. The check task carries the value
 * it started with; a result for an older one (the person cancelled, or started
 * again) is dropped instead of popping a dialog they walked away from. */
static uint32_t s_check_token = 0;
static bool     s_check_running = false;

static void _check_result_async(void *arg) {
    uint32_t token = (uint32_t)(uintptr_t)arg;
    s_check_running = false;
    if (token != s_check_token) {
        ESP_LOGI(TAG, "Update check finished after it was cancelled; ignoring");
        return;
    }
    ota_status_t status = get_ota_status();
    if (status == OTA_UPDATE_AVAILABLE) {
        show_ota_update_dialog(FIRMWARE_VERSION, get_latest_version(),
                               get_update_file_size_mb(), get_release_notes());
    } else if (status == OTA_NO_UPDATE_AVAILABLE) {
        show_ota_up_to_date_dialog(FIRMWARE_VERSION);
    } else {
        show_ota_check_failed_dialog();
    }
}

static void _check_task(void *arg) {
    check_for_update();
    lv_async_call(_check_result_async, arg);
    vTaskDelete(NULL);
}

void ota_update_dialog_check_now(void) {
    const char *ssid = wifi_manager_get_connected_ssid();
    if (!ssid || !ssid[0]) {
        _create_retry_dialog("No internet",
            "Updates download over the internet. Join a WiFi network that has "
            "internet in Connect > WiFi, then check again.");
        return;
    }
    if (s_check_running) {           /* one already asking: just show it */
        show_ota_checking_dialog();
        return;
    }

    show_ota_checking_dialog();
    uint32_t token = ++s_check_token;

    /* TCB in internal RAM (FreeRTOS requires it), the 8 KB stack in PSRAM:
     * a contiguous 8 KB internal block is exactly what a long-running dash
     * runs out of (see reboot_and_update_btn_cb). */
    static StaticTask_t  s_tcb;
    static StackType_t  *s_stack = NULL;
    const uint32_t STACK = 8192;
    if (!s_stack)
        s_stack = heap_caps_calloc(STACK, sizeof(StackType_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_stack ||
        !xTaskCreateStaticPinnedToCore(_check_task, "ota_chk", STACK,
                                       (void *)(uintptr_t)token, 3, s_stack, &s_tcb, 0)) {
        ESP_LOGE(TAG, "Could not start the update check task");
        show_ota_check_failed_dialog();
        return;
    }
    s_check_running = true;
}