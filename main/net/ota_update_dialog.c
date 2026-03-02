#include "ota_update_dialog.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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
static bool update_in_progress = false;

// Forward declarations
static void install_btn_event_cb(lv_event_t *e);
static void cancel_btn_event_cb(lv_event_t *e);
static void progress_timer_cb(lv_timer_t *timer);

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
void show_ota_update_dialog(const char* current_version, const char* new_version, const char* update_type, float file_size_mb, const char* release_notes) {
    // Close existing dialog if any
    close_ota_update_dialog();
    
    ESP_LOGI(TAG, "Showing OTA update dialog for version %s", new_version);
    
    // Create modal background
    ota_modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ota_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(ota_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ota_modal, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ota_modal, 180, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ota_modal, LV_OBJ_FLAG_SCROLLABLE);
    
    // Create main dialog
    ota_dialog = lv_obj_create(ota_modal);
    lv_obj_set_size(ota_dialog, 500, 400);
    lv_obj_align(ota_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(ota_dialog, lv_color_hex(0x2E2F2E), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ota_dialog, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ota_dialog, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ota_dialog, lv_color_hex(0x4080FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ota_dialog, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ota_dialog, 25, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(ota_dialog, LV_OBJ_FLAG_SCROLLABLE);
    
    // Title
    lv_obj_t *title = lv_label_create(ota_dialog);
    lv_label_set_text(title, "Firmware Update Available");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Version info
    lv_obj_t *version_label = lv_label_create(ota_dialog);
    lv_label_set_text_fmt(version_label, "Current: %s -> New: %s", current_version, new_version);
    lv_obj_align(version_label, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_text_font(version_label, &lv_font_montserrat_14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(version_label, lv_color_hex(0xCCCCCC), LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Update type
    lv_obj_t *type_label = lv_label_create(ota_dialog);
    lv_label_set_text_fmt(type_label, "Type: %s", update_type);
    lv_obj_align(type_label, LV_ALIGN_TOP_LEFT, 0, 65);
    lv_obj_set_style_text_font(type_label, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(type_label, lv_color_hex(0x4080FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // File size
    lv_obj_t *size_label = lv_label_create(ota_dialog);
    lv_label_set_text_fmt(size_label, "Size: %.1f MB", file_size_mb);
    lv_obj_align(size_label, LV_ALIGN_TOP_LEFT, 0, 85);
    lv_obj_set_style_text_font(size_label, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(size_label, lv_color_hex(0xCCCCCC), LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Release notes (if provided)
    if (release_notes && strlen(release_notes) > 0) {
        lv_obj_t *notes_label = lv_label_create(ota_dialog);
        lv_label_set_text_fmt(notes_label, "Notes: %s", release_notes);
        lv_obj_align(notes_label, LV_ALIGN_TOP_LEFT, 0, 110);
        lv_obj_set_style_text_font(notes_label, &lv_font_montserrat_10, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(notes_label, lv_color_hex(0xAAAAA), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_label_set_long_mode(notes_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(notes_label, 450);
    }
    
    // Progress bar (initially hidden)
    progress_bar = lv_bar_create(ota_dialog);
    lv_obj_set_size(progress_bar, 450, 20);
    lv_obj_align(progress_bar, LV_ALIGN_TOP_LEFT, 0, 180);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x333333), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x4080FF), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(progress_bar, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(progress_bar, 10, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_add_flag(progress_bar, LV_OBJ_FLAG_HIDDEN);
    
    // Progress label (initially hidden)
    progress_label = lv_label_create(ota_dialog);
    lv_label_set_text(progress_label, "0%");
    lv_obj_align(progress_label, LV_ALIGN_TOP_RIGHT, 0, 185);
    lv_obj_set_style_text_font(progress_label, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(progress_label, lv_color_hex(0x4080FF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(progress_label, LV_OBJ_FLAG_HIDDEN);
    
    // Status label
    status_label = lv_label_create(ota_dialog);
    lv_label_set_text(status_label, "Ready to install firmware update");
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 0, 220);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xCCCCCC), LV_PART_MAIN | LV_STATE_DEFAULT);
    
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
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x666666), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x777777), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(cancel_btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cancel_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_set_style_text_font(cancel_label, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Install button
    install_btn = lv_btn_create(btn_container);
    lv_obj_set_size(install_btn, 200, 40);
    lv_obj_set_style_bg_color(install_btn, lv_color_hex(0x00AA44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(install_btn, lv_color_hex(0x00CC55), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(install_btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(install_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t *install_label = lv_label_create(install_btn);
    lv_label_set_text(install_label, "Install Update");
    lv_obj_center(install_label);
    lv_obj_set_style_text_font(install_label, &lv_font_montserrat_12, LV_PART_MAIN | LV_STATE_DEFAULT);
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