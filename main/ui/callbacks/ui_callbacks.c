#include "ui_callbacks.h"
#include "../theme.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "lvgl.h"
#include "../screens/ui_Screen3.h"
#include "../screens/ui_wifi.h"
#include "../ui.h"

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
    if (wifi_ui_is_active()) {
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
    
    // Create dialog container — flex column for clean vertical stacking
    lv_obj_t *dialog_container = lv_obj_create(current_text_dialog->modal);
    lv_obj_set_size(dialog_container, 420, 170);
    lv_obj_set_style_bg_color(dialog_container, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(dialog_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dialog_container, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(dialog_container, 1, 0);
    lv_obj_set_style_radius(dialog_container, THEME_RADIUS_LARGE, 0);
    lv_obj_set_style_shadow_width(dialog_container, 12, 0);
    lv_obj_set_style_shadow_ofs_y(dialog_container, 4, 0);
    lv_obj_set_style_shadow_color(dialog_container, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(dialog_container, 100, 0);
    lv_obj_align(dialog_container, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_clear_flag(dialog_container, LV_OBJ_FLAG_SCROLLABLE);

    // Title label
    if (title) {
        lv_obj_t *title_label = lv_label_create(dialog_container);
        lv_label_set_text(title_label, title);
        lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM, 0);
        lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 10);
    }

    // Create prefix label if needed (for CAN ID: 0x)
    if (show_prefix) {
        current_text_dialog->prefix_label = lv_label_create(dialog_container);
        lv_label_set_text(current_text_dialog->prefix_label, "0x");
        lv_obj_set_style_text_color(current_text_dialog->prefix_label, THEME_COLOR_TEXT_MUTED, 0);
        lv_obj_set_style_text_font(current_text_dialog->prefix_label, THEME_FONT_BODY, 0);
        lv_obj_align(current_text_dialog->prefix_label, LV_ALIGN_TOP_LEFT, 42, 48);
    } else {
        current_text_dialog->prefix_label = NULL;
    }

    // Text display area — dark input matching settings panel inputs
    current_text_dialog->text_display = lv_label_create(dialog_container);
    int display_width = show_prefix ? 280 : 340;
    int display_x = show_prefix ? 18 : 0;
    lv_obj_set_size(current_text_dialog->text_display, display_width, 34);
    lv_obj_set_style_bg_color(current_text_dialog->text_display, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(current_text_dialog->text_display, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(current_text_dialog->text_display, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_border_width(current_text_dialog->text_display, 2, 0);
    lv_obj_set_style_radius(current_text_dialog->text_display, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_text_color(current_text_dialog->text_display, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(current_text_dialog->text_display, THEME_FONT_BODY, 0);
    lv_obj_set_style_pad_left(current_text_dialog->text_display, 8, 0);
    lv_obj_set_style_pad_right(current_text_dialog->text_display, 8, 0);
    lv_obj_set_style_pad_top(current_text_dialog->text_display, 7, 0);
    lv_obj_set_style_pad_bottom(current_text_dialog->text_display, 7, 0);
    lv_obj_align(current_text_dialog->text_display, LV_ALIGN_TOP_MID, display_x, 40);
    lv_label_set_long_mode(current_text_dialog->text_display, LV_LABEL_LONG_SCROLL_CIRCULAR);

    // Set initial text or placeholder
    if (target_textarea) {
        const char *current_text = lv_textarea_get_text(target_textarea);
        if (current_text && strlen(current_text) > 0) {
            lv_label_set_text(current_text_dialog->text_display, current_text);
        } else if (placeholder) {
            lv_label_set_text(current_text_dialog->text_display, placeholder);
            lv_obj_set_style_text_color(current_text_dialog->text_display, THEME_COLOR_TEXT_GHOST, 0);
        }
    }

    // Button container
    lv_obj_t *btn_container = lv_obj_create(dialog_container);
    lv_obj_set_size(btn_container, 340, 40);
    lv_obj_set_style_bg_opa(btn_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_container, 0, 0);
    lv_obj_set_style_pad_all(btn_container, 0, 0);
    lv_obj_set_style_pad_column(btn_container, 12, 0);
    lv_obj_align(btn_container, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_flex_flow(btn_container, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(btn_container, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_flex_align(btn_container, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_container, LV_OBJ_FLAG_SCROLLABLE);

    // Cancel button — secondary style
    lv_obj_t *cancel_btn = lv_btn_create(btn_container);
    lv_obj_set_size(cancel_btn, 140, 34);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_border_color(cancel_btn, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(cancel_btn, 1, 0);
    lv_obj_set_style_radius(cancel_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_shadow_width(cancel_btn, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_SCROLLBAR, LV_STATE_PRESSED);
    lv_obj_add_event_cb(cancel_btn, text_input_cancel_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_label = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_set_style_text_color(cancel_label, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(cancel_label, THEME_FONT_SMALL, 0);
    lv_obj_center(cancel_label);

    // OK button — primary accent style
    lv_obj_t *ok_btn = lv_btn_create(btn_container);
    lv_obj_set_size(ok_btn, 140, 34);
    lv_obj_set_style_bg_color(ok_btn, THEME_COLOR_BTN_SAVE, 0);
    lv_obj_set_style_bg_color(ok_btn, THEME_COLOR_BTN_SAVE_PRESSED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(ok_btn, 0, 0);
    lv_obj_set_style_radius(ok_btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_shadow_width(ok_btn, 0, 0);
    lv_obj_add_event_cb(ok_btn, text_input_ok_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ok_label = lv_label_create(ok_btn);
    lv_label_set_text(ok_label, LV_SYMBOL_OK "  OK");
    lv_obj_set_style_text_color(ok_label, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_set_style_text_font(ok_label, THEME_FONT_SMALL, 0);
    lv_obj_center(ok_label);
    
    // Create keyboard — styled to match dark theme
    current_text_dialog->keyboard = lv_keyboard_create(current_text_dialog->modal);
    lv_obj_set_style_bg_color(current_text_dialog->keyboard, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(current_text_dialog->keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(current_text_dialog->keyboard, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(current_text_dialog->keyboard, 1, 0);
    lv_obj_set_style_border_side(current_text_dialog->keyboard, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(current_text_dialog->keyboard, 4, 0);
    lv_obj_set_style_pad_gap(current_text_dialog->keyboard, 4, 0);
    lv_obj_align(current_text_dialog->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    /* Key items — dark bg, light text, rounded */
    lv_obj_set_style_bg_color(current_text_dialog->keyboard, THEME_COLOR_SECTION_BG, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(current_text_dialog->keyboard, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(current_text_dialog->keyboard, THEME_COLOR_TEXT_PRIMARY, LV_PART_ITEMS);
    lv_obj_set_style_text_font(current_text_dialog->keyboard, THEME_FONT_BODY, LV_PART_ITEMS);
    lv_obj_set_style_border_color(current_text_dialog->keyboard, THEME_COLOR_BORDER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(current_text_dialog->keyboard, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(current_text_dialog->keyboard, THEME_RADIUS_NORMAL, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(current_text_dialog->keyboard, 0, LV_PART_ITEMS);
    /* Key pressed state */
    lv_obj_set_style_bg_color(current_text_dialog->keyboard, THEME_COLOR_ACCENT_BLUE, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(current_text_dialog->keyboard, THEME_COLOR_TEXT_ON_ACCENT, LV_PART_ITEMS | LV_STATE_PRESSED);
    
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

/* Numeric input convenience wrapper — see ui_callbacks.h.
 *
 * The dialog requires a target textarea (it copies the initial text from
 * lv_textarea_get_text and writes results back on confirm). We keep a single
 * hidden helper textarea, recreated lazily if a screen swap invalidated it.
 * Lives as a child of the active screen so it's cleaned up automatically on
 * screen change. */
void show_numeric_input_dialog(const char *title, const char *initial,
                               void (*on_confirm)(const char *text, void *user_data),
                               void (*on_cancel)(void *user_data),
                               void *user_data) {
    static lv_obj_t *helper_ta = NULL;
    if (!helper_ta || !lv_obj_is_valid(helper_ta)) {
        helper_ta = lv_textarea_create(lv_scr_act());
        if (!helper_ta) return;
        lv_obj_add_flag(helper_ta, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(helper_ta, 1, 1);
    }
    lv_textarea_set_text(helper_ta, initial ? initial : "");

    show_text_input_dialog_ex(helper_ta, title, NULL, false,
                              on_confirm, on_cancel, user_data);

    /* Flip the dialog's keyboard into number mode so the user gets a numeric
     * keypad immediately. current_text_dialog is the module-static set up
     * during the call above. */
    if (current_text_dialog && current_text_dialog->keyboard) {
        lv_keyboard_set_mode(current_text_dialog->keyboard, LV_KEYBOARD_MODE_NUMBER);
    }
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
        if (wifi_ui_is_active()) {
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
        if (!wifi_ui_is_active() && numeric_only && current_text_dialog && current_text_dialog->keyboard) {
            lv_keyboard_set_mode(current_text_dialog->keyboard, LV_KEYBOARD_MODE_NUMBER);
        }
        
    } else if(code == LV_EVENT_DEFOCUSED) {
        if (wifi_ui_is_active()) {
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


