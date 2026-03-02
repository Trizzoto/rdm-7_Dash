#include "ui_callbacks.h"
#include "../theme.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lvgl.h"
#include "../screens/ui_Screen3.h"
#include "../screens/ui_wifi.h"
#include "../ui.h"
#include "esp_log.h"
#include "storage/config_store.h"

// External references to global variables from ui_Screen3.c
extern lv_obj_t * g_label_input[];
extern lv_obj_t * g_can_id_input[];
extern lv_obj_t * g_endian_dropdown[];
extern lv_obj_t * g_bit_start_dropdown[];
extern lv_obj_t * g_bit_length_dropdown[];
extern lv_obj_t * g_scale_input[];
extern lv_obj_t * g_offset_input[];
extern lv_obj_t * g_decimals_dropdown[];
extern lv_obj_t * g_type_dropdown[];

extern value_config_t values_config[];
extern char label_texts[13][64];
extern lv_obj_t* ui_MenuScreen;
extern lv_obj_t* keyboard;

// External references to UI objects that may be used in callbacks
extern lv_obj_t* ui_Label[];
extern lv_obj_t* ui_Gear_Label;
extern lv_obj_t* ui_Bar_1_Label;
extern lv_obj_t* ui_Bar_2_Label;
extern lv_obj_t* ui_Bar_1_Value;
extern lv_obj_t* ui_Bar_2_Value;
extern lv_obj_t* ui_Kmh;
extern lv_obj_t* menu_speed_units_label;
extern lv_obj_t* ui_CustomText[];

extern void fuel_sender_capture_empty(uint8_t value_id);
extern void fuel_sender_capture_full(uint8_t value_id);
extern float fuel_sender_get_filtered_v(uint8_t bar_idx);

#define RPM_VALUE_ID   9
#define SPEED_VALUE_ID 10
#define GEAR_VALUE_ID  11
#define BAR1_VALUE_ID  12
#define BAR2_VALUE_ID  13

// External function references
extern void print_value_config(uint8_t value_id);

// Placeholder functions - you will move the actual implementations here
void label_input_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * textarea = lv_event_get_target(e);
        const char * txt = lv_textarea_get_text(textarea);
        if (txt == NULL) return;

        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);

        // Handle regular panels (1-8) and gear
        if ((value_id >= 1 && value_id <= 8) || (value_id == GEAR_VALUE_ID)) {
            strncpy(label_texts[value_id - 1], txt, sizeof(label_texts[value_id - 1]));
            label_texts[value_id - 1][sizeof(label_texts[value_id - 1]) - 1] = '\0';

            if(value_id == GEAR_VALUE_ID && ui_Gear_Label) {
                lv_label_set_text(ui_Gear_Label, label_texts[value_id - 1]);
            } else if (ui_Label[value_id - 1]) {
                lv_label_set_text(ui_Label[value_id - 1], label_texts[value_id - 1]);
            }
            
            // Also update menu preview label if it exists and menu is visible
            if (value_id >= 1 && value_id <= 8) {
                extern lv_obj_t * menu_panel_labels[8];
                extern lv_obj_t * ui_MenuScreen;
                uint8_t idx = value_id - 1;
                if (menu_panel_labels[idx] && lv_obj_is_valid(menu_panel_labels[idx]) && 
                    ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
                    lv_label_set_text(menu_panel_labels[idx], txt);
                }
            }
        }
        // Handle bar labels
        else if (value_id == BAR1_VALUE_ID && ui_Bar_1_Label) {
            strncpy(label_texts[value_id - 1], txt, sizeof(label_texts[value_id - 1]));
            label_texts[value_id - 1][sizeof(label_texts[value_id - 1]) - 1] = '\0';
            lv_label_set_text(ui_Bar_1_Label, txt);
            
            // Also update menu preview bar label if it exists and menu is visible
            extern lv_obj_t * menu_bar_labels[2];
            extern lv_obj_t * ui_MenuScreen;
            if (menu_bar_labels[0] && lv_obj_is_valid(menu_bar_labels[0]) && 
                ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
                lv_label_set_text(menu_bar_labels[0], txt);
            }
        }
        else if (value_id == BAR2_VALUE_ID && ui_Bar_2_Label) {
            strncpy(label_texts[value_id - 1], txt, sizeof(label_texts[value_id - 1]));
            label_texts[value_id - 1][sizeof(label_texts[value_id - 1]) - 1] = '\0';
            lv_label_set_text(ui_Bar_2_Label, txt);
            
            // Also update menu preview bar label if it exists and menu is visible
            extern lv_obj_t * menu_bar_labels[2];
            extern lv_obj_t * ui_MenuScreen;
            if (menu_bar_labels[1] && lv_obj_is_valid(menu_bar_labels[1]) && 
                ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
                lv_label_set_text(menu_bar_labels[1], txt);
            }
        }
    }
}


void bit_start_roller_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dropdown = lv_event_get_target(e);
        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);

        if (value_id < 1 || value_id > 13) return;

        // Get the selected option index - these are dropdown widgets, not rollers
        uint8_t selected_bit_start = lv_dropdown_get_selected(dropdown);
        values_config[value_id - 1].bit_start = selected_bit_start;

        printf("Updated Bit Start for Value #%d to %d\n", value_id, selected_bit_start);
        print_value_config(value_id);
    }
}


void bit_length_roller_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dropdown = lv_event_get_target(e);
        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);

        if (value_id < 1 || value_id > 13) return;

        // Get the selected option index and adjust for 1-based length - these are dropdown widgets, not rollers
        uint8_t selected_bit_length = lv_dropdown_get_selected(dropdown) + 1;
        values_config[value_id - 1].bit_length = selected_bit_length;

        printf("Updated Bit Length for Value #%d to %d\n", value_id, selected_bit_length);
        print_value_config(value_id);
    }
}

void decimal_dropdown_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t * dropdown = lv_event_get_target(e);

    // Get which value_id we're editing
    uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
    if(value_id < 1 || value_id > 13) return;

    // 0 => "0", 1 => "1", 2 => "2"
    uint8_t selected = lv_dropdown_get_selected(dropdown);
    values_config[value_id - 1].decimals = selected;

    printf("Updated Decimals for Value #%d to %d\n", value_id, selected);
    print_value_config(value_id);
}

// Text Input Dialog Event Handlers
static text_input_dialog_t *current_text_dialog = NULL;
/* Set to true during close_text_input_dialog() so that any focus events
 * triggered by LVGL's internal keyboard/group teardown are ignored. */
static bool s_dialog_closing = false;
/* After closing, block ALL textareas from reopening for a short window.
 * 200ms is long enough to absorb any LVGL-internal cascade (fires within
 * microseconds) but short enough that a deliberate subsequent tap is never
 * blocked (human reaction time is comfortably above 200ms). */
static uint32_t s_dialog_close_tick = 0;
#define DIALOG_REOPEN_COOLDOWN_MS 200

// Text Input Dialog Event Handlers
static void text_input_keyboard_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    
    if (!current_text_dialog) return;
    
    if (code == LV_EVENT_VALUE_CHANGED) {
        // Update the text display with current keyboard input
        const char *text = lv_textarea_get_text(current_text_dialog->target_textarea);
        if (text && current_text_dialog->text_display) {
            lv_label_set_text(current_text_dialog->text_display, text);
            // Restore black color for actual text (not placeholder)
            lv_obj_set_style_text_color(current_text_dialog->text_display, lv_color_black(), 0);
        }
    }
    else if (code == LV_EVENT_READY) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev) lv_indev_reset(indev, NULL);

        if (current_text_dialog) {
            const char *text = lv_textarea_get_text(current_text_dialog->target_textarea);
            
            if (current_text_dialog->on_confirm) {
                current_text_dialog->on_confirm(text ? text : "", current_text_dialog->user_data);
            }
            
            if (current_text_dialog->target_textarea && lv_obj_is_valid(current_text_dialog->target_textarea)) {
                lv_event_send(current_text_dialog->target_textarea, LV_EVENT_VALUE_CHANGED, NULL);
            }
        }
        close_text_input_dialog();
    }
    else if (code == LV_EVENT_CANCEL) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev) lv_indev_reset(indev, NULL);

        if (current_text_dialog) {
            if (current_text_dialog->target_textarea && lv_obj_is_valid(current_text_dialog->target_textarea)) {
                if (current_text_dialog->current_text) {
                    lv_textarea_set_text(current_text_dialog->target_textarea, current_text_dialog->current_text);
                } else {
                    lv_textarea_set_text(current_text_dialog->target_textarea, "");
                }
            }
            
            if (current_text_dialog->on_cancel) {
                current_text_dialog->on_cancel(current_text_dialog->user_data);
            }
        }
        close_text_input_dialog();
    }
}

static void text_input_ok_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    /* Reset the active touch device NOW, while we are still inside the event
     * handler and lv_indev_get_act() is valid.  This discards the current
     * touch so that when the modal is deleted the release event cannot fall
     * through and activate whatever textarea is sitting behind the OK button. */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) lv_indev_reset(indev, NULL);
    
    if (current_text_dialog) {
        const char *text = lv_textarea_get_text(current_text_dialog->target_textarea);
        
        if (current_text_dialog->on_confirm) {
            current_text_dialog->on_confirm(text ? text : "", current_text_dialog->user_data);
        }
        
        if (current_text_dialog->target_textarea && lv_obj_is_valid(current_text_dialog->target_textarea)) {
            lv_event_send(current_text_dialog->target_textarea, LV_EVENT_VALUE_CHANGED, NULL);
        }
    }
    close_text_input_dialog();
}

static void text_input_cancel_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;

    /* Same indev reset as OK — discard the current touch before the modal
     * disappears so nothing beneath receives the release. */
    lv_indev_t *indev = lv_indev_get_act();
    if (indev) lv_indev_reset(indev, NULL);
    
    if (current_text_dialog) {
        if (current_text_dialog->target_textarea && lv_obj_is_valid(current_text_dialog->target_textarea)) {
            if (current_text_dialog->current_text) {
                lv_textarea_set_text(current_text_dialog->target_textarea, current_text_dialog->current_text);
            } else {
                lv_textarea_set_text(current_text_dialog->target_textarea, "");
            }
        }
        
        if (current_text_dialog->on_cancel) {
            current_text_dialog->on_cancel(current_text_dialog->user_data);
        }
    }
    close_text_input_dialog();
}

void show_text_input_dialog_ex(lv_obj_t *target_textarea, const char *title, const char *placeholder, bool show_prefix,
                              void (*on_confirm)(const char *text, void *user_data),
                              void (*on_cancel)(void *user_data), void *user_data) {
    
    // Check if we're on the WiFi screen - don't create dialog
    if (is_wifi_screen_active()) {
        return;
    }
    
    // Validate target textarea
    if (!target_textarea || !lv_obj_is_valid(target_textarea)) {
        return;
    }
    
    // Close any existing dialog
    if (current_text_dialog) {
        close_text_input_dialog();
    }
    
    // Allocate dialog structure
    current_text_dialog = lv_mem_alloc(sizeof(text_input_dialog_t));
    if (!current_text_dialog) return;
    
    memset(current_text_dialog, 0, sizeof(text_input_dialog_t));
    current_text_dialog->target_textarea = target_textarea;
    current_text_dialog->on_confirm = on_confirm;
    current_text_dialog->on_cancel = on_cancel;
    current_text_dialog->user_data = user_data;
    
    // Save the original text so we can restore it on cancel
    const char *original = lv_textarea_get_text(target_textarea);
    if (original && strlen(original) > 0) {
        current_text_dialog->current_text = lv_mem_alloc(strlen(original) + 1);
        if (current_text_dialog->current_text) {
            strcpy(current_text_dialog->current_text, original);
        }
    } else {
        current_text_dialog->current_text = NULL;
    }
    
    // Create modal background on current screen (not top layer) for screenshots
    current_text_dialog->modal = lv_obj_create(lv_scr_act());
    lv_obj_set_size(current_text_dialog->modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(current_text_dialog->modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(current_text_dialog->modal, LV_OPA_70, 0);
    lv_obj_clear_flag(current_text_dialog->modal, LV_OBJ_FLAG_SCROLLABLE);
    
    // Create dialog container - taller to fully contain buttons
    lv_obj_t *dialog_container = lv_obj_create(current_text_dialog->modal);
    lv_obj_set_size(dialog_container, 400, 160);
    lv_obj_set_style_bg_color(dialog_container, THEME_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(dialog_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dialog_container, THEME_COLOR_SCROLLBAR, 0);
    lv_obj_set_style_border_width(dialog_container, 2, 0);
    lv_obj_set_style_radius(dialog_container, 8, 0);
    lv_obj_align(dialog_container, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_clear_flag(dialog_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // Title label - smaller font
    if (title) {
        lv_obj_t *title_label = lv_label_create(dialog_container);
        lv_label_set_text(title_label, title);
        lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(title_label, THEME_FONT_BODY, 0);
        lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 8);
    }
    
    // Create prefix label if needed (for CAN ID: 0x)
    if (show_prefix) {
        current_text_dialog->prefix_label = lv_label_create(dialog_container);
        lv_label_set_text(current_text_dialog->prefix_label, "0x");
        lv_obj_set_style_text_color(current_text_dialog->prefix_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(current_text_dialog->prefix_label, THEME_FONT_BODY, 0);
        lv_obj_align(current_text_dialog->prefix_label, LV_ALIGN_TOP_MID, -130, 47);
    } else {
        current_text_dialog->prefix_label = NULL;
    }
    
    // Text display area (shows what user is typing) - white background like config menu
    current_text_dialog->text_display = lv_label_create(dialog_container);
    int display_width = show_prefix ? 260 : 300;  // Narrower if prefix is shown
    int display_x = show_prefix ? 20 : 0;          // Offset to right if prefix is shown
    lv_obj_set_size(current_text_dialog->text_display, display_width, 30);
    lv_obj_set_style_bg_color(current_text_dialog->text_display, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(current_text_dialog->text_display, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(current_text_dialog->text_display, THEME_COLOR_SCROLLBAR, 0);
    lv_obj_set_style_border_width(current_text_dialog->text_display, 1, 0);
    lv_obj_set_style_radius(current_text_dialog->text_display, 4, 0);
    lv_obj_set_style_text_color(current_text_dialog->text_display, lv_color_black(), 0);
    lv_obj_set_style_text_font(current_text_dialog->text_display, THEME_FONT_BODY, 0);
    lv_obj_set_style_pad_all(current_text_dialog->text_display, 6, 0);
    lv_obj_align(current_text_dialog->text_display, LV_ALIGN_TOP_MID, display_x, 40);
    lv_label_set_long_mode(current_text_dialog->text_display, LV_LABEL_LONG_SCROLL_CIRCULAR);
    
    // Set initial text or placeholder
    if (target_textarea) {
        const char *current_text = lv_textarea_get_text(target_textarea);
        if (current_text && strlen(current_text) > 0) {
            lv_label_set_text(current_text_dialog->text_display, current_text);
            // Keep black text for existing content
            lv_obj_set_style_text_color(current_text_dialog->text_display, lv_color_black(), 0);
        } else if (placeholder) {
            lv_label_set_text(current_text_dialog->text_display, placeholder);
            // Grey color for placeholder text
            lv_obj_set_style_text_color(current_text_dialog->text_display, THEME_COLOR_TEXT_GHOST, 0);
        }
    }
    
    // Button container - positioned with more space in taller dialog
    lv_obj_t *btn_container = lv_obj_create(dialog_container);
    lv_obj_set_size(btn_container, 300, 35);
    lv_obj_set_style_bg_opa(btn_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_container, 0, 0);
    lv_obj_set_style_pad_all(btn_container, 0, 0);
    lv_obj_align(btn_container, LV_ALIGN_TOP_MID, 0, 105);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(btn_container, LV_FLEX_ALIGN_SPACE_EVENLY, 0);
    lv_obj_clear_flag(btn_container, LV_OBJ_FLAG_SCROLLABLE);
    
    // Cancel button - smaller
    lv_obj_t *cancel_btn = lv_btn_create(btn_container);
    lv_obj_set_size(cancel_btn, 120, 30);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_CANCEL, 0);
    lv_obj_set_style_radius(cancel_btn, 6, 0);
    lv_obj_add_event_cb(cancel_btn, text_input_cancel_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(cancel_label, THEME_FONT_SMALL, 0);
    lv_obj_center(cancel_label);
    
    // OK button - smaller
    lv_obj_t *ok_btn = lv_btn_create(btn_container);
    lv_obj_set_size(ok_btn, 120, 30);
    lv_obj_set_style_bg_color(ok_btn, THEME_COLOR_BTN_SAVE, 0);
    lv_obj_set_style_radius(ok_btn, 6, 0);
    lv_obj_add_event_cb(ok_btn, text_input_ok_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, "OK");
    lv_obj_set_style_text_color(ok_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(ok_label, THEME_FONT_SMALL, 0);
    lv_obj_center(ok_label);
    
    // Create keyboard
    current_text_dialog->keyboard = lv_keyboard_create(current_text_dialog->modal);
    lv_obj_set_style_bg_color(current_text_dialog->keyboard, THEME_COLOR_KEYBOARD_BG, 0);
    lv_obj_set_style_bg_opa(current_text_dialog->keyboard, LV_OPA_COVER, 0);
    lv_obj_align(current_text_dialog->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    
    // Connect keyboard to target textarea
    if (target_textarea) {
        lv_keyboard_set_textarea(current_text_dialog->keyboard, target_textarea);
        lv_obj_add_event_cb(current_text_dialog->keyboard, text_input_keyboard_event_cb, LV_EVENT_ALL, NULL);
        
        // Focus the textarea to show cursor
        lv_obj_add_state(target_textarea, LV_STATE_FOCUSED);
    }
}

void close_text_input_dialog(void) {
    if (!current_text_dialog) return;
    if (s_dialog_closing) return;  /* prevent re-entrant teardown */

    s_dialog_closing = true;

    // Store reference to avoid using freed memory
    text_input_dialog_t *dialog_to_close = current_text_dialog;
    current_text_dialog = NULL;  // Clear global reference first

    /* Disconnect the keyboard from the textarea BEFORE deleting the modal.
     * If we delete the modal first, LVGL's keyboard destructor calls
     * lv_keyboard_set_textarea(kb, NULL) internally, which fires
     * LV_EVENT_DEFOCUSED on the target_textarea, causing LVGL's focus group
     * to shift focus to the next focusable object (the textarea that happens
     * to be sitting behind the OK button), which then fires LV_EVENT_FOCUSED
     * and reopens the dialog.  Disconnecting here under our guard flag stops
     * that chain before it starts. */
    if (dialog_to_close->keyboard && lv_obj_is_valid(dialog_to_close->keyboard)) {
        lv_keyboard_set_textarea(dialog_to_close->keyboard, NULL);
    }

    // Explicitly clear textarea focus before deleting the modal
    if (dialog_to_close->target_textarea && lv_obj_is_valid(dialog_to_close->target_textarea)) {
        lv_obj_clear_state(dialog_to_close->target_textarea, LV_STATE_FOCUSED);
    }

    // Delete modal and all its children (keyboard, dialog container, etc.)
    if (dialog_to_close->modal && lv_obj_is_valid(dialog_to_close->modal)) {
        lv_obj_del(dialog_to_close->modal);
    }
    
    // Free the original text if it was allocated
    if (dialog_to_close->current_text) {
        lv_mem_free(dialog_to_close->current_text);
    }
    
    // Free the dialog structure
    lv_mem_free(dialog_to_close);

    s_dialog_close_tick = lv_tick_get();  /* start cooldown window */
    s_dialog_closing = false;
}

// Force close any active text input dialog - useful for screen transitions
void force_close_text_input_dialog(void) {
    if (current_text_dialog) {
        close_text_input_dialog();
    }
}

// Wrapper function for backward compatibility
void show_text_input_dialog(lv_obj_t *target_textarea, const char *title, const char *placeholder,
                           void (*on_confirm)(const char *text, void *user_data),
                           void (*on_cancel)(void *user_data), void *user_data) {
    show_text_input_dialog_ex(target_textarea, title, placeholder, false, on_confirm, on_cancel, user_data);
}

// Updated keyboard_event_cb to use the new text input dialog
void keyboard_event_cb(lv_event_t * e) {
    lv_obj_t * obj = lv_event_get_target(e);
    lv_event_code_t code = lv_event_get_code(e);

    if(code == LV_EVENT_FOCUSED) {
        if(obj == NULL) return;

        /* Block all re-open paths for a short window after any dialog closes.
         * This covers focus-cascade, touch-through, and LVGL group cycling
         * regardless of which textarea ends up being spuriously focused.
         * 200ms is well below deliberate human tap speed but well above any
         * LVGL-internal event cascade. */
        if (s_dialog_closing) return;
        if (current_text_dialog) return;
        if (lv_tick_elaps(s_dialog_close_tick) < DIALOG_REOPEN_COOLDOWN_MS) return;
        
        // Check if we're on the WiFi screen - if so, skip our custom dialog
        // The WiFi screen handles its own keyboard with password_input_event_cb
        if (is_wifi_screen_active()) {
            return;
        }
        
        // For all other screens, show our custom text input dialog with context detection
        const char *title = "Enter Text";
        const char *placeholder = "Type here...";
        bool show_prefix = false;
        bool numeric_only = false;
        
        // Detect field type based on placeholder or position
        const char *existing_placeholder = lv_textarea_get_placeholder_text(obj);
        if (existing_placeholder) {
            if (strstr(existing_placeholder, "CAN ID") || strstr(existing_placeholder, "Enter CAN ID")) {
                title = "CAN ID:";
                placeholder = "530";
                show_prefix = true;
            } else if (strstr(existing_placeholder, "Label") || strstr(existing_placeholder, "Enter Label")) {
                title = "Label:";
                placeholder = "Enter Label";
            } else if (strstr(existing_placeholder, "Scale") || strstr(existing_placeholder, "Enter Scale")) {
                title = "Scale:";
                placeholder = "1.0";
                numeric_only = true;
            } else if (strstr(existing_placeholder, "Offset") || strstr(existing_placeholder, "Enter Offset")) {
                title = "Offset:";
                placeholder = "0.0";
                numeric_only = true;
            } else if (strstr(existing_placeholder, "Range Low")) {
                title = "Range Low:";
                placeholder = "0.0";
                numeric_only = true;
            } else if (strstr(existing_placeholder, "Range High")) {
                title = "Range High:";
                placeholder = "100.0";
                numeric_only = true;
            } else if (strstr(existing_placeholder, "Min Value")) {
                title = "Min Value:";
                placeholder = "0.0";
                numeric_only = true;
            } else if (strstr(existing_placeholder, "Max Value")) {
                title = "Max Value:";
                placeholder = "100.0";
                numeric_only = true;
            } else if (strstr(existing_placeholder, "Low Value")) {
                title = "Low Value:";
                placeholder = "0.0";
                numeric_only = true;
            } else if (strstr(existing_placeholder, "High Value")) {
                title = "High Value:";
                placeholder = "100.0";
                numeric_only = true;
            } else if (strstr(existing_placeholder, "Empty V")) {
                title = "Empty Voltage:";
                placeholder = "0.00";
                numeric_only = true;
            } else if (strstr(existing_placeholder, "Full V")) {
                title = "Full Voltage:";
                placeholder = "3.30";
                numeric_only = true;
            }
        }
        
        show_text_input_dialog_ex(obj, title, placeholder, show_prefix, NULL, NULL, NULL);
        
        // If we opened a dialog and know this is a numeric field, switch keyboard to number mode
        if (!is_wifi_screen_active() && numeric_only && current_text_dialog && current_text_dialog->keyboard) {
            lv_keyboard_set_mode(current_text_dialog->keyboard, LV_KEYBOARD_MODE_NUMBER);
        }
        
    } else if(code == LV_EVENT_DEFOCUSED) {
        if (is_wifi_screen_active()) {
            return;
        }

        /* Do NOT close the dialog here if one is currently open or being built.
         * When the dialog's keyboard widget is created, LVGL internally shifts
         * focus away from the textarea (the keyboard claims it), firing
         * DEFOCUSED on the textarea.  Closing here would destroy the dialog
         * that was just opened and restart the cooldown, making the textarea
         * appear unresponsive.  The modal ensures OK/Cancel are the only valid
         * close paths; force_close_text_input_dialog() covers screen switches. */
        if (current_text_dialog || s_dialog_closing) {
            return;
        }
    }
}

void free_value_id_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        uint8_t * p_id = (uint8_t *)lv_event_get_user_data(e);
        if (p_id) {
            lv_mem_free(p_id);  // Only free if it's dynamically allocated
        }
    }
}


void endianess_roller_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dropdown = lv_event_get_target(e);
        uint8_t selected = lv_dropdown_get_selected(dropdown);

        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
        if (value_id < 1 || value_id > 13) return;

        // Update the configuration
        values_config[value_id - 1].endianess = (selected == 0) ? BIG_ENDIAN_ORDER : LITTLE_ENDIAN_ORDER;

        printf("Updated Endianess for Value #%d to %s\n",
               value_id,
               (selected == 0) ? "Big Endian" : "Little Endian");
        print_value_config(value_id);
    }
}


void value_offset_input_event_cb(lv_event_t * e) {
    if(lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * textarea = lv_event_get_target(e);
        const char * txt = lv_textarea_get_text(textarea);

        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
        if(value_id < 1 || value_id > 13) return;

        strncpy(value_offset_texts[value_id -1], txt, sizeof(value_offset_texts[value_id -1]));
        value_offset_texts[value_id -1][sizeof(value_offset_texts[value_id -1]) - 1] = '\0'; 

        float entered_offset = atof(txt);
        values_config[value_id - 1].value_offset = entered_offset;

        printf("Updated Value Offset for Value #%d to %f\n", value_id, entered_offset);
        print_value_config(value_id);
    }
}


void can_id_input_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * textarea = lv_event_get_target(e);
        const char * txt = lv_textarea_get_text(textarea);

        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
        if(value_id < 1 || value_id > 13) return;

        // 1) Build a "0x..." string from what the user typed
        //    If user typed "0x123", skip re-appending "0x"
        char buffer[32];
        if(txt != NULL && txt[0] != '\0') {
            if((txt[0] == '0') && (txt[1] == 'x' || txt[1] == 'X')) {
                // Already starts with 0x, copy as-is
                snprintf(buffer, sizeof(buffer), "%s", txt);
            } else {
                // Prepend 0x
                snprintf(buffer, sizeof(buffer), "0x%s", txt);
            }
        } else {
            // Empty text means ID=0
            snprintf(buffer, sizeof(buffer), "0x0");
        }

        // 2) Convert to number, always as hex
        uint32_t entered_can_id = strtoul(buffer, NULL, 16);

        // 3) Store the numeric ID in your config
        values_config[value_id - 1].can_id = entered_can_id;
        values_config[value_id - 1].enabled = (entered_can_id != 0);

        printf("Updated CAN ID for Value #%d to 0x%X (decimal: %u)\n",
               value_id, entered_can_id, entered_can_id);
        print_value_config(value_id);
    }
}


void scale_input_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * textarea = lv_event_get_target(e);
        const char * txt = lv_textarea_get_text(textarea);

        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
        if(value_id < 1 || value_id > 13) return;

        float entered_scale = atof(txt);
        if (entered_scale == 0.0f) {
            entered_scale = 1.0f;
        }
        values_config[value_id - 1].scale = entered_scale;
        printf("Updated Scale for Value #%d to %f\n", value_id, entered_scale);
        print_value_config(value_id);
    }
}

void type_dropdown_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * dropdown = lv_event_get_target(e);
        uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
        if(value_id < 1 || value_id > 13) return;

        uint8_t selected = lv_dropdown_get_selected(dropdown);
        values_config[value_id - 1].is_signed = (selected == 1);

        printf("Updated Type for Value #%d to %s\n", value_id, 
               values_config[value_id - 1].is_signed ? "Signed" : "Unsigned");
        print_value_config(value_id);
    }
}

/* =========================================================================
 * Speed-units callback
 * ========================================================================= */
void speed_units_dropdown_event_cb(lv_event_t *e)
{
    lv_obj_t *dd = lv_event_get_target(e);
    uint16_t sel = lv_dropdown_get_selected(dd);
    values_config[SPEED_VALUE_ID - 1].use_mph = (sel == 1);
    if (ui_Kmh) lv_label_set_text(ui_Kmh, values_config[SPEED_VALUE_ID - 1].use_mph ? "mph" : "k/mh");
    if (menu_speed_units_label && lv_obj_is_valid(menu_speed_units_label))
        lv_label_set_text(menu_speed_units_label, values_config[SPEED_VALUE_ID - 1].use_mph ? "mph" : "k/mh");
    config_store_save_values(values_config, 13);
    ESP_LOGI("MENU", "Speed units: %s", values_config[SPEED_VALUE_ID - 1].use_mph ? "MPH" : "KMH");
}

/* =========================================================================
 * Bar show/invert callbacks
 * ========================================================================= */
void show_value_switch_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    uint8_t vid = *(uint8_t *)lv_event_get_user_data(e);
    bool show = lv_obj_has_state(sw, LV_STATE_CHECKED);
    values_config[vid - 1].show_bar_value = show;
    lv_obj_t *target = (vid == BAR1_VALUE_ID) ? ui_Bar_1_Value : ui_Bar_2_Value;
    if (target && lv_obj_is_valid(target)) {
        if (show) lv_obj_clear_flag(target, LV_OBJ_FLAG_HIDDEN);
        else       lv_obj_add_flag(target,   LV_OBJ_FLAG_HIDDEN);
    }
    config_store_save_values(values_config, 13);
    ESP_LOGI("BAR", "Show value %s for bar %d", show ? "on" : "off", vid);
}

void invert_value_switch_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    uint8_t vid = *(uint8_t *)lv_event_get_user_data(e);
    bool inv = lv_obj_has_state(sw, LV_STATE_CHECKED);
    values_config[vid - 1].invert_bar_value = inv;
    config_store_save_values(values_config, 13);
    ESP_LOGI("BAR", "Invert %s for bar %d", inv ? "on" : "off", vid);
}

/* =========================================================================
 * Custom text (display unit) callback
 * ========================================================================= */
void custom_text_input_event_cb(lv_event_t *e)
{
    uint8_t *id_ptr = (uint8_t *)lv_event_get_user_data(e);
    if (!id_ptr) return;
    uint8_t vid = *id_ptr;
    const char *text = lv_textarea_get_text(lv_event_get_target(e));
    if (text) {
        strncpy(values_config[vid - 1].custom_text, text, sizeof(values_config[vid - 1].custom_text) - 1);
        values_config[vid - 1].custom_text[sizeof(values_config[vid - 1].custom_text) - 1] = '\0';
    } else {
        values_config[vid - 1].custom_text[0] = '\0';
    }
    uint8_t panel_idx = vid - 1;
    if (panel_idx < 8 && ui_CustomText[panel_idx] && lv_obj_is_valid(ui_CustomText[panel_idx])) {
        lv_label_set_text(ui_CustomText[panel_idx], values_config[panel_idx].custom_text);
        if (strlen(values_config[panel_idx].custom_text) == 0)
            lv_obj_add_flag(ui_CustomText[panel_idx], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(ui_CustomText[panel_idx], LV_OBJ_FLAG_HIDDEN);
    }
    config_store_save_values(values_config, 13);
    ESP_LOGI("PANEL", "Custom text '%s' for panel %d", values_config[vid - 1].custom_text, vid);
}

/* =========================================================================
 * Fuel sender voltage-input callbacks
 * ========================================================================= */
void fs_empty_v_input_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint8_t *id = (uint8_t *)lv_event_get_user_data(e);
    if (!id) return;
    float v = atof(lv_textarea_get_text(lv_event_get_target(e)));
    v = (v < 0.0f) ? 0.0f : (v > 3.3f) ? 3.3f : v;
    values_config[*id - 1].fuel_sender_empty_v = v;
    config_store_save_values(values_config, 13);
}

void fs_full_v_input_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    uint8_t *id = (uint8_t *)lv_event_get_user_data(e);
    if (!id) return;
    float v = atof(lv_textarea_get_text(lv_event_get_target(e)));
    v = (v < 0.0f) ? 0.0f : (v > 3.3f) ? 3.3f : v;
    values_config[*id - 1].fuel_sender_full_v = v;
    config_store_save_values(values_config, 13);
}

/* =========================================================================
 * Fuel sender calibration-button callbacks
 * ========================================================================= */
void fs_empty_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint8_t *id = (uint8_t *)lv_event_get_user_data(e);
    if (!id) return;
    fuel_sender_capture_empty(*id);
    char vbuf[12];
    snprintf(vbuf, sizeof(vbuf), "%.2f", values_config[*id - 1].fuel_sender_empty_v);
    lv_obj_t *screen = lv_obj_get_screen(lv_event_get_target(e));
    uint32_t cnt = lv_obj_get_child_cnt(screen);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *obj = lv_obj_get_child(screen, i);
        if (lv_obj_check_type(obj, &lv_textarea_class)) {
            const char *ph = lv_textarea_get_placeholder_text(obj);
            if (ph && strstr(ph, "Empty V")) { lv_textarea_set_text(obj, vbuf); break; }
        }
    }
}

void fs_full_btn_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    uint8_t *id = (uint8_t *)lv_event_get_user_data(e);
    if (!id) return;
    fuel_sender_capture_full(*id);
    char vbuf[12];
    snprintf(vbuf, sizeof(vbuf), "%.2f", values_config[*id - 1].fuel_sender_full_v);
    lv_obj_t *screen = lv_obj_get_screen(lv_event_get_target(e));
    uint32_t cnt = lv_obj_get_child_cnt(screen);
    for (uint32_t i = 0; i < cnt; i++) {
        lv_obj_t *obj = lv_obj_get_child(screen, i);
        if (lv_obj_check_type(obj, &lv_textarea_class)) {
            const char *ph = lv_textarea_get_placeholder_text(obj);
            if (ph && strstr(ph, "Full V")) { lv_textarea_set_text(obj, vbuf); break; }
        }
    }
}

/* =========================================================================
 * Fuel sender context callbacks
 * ========================================================================= */
void fuel_sender_ctx_free_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_DELETE) return;
    fuel_sender_ctx_t *ctx = (fuel_sender_ctx_t *)lv_event_get_user_data(e);
    if (ctx) {
        if (ctx->update_timer) { lv_timer_del(ctx->update_timer); ctx->update_timer = NULL; }
        lv_mem_free(ctx);
    }
}

void fs_voltage_update_timer_cb(lv_timer_t *timer)
{
    fuel_sender_ctx_t *ctx = (fuel_sender_ctx_t *)timer->user_data;
    if (!ctx) return;
    if (!ctx->current_label || !lv_obj_is_valid(ctx->current_label)) return;
    if (lv_obj_has_flag(ctx->current_label, LV_OBJ_FLAG_HIDDEN)) return;
    uint8_t bar_idx = (ctx->value_id == 12) ? 0 : 1;
    char vbuf[24];
    snprintf(vbuf, sizeof(vbuf), "Current: %.2f V", fuel_sender_get_filtered_v(bar_idx));
    lv_label_set_text(ctx->current_label, vbuf);
}

void fs_filter_slider_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) return;
    fuel_sender_ctx_t *ctx = (fuel_sender_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    int32_t val = lv_slider_get_value(lv_event_get_target(e));
    values_config[ctx->value_id - 1].fuel_sender_filter = (uint8_t)val;
    char fbuf[24];
    snprintf(fbuf, sizeof(fbuf), "Filter: %d%%", (int)val);
    lv_label_set_text(ctx->filter_label, fbuf);
    if (code == LV_EVENT_RELEASED) config_store_save_values(values_config, 13);
}

void fuel_sender_switch_event_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    fuel_sender_ctx_t *ctx = (fuel_sender_ctx_t *)lv_event_get_user_data(e);
    if (!ctx) return;
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    values_config[ctx->value_id - 1].fuel_sender = on;
#define FS_SHOW(o) if ((o) && lv_obj_is_valid(o)) lv_obj_clear_flag((o), LV_OBJ_FLAG_HIDDEN)
#define FS_HIDE(o) if ((o) && lv_obj_is_valid(o)) lv_obj_add_flag((o),   LV_OBJ_FLAG_HIDDEN)
    if (on) {
        FS_SHOW(ctx->set_label);   FS_SHOW(ctx->empty_btn);  FS_SHOW(ctx->full_btn);
        FS_SHOW(ctx->empty_input); FS_SHOW(ctx->full_input); FS_SHOW(ctx->current_label);
        FS_SHOW(ctx->filter_label); FS_SHOW(ctx->filter_slider);
    } else {
        FS_HIDE(ctx->set_label);   FS_HIDE(ctx->empty_btn);  FS_HIDE(ctx->full_btn);
        FS_HIDE(ctx->empty_input); FS_HIDE(ctx->full_input); FS_HIDE(ctx->current_label);
        FS_HIDE(ctx->filter_label); FS_HIDE(ctx->filter_slider);
    }
#undef FS_SHOW
#undef FS_HIDE
    config_store_save_values(values_config, 13);
    ESP_LOGI("BAR", "Fuel sender %s for bar %d", on ? "on" : "off", ctx->value_id);
}


