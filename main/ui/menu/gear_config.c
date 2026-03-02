#include "gear_config.h"
#include "../theme.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "lvgl.h"
#include "../screens/ui_Screen3.h"
#include "preset_picker.h"
#include "../callbacks/ui_callbacks.h"
#include "esp_log.h"
#include "storage/config_store.h"

extern const lv_img_dsc_t Smart_Car_Key;
extern value_config_t values_config[];

#define GEAR_VALUE_ID 11

static lv_obj_t *custom_gear_values_container       = NULL;
static lv_obj_t *custom_gear_value_inputs[14]       = {NULL};
static lv_obj_t *custom_icon_inputs[7]              = {NULL};
static lv_obj_t *custom_icon_type_dropdowns[7]      = {NULL};
static lv_obj_t *custom_icon_images[7]              = {NULL};

/* =========================================================================
 * Helpers
 * ========================================================================= */

static uint32_t parse_hex_or_dec(const char *s)
{
    if (!s || !*s) return UINT32_MAX;
    if (strncmp(s, "0x", 2) == 0 || strncmp(s, "0X", 2) == 0)
        return strtoul(s, NULL, 16);
    return strtoul(s, NULL, 10);
}

/* =========================================================================
 * Callbacks (defined here, declared extern in menu_screen.h)
 * ========================================================================= */

static void custom_icon_type_dropdown_event_cb(lv_event_t *e)
{
    lv_obj_t *dropdown = lv_event_get_target(e);
    int icon_index = (int)(intptr_t)lv_event_get_user_data(e);
    if (icon_index < 0 || icon_index >= 7) return;

    uint16_t selected = lv_dropdown_get_selected(dropdown);
    values_config[GEAR_VALUE_ID - 1].custom_icon_types[icon_index] = selected;

    if (selected == 0)
        values_config[GEAR_VALUE_ID - 1].custom_icon_values[icon_index] = UINT32_MAX;

    if (custom_icon_images[icon_index] && lv_obj_is_valid(custom_icon_images[icon_index])) {
        if (selected == 1)
            lv_obj_clear_flag(custom_icon_images[icon_index], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(custom_icon_images[icon_index], LV_OBJ_FLAG_HIDDEN);

        if (selected == 0 && custom_icon_inputs[icon_index] &&
            lv_obj_is_valid(custom_icon_inputs[icon_index]))
            lv_textarea_set_text(custom_icon_inputs[icon_index], "");
    }
    config_store_save_values(values_config, 13);
    ESP_LOGI("GEAR", "Custom icon %d type: %d", icon_index, selected);
}

static void custom_icon_input_event_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    int idx = -1;
    for (int i = 0; i < 7; i++) {
        if (custom_icon_inputs[i] == ta) { idx = i; break; }
    }
    if (idx < 0) return;
    uint32_t v = parse_hex_or_dec(lv_textarea_get_text(ta));
    values_config[GEAR_VALUE_ID - 1].custom_icon_values[idx] = v;
    config_store_save_values(values_config, 13);
    ESP_LOGI("GEAR", "Custom icon %d value: %u", idx, v);
}

static void custom_gear_value_input_event_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    int idx = -1;
    for (int i = 0; i < 14; i++) {
        if (custom_gear_value_inputs[i] == ta) { idx = i; break; }
    }
    if (idx < 0) return;
    uint32_t v = parse_hex_or_dec(lv_textarea_get_text(ta));
    values_config[GEAR_VALUE_ID - 1].gear_custom_values[idx] = v;
    config_store_save_values(values_config, 13);
    const char *names[] = {"P","R","N","D","1","2","3","4","5","6","7","8","9","10"};
    ESP_LOGI("GEAR", "Gear %s = %u", names[idx], v);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void custom_gear_section_flush_to_config(void)
{
    for (int i = 0; i < 14; i++) {
        if (custom_gear_value_inputs[i] && lv_obj_is_valid(custom_gear_value_inputs[i]))
            values_config[GEAR_VALUE_ID - 1].gear_custom_values[i] =
                parse_hex_or_dec(lv_textarea_get_text(custom_gear_value_inputs[i]));
    }
    for (int i = 0; i < 7; i++) {
        if (custom_icon_inputs[i] && lv_obj_is_valid(custom_icon_inputs[i]))
            values_config[GEAR_VALUE_ID - 1].custom_icon_values[i] =
                parse_hex_or_dec(lv_textarea_get_text(custom_icon_inputs[i]));
    }
}

static void clear_refs(void)
{
    custom_gear_values_container = NULL;
    for (int i = 0; i < 14; i++) custom_gear_value_inputs[i] = NULL;
    for (int i = 0; i < 7;  i++) {
        custom_icon_inputs[i] = NULL;
        custom_icon_type_dropdowns[i] = NULL;
        custom_icon_images[i] = NULL;
    }
}

void hide_custom_gear_values_section(void)
{
    if (custom_gear_values_container && lv_obj_is_valid(custom_gear_values_container))
        lv_obj_del(custom_gear_values_container);
    clear_refs();
}

void create_custom_gear_values_section(lv_obj_t *parent, uint8_t gear_mode)
{
    (void)gear_mode;

    if (custom_gear_values_container) {
        if (lv_obj_is_valid(custom_gear_values_container)) {
            if (lv_obj_get_parent(custom_gear_values_container) == parent) {
                lv_obj_clear_flag(custom_gear_values_container, LV_OBJ_FLAG_HIDDEN);
                return;
            }
            lv_obj_del(custom_gear_values_container);
        }
        clear_refs();
    }

    custom_gear_values_container = lv_obj_create(parent);
    lv_obj_set_size(custom_gear_values_container, 500, 330);
    lv_obj_align(custom_gear_values_container, LV_ALIGN_CENTER, 110, 75);
    lv_obj_set_style_bg_color(custom_gear_values_container, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(custom_gear_values_container, 0, 0);
    lv_obj_set_style_border_width(custom_gear_values_container, 0, 0);
    lv_obj_set_style_radius(custom_gear_values_container, 7, 0);
    lv_obj_set_style_pad_all(custom_gear_values_container, 15, 0);
    lv_obj_clear_flag(custom_gear_values_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(custom_gear_values_container, LV_OBJ_FLAG_CLICKABLE);

    const char *gear_labels[] = {"P","R","N","D","1","2","3","4","5","6","7","8","9","10"};
    int col1_x = 20, col2_x = 173, col3_x = 326;
    lv_style_t *cs = get_common_style();

    for (int i = 0; i < 14; i++) {
        int col = (i < 7) ? 0 : 1;
        int row = (i < 7) ? i : i - 7;
        int x_pos = (col == 0) ? col1_x : col2_x;
        int y_pos = 25 + row * 30;

        char lbl_text[8];
        snprintf(lbl_text, sizeof(lbl_text), "%s =", gear_labels[i]);
        lv_obj_t *gl = lv_label_create(custom_gear_values_container);
        lv_label_set_text(gl, lbl_text);
        lv_obj_set_style_text_color(gl, THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_font(gl, THEME_FONT_SMALL, 0);
        lv_obj_set_pos(gl, x_pos, y_pos);

        custom_gear_value_inputs[i] = lv_textarea_create(custom_gear_values_container);
        lv_textarea_set_one_line(custom_gear_value_inputs[i], true);
        lv_textarea_set_max_length(custom_gear_value_inputs[i], 8);
        lv_obj_set_size(custom_gear_value_inputs[i], 60, 25);
        lv_obj_set_pos(custom_gear_value_inputs[i], x_pos + 30, y_pos - 2);
        lv_obj_clear_flag(custom_gear_value_inputs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(custom_gear_value_inputs[i], cs, LV_PART_MAIN);

        uint32_t gv = values_config[GEAR_VALUE_ID - 1].gear_custom_values[i];
        if (gv != UINT32_MAX) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", gv);
            lv_textarea_set_text(custom_gear_value_inputs[i], buf);
        } else {
            lv_textarea_set_text(custom_gear_value_inputs[i], "");
        }
        lv_obj_add_event_cb(custom_gear_value_inputs[i], custom_gear_value_input_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(custom_gear_value_inputs[i], custom_gear_value_input_event_cb, LV_EVENT_DEFOCUSED, NULL);
        lv_obj_add_event_cb(custom_gear_value_inputs[i], keyboard_event_cb, LV_EVENT_ALL, NULL);
    }

    for (int i = 0; i < 7; i++) {
        int y_pos = 25 + i * 30;

        custom_icon_type_dropdowns[i] = lv_dropdown_create(custom_gear_values_container);
        lv_dropdown_set_options(custom_icon_type_dropdowns[i], "None\nKEY");
        lv_obj_set_size(custom_icon_type_dropdowns[i], 60, 25);
        lv_obj_set_pos(custom_icon_type_dropdowns[i], col3_x, y_pos - 2);
        lv_obj_add_style(custom_icon_type_dropdowns[i], cs, LV_PART_MAIN);
        uint8_t icon_type = values_config[GEAR_VALUE_ID - 1].custom_icon_types[i];
        lv_dropdown_set_selected(custom_icon_type_dropdowns[i], icon_type);
        lv_obj_add_event_cb(custom_icon_type_dropdowns[i], custom_icon_type_dropdown_event_cb,
                            LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);

        custom_icon_images[i] = lv_img_create(custom_gear_values_container);
        lv_img_set_src(custom_icon_images[i], &Smart_Car_Key);
        lv_img_set_zoom(custom_icon_images[i], 120);
        lv_obj_set_size(custom_icon_images[i], LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_img_set_size_mode(custom_icon_images[i], LV_IMG_SIZE_MODE_REAL);
        lv_obj_set_pos(custom_icon_images[i], col3_x + 65, y_pos);
        if (icon_type != 1)
            lv_obj_add_flag(custom_icon_images[i], LV_OBJ_FLAG_HIDDEN);

        custom_icon_inputs[i] = lv_textarea_create(custom_gear_values_container);
        lv_textarea_set_one_line(custom_icon_inputs[i], true);
        lv_textarea_set_max_length(custom_icon_inputs[i], 8);
        lv_obj_set_size(custom_icon_inputs[i], 60, 25);
        lv_obj_set_pos(custom_icon_inputs[i], col3_x + 90, y_pos - 2);
        lv_obj_clear_flag(custom_icon_inputs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_style(custom_icon_inputs[i], cs, LV_PART_MAIN);

        uint32_t iv = values_config[GEAR_VALUE_ID - 1].custom_icon_values[i];
        if (iv != UINT32_MAX) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", iv);
            lv_textarea_set_text(custom_icon_inputs[i], buf);
        } else {
            lv_textarea_set_text(custom_icon_inputs[i], "");
        }
        lv_obj_add_event_cb(custom_icon_inputs[i], custom_icon_input_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(custom_icon_inputs[i], custom_icon_input_event_cb, LV_EVENT_DEFOCUSED, NULL);
        lv_obj_add_event_cb(custom_icon_inputs[i], keyboard_event_cb, LV_EVENT_ALL, NULL);
    }
}
