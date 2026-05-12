#ifndef UI_CALLBACKS_H
#define UI_CALLBACKS_H

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Keyboard focus/defocus handler — opens the text input dialog on focus */
void keyboard_event_cb(lv_event_t * e);

/* Text Input Dialog System */
typedef struct {
    lv_obj_t *modal;
    lv_obj_t *text_display;
    lv_obj_t *prefix_label;
    lv_obj_t *keyboard;
    lv_obj_t *target_textarea;
    char *current_text;
    size_t max_length;
    void (*on_confirm)(const char *text, void *user_data);
    void (*on_cancel)(void *user_data);
    void *user_data;
} text_input_dialog_t;

void show_text_input_dialog(lv_obj_t *target_textarea, const char *title, const char *placeholder,
                           void (*on_confirm)(const char *text, void *user_data),
                           void (*on_cancel)(void *user_data), void *user_data);
void show_text_input_dialog_ex(lv_obj_t *target_textarea, const char *title, const char *placeholder, bool show_prefix,
                              void (*on_confirm)(const char *text, void *user_data),
                              void (*on_cancel)(void *user_data), void *user_data);
void close_text_input_dialog(void);
void force_close_text_input_dialog(void);

/* Convenience wrapper around show_text_input_dialog_ex that:
 *   - sets the keyboard to LV_KEYBOARD_MODE_NUMBER after opening so the user
 *     gets a numeric keypad immediately (no symbols-toggle dance), and
 *   - manages a hidden helper textarea internally so callers don't need to
 *     keep one around just to satisfy the dialog's target_textarea argument.
 *
 * `initial` is the pre-filled value displayed when the dialog opens.
 * `on_confirm` fires when the user taps Save with whatever they typed.
 * `on_cancel` fires when the user taps Cancel (or dismisses without
 *   confirming). Either callback may be NULL. */
void show_numeric_input_dialog(const char *title, const char *initial,
                               void (*on_confirm)(const char *text, void *user_data),
                               void (*on_cancel)(void *user_data),
                               void *user_data);

#ifdef __cplusplus
}
#endif

#endif // UI_CALLBACKS_H
