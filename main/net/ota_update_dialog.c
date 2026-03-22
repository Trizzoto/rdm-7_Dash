#include "ota_update_dialog.h"
#include "theme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ota_dialog";

// Static variables for the dialog components
static lv_obj_t *ota_modal = NULL;
static lv_obj_t *ota_dialog = NULL;
static lv_obj_t *progress_bar = NULL;
static lv_obj_t *progress_label = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *install_btn = NULL;
static lv_obj_t *cancel_btn = NULL;
static lv_timer_t *progress_timer = NULL;
static lv_timer_t *checking_timeout_timer = NULL;
static bool update_in_progress = false;

// Forward declarations
static void install_btn_event_cb(lv_event_t *e);
static void cancel_btn_event_cb(lv_event_t *e);
static void progress_timer_cb(lv_timer_t *timer);
void show_ota_check_failed_dialog(void);

// Install button event handler
static void install_btn_event_cb(lv_event_t *e) {
    if (update_in_progress) return;
    
    ESP_LOGI(TAG, "Starting OTA update...");
    update_in_progress = true;
    
    // Hide install button and show progress
    if (install_btn && lv_obj_is_valid(install_btn)) {
        lv_obj_add_flag(install_btn, LV_OBJ_FLAG_HIDDEN);
    }
    
    // Update status and show progress bar
    if (status_label && lv_obj_is_valid(status_label)) {
        lv_label_set_text(status_label, "Initializing update...");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0x4080FF), LV_PART_MAIN | LV_STATE_DEFAULT);
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
    
    // Start the OTA update task
    start_ota_update_task();
}

// Cancel button event handler
static void cancel_btn_event_cb(lv_event_t *e) {
    if (update_in_progress) {
        // Cannot cancel during update - could brick device
        ESP_LOGW(TAG, "Cannot cancel update in progress");
        return;
    }
    
    ESP_LOGI(TAG, "OTA update cancelled by user");
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
                lv_obj_set_style_text_color(status_label, lv_color_hex(0x4080FF), LV_PART_MAIN | LV_STATE_DEFAULT);
                break;
                
            case OTA_UPDATE_COMPLETED:
                lv_label_set_text(status_label, "Update successful! Rebooting...");
                lv_obj_set_style_text_color(status_label, lv_color_hex(0x00FF80), LV_PART_MAIN | LV_STATE_DEFAULT);
                // Will reboot automatically, dialog will be destroyed
                break;
                
            case OTA_UPDATE_FAILED:
                lv_label_set_text(status_label, "Update failed! Please try again.");
                lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF4444), LV_PART_MAIN | LV_STATE_DEFAULT);
                update_in_progress = false;
                
                // Show install button again for retry
                if (install_btn && lv_obj_is_valid(install_btn)) {
                    lv_obj_clear_flag(install_btn, LV_OBJ_FLAG_HIDDEN);
                }
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
    
    ESP_LOGI(TAG, "Showing OTA update dialog for version %s", new_version);
    
    // Create modal background
    ota_modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ota_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(ota_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ota_modal, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ota_modal, 180, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ota_modal, LV_OBJ_FLAG_SCROLLABLE);
    
    // Create main dialog
    ota_dialog = lv_obj_create(ota_modal);
    lv_obj_set_size(ota_dialog, 500, 400);
    lv_obj_align(ota_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ota_dialog, THEME_COLOR_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ota_dialog, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ota_dialog, THEME_RADIUS_LARGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ota_dialog, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ota_dialog, THEME_BORDER_W_NORMAL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ota_dialog, 25, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ota_dialog, LV_OBJ_FLAG_SCROLLABLE);
    
    // Title
    lv_obj_t *title = lv_label_create(ota_dialog);
    lv_label_set_text(title, "Firmware Update Available");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Version info
    lv_obj_t *version_label = lv_label_create(ota_dialog);
    lv_label_set_text_fmt(version_label, "Current: %s -> New: %s", current_version, new_version);
    lv_obj_align(version_label, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_text_font(version_label, THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(version_label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // File size
    lv_obj_t *size_label = lv_label_create(ota_dialog);
    lv_label_set_text_fmt(size_label, "Size: %.1f MB", file_size_mb);
    lv_obj_align(size_label, LV_ALIGN_TOP_LEFT, 0, 65);
    lv_obj_set_style_text_font(size_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(size_label, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Release notes (if provided)
    if (release_notes && strlen(release_notes) > 0) {
        lv_obj_t *notes_label = lv_label_create(ota_dialog);
        lv_label_set_text_fmt(notes_label, "Notes: %s", release_notes);
        lv_obj_align(notes_label, LV_ALIGN_TOP_LEFT, 0, 90);
        lv_obj_set_style_text_font(notes_label, THEME_FONT_TINY, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(notes_label, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_long_mode(notes_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(notes_label, 450);
    }
    
    // Progress bar (initially hidden)
    progress_bar = lv_bar_create(ota_dialog);
    lv_obj_set_size(progress_bar, 450, 20);
    lv_obj_align(progress_bar, LV_ALIGN_TOP_LEFT, 0, 180);
    lv_obj_set_style_bg_color(progress_bar, THEME_COLOR_SECTION_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(progress_bar, THEME_COLOR_ACCENT_BLUE, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(progress_bar, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(progress_bar, 10, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);
    
    // Progress label (initially hidden)
    progress_label = lv_label_create(ota_dialog);
    lv_label_set_text(progress_label, "0%");
    lv_obj_align(progress_label, LV_ALIGN_TOP_RIGHT, 0, 185);
    lv_obj_set_style_text_font(progress_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(progress_label, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(progress_label, LV_OBJ_FLAG_HIDDEN);
    
    // Status label
    status_label = lv_label_create(ota_dialog);
    lv_label_set_text(status_label, "Ready to install firmware update");
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 0, 220);
    lv_obj_set_style_text_font(status_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(status_label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Button container
    lv_obj_t *btn_container = lv_obj_create(ota_dialog);
    lv_obj_set_size(btn_container, 450, 50);
    lv_obj_align(btn_container, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btn_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // Cancel button
    cancel_btn = lv_btn_create(btn_container);
    lv_obj_set_size(cancel_btn, 200, 40);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_GRAY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_GRAY_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(cancel_btn, THEME_RADIUS_NORMAL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cancel_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_set_style_text_color(cancel_label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(cancel_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Install button
    install_btn = lv_btn_create(btn_container);
    lv_obj_set_size(install_btn, 200, 40);
    lv_obj_set_style_bg_color(install_btn, THEME_COLOR_BTN_SAVE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(install_btn, THEME_COLOR_BTN_SAVE_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(install_btn, THEME_RADIUS_NORMAL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(install_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *install_label = lv_label_create(install_btn);
    lv_label_set_text(install_label, "Install Update");
    lv_obj_center(install_label);
    lv_obj_set_style_text_color(install_label, THEME_COLOR_TEXT_ON_ACCENT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(install_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(install_btn, install_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
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

    /* Modal background */
    ota_modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ota_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(ota_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ota_modal, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ota_modal, 180, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ota_modal, LV_OBJ_FLAG_SCROLLABLE);

    /* Dialog card */
    ota_dialog = lv_obj_create(ota_modal);
    lv_obj_set_size(ota_dialog, 400, 200);
    lv_obj_align(ota_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ota_dialog, THEME_COLOR_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ota_dialog, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ota_dialog, THEME_RADIUS_LARGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ota_dialog, accent_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ota_dialog, THEME_BORDER_W_NORMAL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ota_dialog, 25, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ota_dialog, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(ota_dialog);
    lv_label_set_text(title, title_text);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Body */
    lv_obj_t *body = lv_label_create(ota_dialog);
    lv_label_set_text(body, body_text);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, -5);
    lv_obj_set_style_text_font(body, THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(body, accent_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 350);

    /* OK button */
    lv_obj_t *ok_btn = lv_btn_create(ota_dialog);
    lv_obj_set_size(ok_btn, 120, 40);
    lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(ok_btn, accent_color, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ok_btn, THEME_RADIUS_NORMAL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ok_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ok_btn, _info_ok_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, "OK");
    lv_obj_center(ok_label);
    lv_obj_set_style_text_color(ok_label, THEME_COLOR_TEXT_ON_ACCENT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ok_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
}

/* ── Checking dialog with spinner ──────────────────────────────────────── */

static void _checking_cancel_cb(lv_event_t *e) {
    (void)e;
    ESP_LOGI(TAG, "OTA check cancelled by user");
    close_ota_update_dialog();
}

static void _checking_timeout_cb(lv_timer_t *timer) {
    (void)timer;
    checking_timeout_timer = NULL;
    ESP_LOGW(TAG, "OTA check timed out");
    show_ota_check_failed_dialog();
}

void show_ota_checking_dialog(void) {
    close_ota_update_dialog();

    ota_modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ota_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(ota_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ota_modal, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ota_modal, 180, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ota_modal, LV_OBJ_FLAG_SCROLLABLE);

    ota_dialog = lv_obj_create(ota_modal);
    lv_obj_set_size(ota_dialog, 350, 220);
    lv_obj_align(ota_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ota_dialog, THEME_COLOR_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ota_dialog, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ota_dialog, THEME_RADIUS_LARGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ota_dialog, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ota_dialog, THEME_BORDER_W_NORMAL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ota_dialog, 25, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ota_dialog, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(ota_dialog);
    lv_label_set_text(title, "Checking for Updates");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *spinner = lv_spinner_create(ota_dialog, 1000, 60);
    lv_obj_set_size(spinner, 44, 44);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_arc_color(spinner, THEME_COLOR_SECTION_BG, LV_PART_MAIN);
    lv_obj_set_style_arc_color(spinner, THEME_COLOR_ACCENT_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(spinner, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(spinner, 5, LV_PART_INDICATOR);

    lv_obj_t *msg = lv_label_create(ota_dialog);
    lv_label_set_text(msg, "Contacting update server...");
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_text_font(msg, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(msg, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);

    /* Cancel button */
    cancel_btn = lv_btn_create(ota_dialog);
    lv_obj_set_size(cancel_btn, 120, 36);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_GRAY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_GRAY_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(cancel_btn, THEME_RADIUS_NORMAL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cancel_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(cancel_btn, _checking_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_set_style_text_color(cancel_label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(cancel_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);

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
    _create_info_dialog("Up to Date", body, lv_color_hex(0x00FF80));
    ESP_LOGI(TAG, "Showing up-to-date dialog (v%s)", current_version ? current_version : "?");
}

/* ── Check failed dialog ───────────────────────────────────────────────── */

void show_ota_check_failed_dialog(void) {
    _create_info_dialog("Update Check Failed",
                        "Could not reach update server.\nCheck your internet connection.",
                        lv_color_hex(0xFF4444));
    ESP_LOGI(TAG, "Showing OTA check-failed dialog");
}