#include "ui_wifi.h"
#include "../theme.h"
#include "ui_Screen3.h"
#include "ui.h"
#include "device_settings.h"
#include "../callbacks/ui_callbacks.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_wifi_types.h"
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_screen";
#define DEFAULT_SCAN_LIST_SIZE 20
#define CONNECT_TIMEOUT_MS 15000

// Forward declarations
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data);

// Structure to hold WiFi scan results
typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t auth_mode;
} wifi_ap_record;

// Connection state
typedef enum {
    WIFI_STATE_IDLE,
    WIFI_STATE_SCANNING,
    WIFI_STATE_CONNECTING,
    WIFI_STATE_CONNECTED,
    WIFI_STATE_DISCONNECTED,
    WIFI_STATE_FAILED
} wifi_connection_state_t;

// Static variables
static lv_obj_t *wifi_screen = NULL;
static lv_obj_t *wifi_list = NULL;
static lv_obj_t *password_modal = NULL;
static lv_obj_t *password_input = NULL;
static lv_obj_t *status_label = NULL;
static lv_obj_t *connection_progress = NULL;
static lv_obj_t *wifi_keyboard = NULL;
static char *selected_ssid = NULL;
static wifi_ap_record *ap_records = NULL;
static uint16_t ap_count = 0;
static bool wifi_screen_initialized = false;
static wifi_connection_state_t connection_state = WIFI_STATE_IDLE;
static lv_timer_t *connection_timeout_timer = NULL;
static lv_timer_t *wifi_event_timer = NULL;  // Timer for event task
static lv_obj_t *previous_screen = NULL;  // Store the screen that was active before WiFi
char *connected_ssid = NULL;

// Event handling variables
static bool wifi_event_pending = false;
static esp_event_base_t pending_event_base;
static int32_t pending_event_id;
static bool pending_connected = false;
static char pending_ssid[33] = {0};
static bool pending_needs_scan = false;

// Forward declarations
static void wifi_scan(void);
static void update_wifi_list(void);
static void connect_to_wifi(const char *ssid, const char *password);
static void show_password_modal(const char *ssid);
static void hide_password_modal(void);
static void update_connection_status(const char *message, bool is_error);
static void connection_timeout_cb(lv_timer_t *timer);
static void delayed_scan_cb(lv_timer_t *timer);

// Safe UI update function
static void wifi_event_task(lv_timer_t *timer) {
    if (!wifi_event_pending) return;
    
    // Critical fix: Only process WiFi events when the WiFi screen is actively displayed
    if (!wifi_screen || !lv_obj_is_valid(wifi_screen) || lv_scr_act() != wifi_screen) {
        wifi_event_pending = false;
        return;
    }

    if (pending_needs_scan) {
        wifi_scan();
        pending_needs_scan = false;
    }

    // Update status based on events
    switch (pending_event_id) {
        case WIFI_EVENT_STA_CONNECTED:
            connection_state = WIFI_STATE_CONNECTED;
            update_connection_status("Connected successfully!", false);
            if (pending_ssid[0] != '\0') {
                if (connected_ssid) free(connected_ssid);
                connected_ssid = strdup(pending_ssid);
            }
            hide_password_modal();
            update_wifi_list();
            if (connection_timeout_timer) {
                lv_timer_del(connection_timeout_timer);
                connection_timeout_timer = NULL;
            }
            break;
            
        case WIFI_EVENT_STA_DISCONNECTED:
            connection_state = WIFI_STATE_DISCONNECTED;
            update_connection_status("Disconnected from network", true);
            if (connected_ssid) {
                free(connected_ssid);
                connected_ssid = NULL;
            }
            update_wifi_list();
            if (connection_timeout_timer) {
                lv_timer_del(connection_timeout_timer);
                connection_timeout_timer = NULL;
            }
            break;
            
        case IP_EVENT_STA_GOT_IP:
            connection_state = WIFI_STATE_CONNECTED;
            update_connection_status("Connected with IP address!", false);
            break;
            
        case WIFI_EVENT_SCAN_DONE:
            connection_state = WIFI_STATE_IDLE;
            update_wifi_list();
            break;
    }

    wifi_event_pending = false;
}

// WiFi event handler
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                             int32_t event_id, void* event_data)
{
    pending_event_base = event_base;
    pending_event_id = event_id;
    
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi station started");
                break;
                
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Connected to WiFi");
                pending_connected = true;
                if (selected_ssid) {
                    strncpy(pending_ssid, selected_ssid, sizeof(pending_ssid) - 1);
                }
                break;
                
            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGI(TAG, "Disconnected from WiFi");
                pending_connected = false;
                break;
                
            case WIFI_EVENT_SCAN_DONE:
                ESP_LOGI(TAG, "WiFi scan completed");
                break;
        }
    }
    
    // Only set event pending if WiFi screen is currently active
    // This prevents stale events from being processed when screen reopens
    if (wifi_screen && lv_obj_is_valid(wifi_screen) && lv_scr_act() == wifi_screen) {
        wifi_event_pending = true;
    }
}

// Connection timeout handler
static void connection_timeout_cb(lv_timer_t *timer) {
    connection_state = WIFI_STATE_FAILED;
    update_connection_status("Connection timeout - Check password", true);
    esp_wifi_disconnect();
    hide_password_modal();
    connection_timeout_timer = NULL;
}

// Delayed scan callback - ensures UI is ready before scanning
static void delayed_scan_cb(lv_timer_t *timer) {
    // Delete the timer (one-shot)
    lv_timer_del(timer);
    
    // Only scan if the WiFi screen is still active and valid
    if (wifi_screen && lv_obj_is_valid(wifi_screen) && lv_scr_act() == wifi_screen) {
        ESP_LOGI(TAG, "Starting delayed WiFi scan");
        wifi_scan();
    } else {
        ESP_LOGW(TAG, "Delayed scan aborted - screen no longer active");
    }
}

// Update connection status
static void update_connection_status(const char *message, bool is_error) {
    if (!status_label || !lv_obj_is_valid(status_label)) return;
    
    lv_label_set_text(status_label, message);
    
    if (is_error) {
        lv_obj_set_style_text_color(status_label, THEME_COLOR_BTN_CLOSE, LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_obj_set_style_text_color(status_label, THEME_COLOR_STATUS_CONNECTED, LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    
    // Hide progress indicator if showing
    if (connection_progress && lv_obj_is_valid(connection_progress)) {
        lv_obj_add_flag(connection_progress, LV_OBJ_FLAG_HIDDEN);
    }
}

// Event handlers
static void cancel_btn_event_cb(lv_event_t *e) {
    hide_password_modal();
}

static void connect_btn_event_cb(lv_event_t *e) {
    if (!password_input || !selected_ssid) return;
    
    const char *password = lv_textarea_get_text(password_input);
    
    // Show connection progress
    if (connection_progress && lv_obj_is_valid(connection_progress)) {
        lv_obj_clear_flag(connection_progress, LV_OBJ_FLAG_HIDDEN);
    }
    
    connection_state = WIFI_STATE_CONNECTING;
    update_connection_status("Connecting to network...", false);
    
    // Start connection timeout timer
    if (connection_timeout_timer) {
        lv_timer_del(connection_timeout_timer);
    }
    connection_timeout_timer = lv_timer_create(connection_timeout_cb, CONNECT_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(connection_timeout_timer, 1);
    
    connect_to_wifi(selected_ssid, password);
}

static void password_input_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        if (!wifi_keyboard) {
            wifi_keyboard = lv_keyboard_create(password_modal);
            lv_obj_set_style_bg_color(wifi_keyboard, THEME_COLOR_KEYBOARD_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_opa(wifi_keyboard, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        }
        lv_keyboard_set_textarea(wifi_keyboard, password_input);
        lv_obj_clear_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(wifi_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    } else if (code == LV_EVENT_DEFOCUSED) {
        if (wifi_keyboard) {
            lv_obj_add_flag(wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Show password input modal
static void show_password_modal(const char *ssid) {
    if (password_modal && lv_obj_is_valid(password_modal)) {
        lv_obj_del(password_modal);
    }
    
    // Create modal background
    password_modal = lv_obj_create(wifi_screen);
    lv_obj_set_size(password_modal, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(password_modal, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(password_modal, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(password_modal, 180, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(password_modal, LV_OBJ_FLAG_SCROLLABLE);
    
    // Create modal dialog
    lv_obj_t *dialog = lv_obj_create(password_modal);
    lv_obj_set_size(dialog, 400, 280);
    lv_obj_align(dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(dialog, THEME_COLOR_PANEL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(dialog, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(dialog, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(dialog, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(dialog, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(dialog, 25, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(dialog, LV_OBJ_FLAG_SCROLLABLE);
    
    // Title
    lv_obj_t *title = lv_label_create(dialog);
    lv_label_set_text(title, "Connect to Network");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Network name
    lv_obj_t *network_label = lv_label_create(dialog);
    lv_label_set_text_fmt(network_label, "Network: %s", ssid);
    lv_obj_align(network_label, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_set_style_text_font(network_label, THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(network_label, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Password label
    lv_obj_t *pwd_label = lv_label_create(dialog);
    lv_label_set_text(pwd_label, "Password:");
    lv_obj_align(pwd_label, LV_ALIGN_TOP_LEFT, 0, 75);
    lv_obj_set_style_text_font(pwd_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(pwd_label, THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Password input
    password_input = lv_textarea_create(dialog);
    lv_textarea_set_password_mode(password_input, true);
    lv_textarea_set_one_line(password_input, true);
    lv_textarea_set_placeholder_text(password_input, "Enter network password");
    lv_obj_set_size(password_input, 350, 40);
    lv_obj_align(password_input, LV_ALIGN_TOP_LEFT, 0, 100);
    lv_obj_set_style_bg_color(password_input, THEME_COLOR_CONTROL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(password_input, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(password_input, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(password_input, 2, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(password_input, THEME_COLOR_SCROLLBAR, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(password_input, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(password_input, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(password_input, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(password_input, password_input_event_cb, LV_EVENT_ALL, NULL);
    
    // Connection progress (initially hidden)
    connection_progress = lv_spinner_create(dialog, 1000, 60);
    lv_obj_set_size(connection_progress, 30, 30);
    lv_obj_align(connection_progress, LV_ALIGN_TOP_LEFT, 0, 155);
    lv_obj_set_style_arc_color(connection_progress, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(connection_progress, LV_OBJ_FLAG_HIDDEN);
    
    // Button container
    lv_obj_t *btn_container = lv_obj_create(dialog);
    lv_obj_set_size(btn_container, 350, 50);
    lv_obj_align(btn_container, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(btn_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btn_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // Cancel button
    lv_obj_t *cancel_btn = lv_btn_create(btn_container);
    lv_obj_set_size(cancel_btn, 160, 40);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_GRAY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_GRAY_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(cancel_btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(cancel_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);
    lv_obj_set_style_text_font(cancel_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(cancel_btn, cancel_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Connect button
    lv_obj_t *connect_btn = lv_btn_create(btn_container);
    lv_obj_set_size(connect_btn, 160, 40);
    lv_obj_set_style_bg_color(connect_btn, THEME_COLOR_BTN_CONNECT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(connect_btn, THEME_COLOR_BTN_CONNECT_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(connect_btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(connect_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t *connect_label = lv_label_create(connect_btn);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);
    lv_obj_set_style_text_font(connect_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(connect_btn, connect_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    // Auto-focus password input
    lv_obj_add_state(password_input, LV_STATE_FOCUSED);
}

// Hide password modal
static void hide_password_modal(void) {
    if (password_modal && lv_obj_is_valid(password_modal)) {
        lv_obj_del(password_modal);
        password_modal = NULL;
        password_input = NULL;
        connection_progress = NULL;
        wifi_keyboard = NULL;
    }
}

// Network list item click handler
static void wifi_list_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED) return;
    
    lv_obj_t *btn = lv_event_get_target(e);
    char *ssid = (char *)lv_obj_get_user_data(btn);
    
    if (!ssid) return;
    
    // Store selected SSID
    if (selected_ssid) free(selected_ssid);
    selected_ssid = strdup(ssid);
    
    ESP_LOGI(TAG, "Selected network: %s", selected_ssid);
    
    // Check if this network requires password
    bool needs_password = true;
    for (int i = 0; i < ap_count; i++) {
        if (strcmp(ap_records[i].ssid, ssid) == 0) {
            needs_password = (ap_records[i].auth_mode != WIFI_AUTH_OPEN);
            break;
        }
    }
    
    if (needs_password) {
        show_password_modal(ssid);
    } else {
        // Connect directly for open networks
        connection_state = WIFI_STATE_CONNECTING;
        update_connection_status("Connecting to open network...", false);
        connect_to_wifi(ssid, "");
    }
}

// Forget network handler
static void forget_btn_event_cb(lv_event_t *e) {
    if (!connected_ssid) return;
    
    ESP_LOGI(TAG, "Forgetting network: %s", connected_ssid);
    
    update_connection_status("Disconnecting...", false);
    
    // Clear connected SSID
    free(connected_ssid);
    connected_ssid = NULL;
    
    // Disconnect and scan
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    wifi_scan();
}

// Back button handler
static void back_btn_event_cb(lv_event_t *e) {
    // Return to the previous screen
    if (previous_screen && lv_obj_is_valid(previous_screen)) {
        lv_scr_load(previous_screen);
    }
    
    // Clean up WiFi screen after switching
    if (wifi_screen && lv_obj_is_valid(wifi_screen)) {
        lv_obj_del(wifi_screen);
        wifi_screen = NULL;
    }
}

// Refresh/scan button handler
static void refresh_btn_event_cb(lv_event_t *e) {
    connection_state = WIFI_STATE_SCANNING;
    update_connection_status("Scanning for networks...", false);
    wifi_scan();
}

// WiFi scan function
static void wifi_scan(void) {
    // Safety check - don't scan if WiFi screen is not active or valid
    if (!wifi_screen || !lv_obj_is_valid(wifi_screen) || !wifi_screen_initialized) {
        ESP_LOGW(TAG, "WiFi scan aborted - screen not active");
        return;
    }
    
    ESP_LOGI(TAG, "Starting WiFi scan");
    
    // Configure scan
    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 100,
        .scan_time.active.max = 500
    };

    // Start scan
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scan: %s", esp_err_to_name(err));
        update_connection_status("Scan failed - Try again", true);
        return;
    }

    // Get scan results
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    
    if (ap_num > DEFAULT_SCAN_LIST_SIZE) {
        ap_num = DEFAULT_SCAN_LIST_SIZE;
    }

    if (ap_num == 0) {
        ap_count = 0;
        update_connection_status("No networks found", true);
        return;
    }

    // Allocate memory for scan results
    wifi_ap_record_t *ap_info = malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (!ap_info) {
        ESP_LOGE(TAG, "Failed to allocate memory for scan results");
        update_connection_status("Memory error", true);
        return;
    }

    // Get scan results
    err = esp_wifi_scan_get_ap_records(&ap_num, ap_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get scan records: %s", esp_err_to_name(err));
        free(ap_info);
        update_connection_status("Scan failed", true);
        return;
    }

    // Free previous records
    if (ap_records) {
        free(ap_records);
    }

    // Allocate memory for our records
    ap_records = malloc(sizeof(wifi_ap_record) * ap_num);
    if (!ap_records) {
        ESP_LOGE(TAG, "Failed to allocate memory for AP records");
        free(ap_info);
        update_connection_status("Memory error", true);
        return;
    }

    // Copy scan results
    ap_count = ap_num;
    for (int i = 0; i < ap_count; i++) {
        strncpy(ap_records[i].ssid, (char *)ap_info[i].ssid, 32);
        ap_records[i].ssid[32] = '\0';
        ap_records[i].rssi = ap_info[i].rssi;
        ap_records[i].auth_mode = ap_info[i].authmode;
    }

    free(ap_info);
    
    // Sort by signal strength
    for (int i = 0; i < ap_count - 1; i++) {
        for (int j = 0; j < ap_count - i - 1; j++) {
            if (ap_records[j].rssi < ap_records[j + 1].rssi) {
                wifi_ap_record temp = ap_records[j];
                ap_records[j] = ap_records[j + 1];
                ap_records[j + 1] = temp;
            }
        }
    }

    update_connection_status("Scan completed", false);
    ESP_LOGI(TAG, "Scan completed, found %d networks", ap_count);
}

// Update WiFi list UI
static void update_wifi_list(void) {
    if (!wifi_list || !lv_obj_is_valid(wifi_list)) {
        return;
    }

    // Clear existing list
    lv_obj_clean(wifi_list);
    
    if (ap_count == 0 || !ap_records) {
        lv_obj_t *empty_btn = lv_list_add_btn(wifi_list, NULL, "No networks found");
        lv_obj_set_style_bg_color(empty_btn, THEME_COLOR_BTN_NEUTRAL, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(empty_btn, THEME_COLOR_TEXT_HINT, LV_PART_MAIN | LV_STATE_DEFAULT);
        return;
    }

    for (int i = 0; i < ap_count; i++) {
        if (ap_records[i].ssid[0] == '\0') continue;
        
        // Create network info string
        char network_info[100];
        const char* security_text = (ap_records[i].auth_mode == WIFI_AUTH_OPEN) ? "Open" : "Secured";
        const char* signal_text;
        
        // Signal strength text
        if (ap_records[i].rssi > -50) signal_text = "Excellent";
        else if (ap_records[i].rssi > -70) signal_text = "Good";
        else if (ap_records[i].rssi > -80) signal_text = "Fair";
        else signal_text = "Weak";
        
        snprintf(network_info, sizeof(network_info), "%s\n%s | %s (%d dBm)", 
                 ap_records[i].ssid, security_text, signal_text, ap_records[i].rssi);
        
        lv_obj_t *btn = lv_list_add_btn(wifi_list, NULL, network_info);
        if (!btn) continue;

        // Check if this is the connected network
        bool is_connected = (connected_ssid && strcmp(ap_records[i].ssid, connected_ssid) == 0);
        
        // Style based on connection status
        if (is_connected) {
            lv_obj_set_style_bg_color(btn, THEME_COLOR_BTN_CONNECT, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(btn, THEME_COLOR_BTN_CONNECT_PRESSED, LV_PART_MAIN | LV_STATE_PRESSED);
            
            // Add connected indicator
            lv_obj_t *status_label = lv_label_create(btn);
            lv_label_set_text(status_label, "CONNECTED");
            lv_obj_align(status_label, LV_ALIGN_RIGHT_MID, -15, -10);
            lv_obj_set_style_text_color(status_label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_text_font(status_label, THEME_FONT_TINY, LV_PART_MAIN | LV_STATE_DEFAULT);
            
            // Add forget button
            lv_obj_t *forget_btn = lv_btn_create(btn);
            lv_obj_set_size(forget_btn, 80, 25);
            lv_obj_align(forget_btn, LV_ALIGN_RIGHT_MID, -15, 10);
            lv_obj_set_style_bg_color(forget_btn, THEME_COLOR_BTN_CLOSE, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_radius(forget_btn, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_border_width(forget_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
            
            lv_obj_t *forget_label = lv_label_create(forget_btn);
            lv_label_set_text(forget_label, "Forget");
            lv_obj_center(forget_label);
            lv_obj_set_style_text_font(forget_label, THEME_FONT_TINY, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_add_event_cb(forget_btn, forget_btn_event_cb, LV_EVENT_CLICKED, NULL);
        } else {
            lv_obj_set_style_bg_color(btn, THEME_COLOR_CONTROL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
            lv_obj_set_style_bg_color(btn, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_PRESSED);
        }

        // Common styling
        lv_obj_set_size(btn, lv_pct(100), 60);
        lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(btn, THEME_COLOR_SCROLLBAR, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_radius(btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(btn, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(btn, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(btn, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);

        // Store SSID in user data
        char *ssid_copy = malloc(strlen(ap_records[i].ssid) + 1);
        strcpy(ssid_copy, ap_records[i].ssid);
        lv_obj_set_user_data(btn, ssid_copy);

        // Add click event
        lv_obj_add_event_cb(btn, wifi_list_event_cb, LV_EVENT_CLICKED, NULL);
    }
}

// Connect to WiFi function
static void connect_to_wifi(const char *ssid, const char *password) {
    if (!ssid) {
        ESP_LOGE(TAG, "SSID is NULL");
        return;
    }

    ESP_LOGI(TAG, "Connecting to WiFi: %s", ssid);

    // Configure WiFi
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    
    if (password && strlen(password) > 0) {
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    }

    // Set configuration and connect
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(err));
        update_connection_status("Configuration failed", true);
        return;
    }

    // Disconnect from current network if connected
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    // Connect to new network
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect: %s", esp_err_to_name(err));
        update_connection_status("Connection failed", true);
        return;
    }
}

// Initialize WiFi screen
void init_wifi_screen(void) {
    if (wifi_screen_initialized) {
        return;
    }

    ESP_LOGI(TAG, "Initializing WiFi screen");

    // Initialize networking (handle multiple init calls gracefully)
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }
    
    (void)esp_netif_create_default_wifi_sta();
    
    // Initialize WiFi
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    
    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // Start WiFi
    ESP_ERROR_CHECK(esp_wifi_start());

    // Create event timer (only once, store reference)
    if (!wifi_event_timer) {
        wifi_event_timer = lv_timer_create(wifi_event_task, 100, NULL);
    }

    wifi_screen_initialized = true;
    ESP_LOGI(TAG, "WiFi screen initialized");
}

// Reset static variables to prevent crashes on re-entry
static void reset_wifi_screen_state(void) {
    // Reset UI object pointers
    wifi_list = NULL;
    password_modal = NULL;
    password_input = NULL;
    status_label = NULL;
    connection_progress = NULL;
    wifi_keyboard = NULL;
    
    // Reset state variables
    connection_state = WIFI_STATE_IDLE;
    
    // Clear any existing timeout timer
    if (connection_timeout_timer) {
        lv_timer_del(connection_timeout_timer);
        connection_timeout_timer = NULL;
    }
    
    // Keep wifi_event_timer running - it's persistent across screen recreations
    
    // Reset event handling variables
    wifi_event_pending = false;
    pending_connected = false;
    memset(pending_ssid, 0, sizeof(pending_ssid));
    pending_needs_scan = false;
}

// Show WiFi screen
void show_wifi_screen(void) {
    if (!wifi_screen_initialized) {
        init_wifi_screen();
    }

    // Store the current screen before switching to WiFi
    previous_screen = lv_scr_act();

    if (wifi_screen) {
        lv_obj_del(wifi_screen);
    }
    
    // Reset all static variables to prevent crashes
    reset_wifi_screen_state();
    
    // Clear any stale WiFi events that arrived while screen was closed
    wifi_event_pending = false;
    pending_connected = false;
    memset(pending_ssid, 0, sizeof(pending_ssid));
    pending_needs_scan = false;
    
    // Close any active text input dialogs
    force_close_text_input_dialog();

    // Create main screen
    wifi_screen = lv_obj_create(NULL);
    lv_obj_set_size(wifi_screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(wifi_screen, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(wifi_screen, LV_OBJ_FLAG_SCROLLABLE);
    
    // Main container
    lv_obj_t* container = lv_obj_create(wifi_screen);
    lv_obj_set_size(container, 750, 450);
    lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(container, THEME_COLOR_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(container, THEME_COLOR_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(container, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(container, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(container, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

    // Header
    lv_obj_t* header = lv_obj_create(container);
    lv_obj_set_size(header, lv_pct(100), 60);
    lv_obj_align(header, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(header, THEME_COLOR_PANEL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(header, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(header, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    
    // Back button
    lv_obj_t *back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 60, 40);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 15, 0);
    lv_obj_set_style_bg_color(back_btn, THEME_COLOR_ACCENT_BLUE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(back_btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(back_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "< Back");
    lv_obj_center(back_label);
    lv_obj_set_style_text_font(back_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Title
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Wi-Fi Networks");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Refresh button
    lv_obj_t *refresh_btn = lv_btn_create(header);
    lv_obj_set_size(refresh_btn, 80, 40);
    lv_obj_align(refresh_btn, LV_ALIGN_RIGHT_MID, -15, 0);
    lv_obj_set_style_bg_color(refresh_btn, THEME_COLOR_BTN_SAVE_ALT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(refresh_btn, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(refresh_btn, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    lv_obj_t *refresh_label = lv_label_create(refresh_btn);
    lv_label_set_text(refresh_label, "Scan");
    lv_obj_center(refresh_label);
    lv_obj_set_style_text_font(refresh_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(refresh_label, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(refresh_btn, refresh_btn_event_cb, LV_EVENT_CLICKED, NULL);

    // Status bar
    lv_obj_t* status_bar = lv_obj_create(container);
    lv_obj_set_size(status_bar, lv_pct(100), 40);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_set_style_bg_color(status_bar, THEME_COLOR_SECTION_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(status_bar, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(status_bar, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(status_bar, THEME_COLOR_BORDER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(status_bar, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Status label
    status_label = lv_label_create(status_bar);
    lv_label_set_text(status_label, "Scanning for networks...");
    lv_obj_align(status_label, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_text_color(status_label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(status_label, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Networks list
    wifi_list = lv_list_create(container);
    lv_obj_set_size(wifi_list, lv_pct(100), 290);
    lv_obj_align(wifi_list, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_set_style_bg_color(wifi_list, THEME_COLOR_SURFACE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(wifi_list, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(wifi_list, THEME_COLOR_CONTROL_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(wifi_list, 6, LV_PART_MAIN | LV_STATE_DEFAULT);
    
    // Scrollbar styling
    lv_obj_set_style_bg_color(wifi_list, THEME_COLOR_SCROLLBAR, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(wifi_list, 200, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_width(wifi_list, 6, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);

    lv_scr_load(wifi_screen);
    
    // Auto-start scan with a small delay to ensure UI is ready
    lv_timer_create(delayed_scan_cb, 100, NULL);
}



// Cleanup functions
void hide_wifi_screen(void) {
    // Cancel any ongoing scan first
    esp_wifi_scan_stop();
    
    // Delete any active timers
    if (connection_timeout_timer) {
        lv_timer_del(connection_timeout_timer);
        connection_timeout_timer = NULL;
    }
    
    if (wifi_screen && lv_obj_is_valid(wifi_screen)) {
        lv_obj_del(wifi_screen);
        wifi_screen = NULL;
        previous_screen = NULL;  // Clear the previous screen reference
    }
    
    // Reset state to prevent issues on next entry
    reset_wifi_screen_state();
    
    // DON'T deinitialize WiFi system - keep it running to prevent scan crashes
    // Just reset the UI state
}

void wifi_screen_delete(void) {
    hide_wifi_screen();  // Reuse the existing hide function
}

bool is_wifi_screen_active(void) {
    return wifi_screen && lv_obj_is_valid(wifi_screen) && lv_scr_act() == wifi_screen;
}

