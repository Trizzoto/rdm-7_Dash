#!/usr/bin/env python3
"""Phase 0F: Extract widget code from ui_Screen3.c into main/widgets/"""
import os, re

ROOT = r"c:\Users\ruuva\workspace\RDM-7_Dash\main"
SRC  = os.path.join(ROOT, "ui", "screens", "ui_Screen3.c")
WDIR = os.path.join(ROOT, "widgets")
os.makedirs(WDIR, exist_ok=True)

with open(SRC, "r", encoding="utf-8") as f:
    L = f.readlines()
N = len(L)
print(f"Read {N} lines from ui_Screen3.c")

def ext(start, end):
    """Extract lines start..end inclusive (1-based), returns string"""
    return "".join(L[start-1:end])

def deprivatize(code, names):
    """Remove 'static ' prefix from specified function definitions."""
    for name in names:
        code = re.sub(
            r'^(\s*)static\s+(\w[\w\s\*]*?\s+' + re.escape(name) + r'\s*\()',
            r'\1\2',
            code, flags=re.MULTILINE
        )
    return code

COMMON_HDR = """\
#include "ui/screens/ui_Screen3.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "can/can_decode.h"
#include "can/can_dispatch.h"
#include "storage/config_store.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/menu/menu_screen.h"
#include "ui/settings/device_settings.h"
#include "ui/settings/preset_picker.h"
#include "ui/callbacks/ui_callbacks.h"

"""

# ─────────────────────────────────────────────────────────────────────────────
# widget_warning.c
# ─────────────────────────────────────────────────────────────────────────────
warn_ranges = [
    (165, 184),   # warning_positions + state statics
    (209, 219),   # previous_bit_states + warning_save_data_t struct
    (2488, 2532), # threshold/color callbacks + warning_longpress_cb
    (2545, 2612), # label_text_cb, color_dropdown_cb
    (2613, 2827), # apply_preconfig_warning_cb, save_warning_config_cb
    (4270, 4318), # update_warning_ui + update_warning_ui_immediate
    (5563, 5979), # create_warning_config_menu, free_warning_idx, invert_warning_toggle
    (3369, 3374), # check_warning_timeouts stub
]

warn_code = '#include "widget_warning.h"\n'
warn_code += COMMON_HDR
warn_code += "/* forward declarations */\n"
warn_code += "static void create_warning_config_menu(uint8_t warning_idx);\n\n"
for s, e in warn_ranges:
    warn_code += ext(s, e)

warn_code = deprivatize(warn_code, [
    "update_warning_ui_immediate",
    "check_warning_timeouts",
])

warn_code += r"""
void widget_warning_create(lv_obj_t *parent)
{
    for (int i = 0; i < 8; i++) {
        warning_circles[i] = lv_obj_create(parent);
        lv_obj_set_width(warning_circles[i], 15);
        lv_obj_set_height(warning_circles[i], 15);
        lv_obj_set_x(warning_circles[i], warning_positions[i].x);
        lv_obj_set_y(warning_circles[i], warning_positions[i].y);
        lv_obj_set_align(warning_circles[i], LV_ALIGN_CENTER);
        lv_obj_clear_flag(warning_circles[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(warning_circles[i], 100, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(warning_circles[i], THEME_COLOR_INACTIVE, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(warning_circles[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(warning_circles[i], 0, LV_PART_MAIN | LV_STATE_DEFAULT);

        warning_labels[i] = lv_label_create(parent);
        lv_obj_set_width(warning_labels[i], LV_SIZE_CONTENT);
        lv_obj_set_height(warning_labels[i], LV_SIZE_CONTENT);
        lv_obj_set_x(warning_labels[i], warning_positions[i].x);
        lv_obj_set_y(warning_labels[i], -112);
        lv_obj_set_align(warning_labels[i], LV_ALIGN_CENTER);
        lv_obj_add_flag(warning_labels[i], LV_OBJ_FLAG_HIDDEN);
        const char *saved_label = warning_configs[i].label;
        if (saved_label && saved_label[0] != '\0') {
            lv_label_set_text(warning_labels[i], saved_label);
        } else {
            char label_text[20];
            snprintf(label_text, sizeof(label_text), "Warning\n%d", i + 1);
            lv_label_set_text(warning_labels[i], label_text);
        }
        lv_obj_set_style_text_color(warning_labels[i], THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(warning_labels[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(warning_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(warning_labels[i], THEME_FONT_TINY, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_obj_t *touch_area = lv_obj_create(parent);
        lv_obj_set_size(touch_area, 50, 80);
        lv_obj_set_x(touch_area, warning_positions[i].x);
        lv_obj_set_y(touch_area, warning_positions[i].y);
        lv_obj_set_align(touch_area, LV_ALIGN_CENTER);
        lv_obj_clear_flag(touch_area, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(touch_area, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(touch_area, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

        uint8_t *warning_id = lv_mem_alloc(sizeof(uint8_t));
        *warning_id = i;
        lv_obj_add_event_cb(touch_area, warning_longpress_cb, LV_EVENT_LONG_PRESSED, warning_id);

        update_warning_ui_immediate(i);
    }

    extern lv_obj_t *ui_Warning_1, *ui_Warning_2, *ui_Warning_3, *ui_Warning_4;
    extern lv_obj_t *ui_Warning_5, *ui_Warning_6, *ui_Warning_7, *ui_Warning_8;
    ui_Warning_1 = warning_circles[0]; ui_Warning_2 = warning_circles[1];
    ui_Warning_3 = warning_circles[2]; ui_Warning_4 = warning_circles[3];
    ui_Warning_5 = warning_circles[4]; ui_Warning_6 = warning_circles[5];
    ui_Warning_7 = warning_circles[6]; ui_Warning_8 = warning_circles[7];
}

void init_warning_configs(void)
{
    for (int i = 0; i < 8; i++) {
        warning_configs[i].can_id       = 0x000;
        warning_configs[i].bit_position = 0;
        warning_configs[i].endianess    = 1;
        warning_configs[i].active_color = THEME_COLOR_RED;
        char buf[32];
        snprintf(buf, sizeof(buf), "Warning %d", i + 1);
        strncpy(warning_configs[i].label, buf, sizeof(warning_configs[i].label) - 1);
        warning_configs[i].is_momentary  = true;
        warning_configs[i].current_state = false;
        warning_configs[i].invert_toggle = false;
    }
}
"""

with open(os.path.join(WDIR, "widget_warning.c"), "w", encoding="utf-8") as f:
    f.write(warn_code)
print("OK widget_warning.c")

# ─────────────────────────────────────────────────────────────────────────────
# widget_indicator.c
# ─────────────────────────────────────────────────────────────────────────────
ind_ranges = [
    (223, 230),   # indicator animation statics
    (2800, 2826), # indicator_save_data_t + preview statics + indicator_input_visibility_t
    (2533, 2544), # indicator_longpress_cb
    (2828, 2975), # indicator config callbacks
    (2976, 3368), # create_indicator_config_menu
    (4321, 4458), # update_indicator_ui_immediate, update_config_preview, indicator_apply_analog_state, update_indicator_ui
    (7682, 7717), # indicator_animation_timer_cb
]

ind_code = '#include "widget_indicator.h"\n'
ind_code += COMMON_HDR
ind_code += "static void update_indicator_ui_immediate(uint8_t indicator_idx);\n\n"
for s, e in ind_ranges:
    ind_code += ext(s, e)
ind_code = deprivatize(ind_code, [
    "update_indicator_ui_immediate",
    "indicator_longpress_cb",
])

ind_code += r"""
void widget_indicator_create(lv_obj_t *parent)
{
    ui_Indicator_Left = lv_img_create(parent);
    lv_img_set_src(ui_Indicator_Left, &ui_img_indicator_left_png);
    lv_obj_set_width(ui_Indicator_Left, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Indicator_Left, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Indicator_Left, -95);
    lv_obj_set_y(ui_Indicator_Left, -133);
    lv_obj_set_align(ui_Indicator_Left, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Indicator_Left, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_Indicator_Left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(ui_Indicator_Left, 50, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(ui_Indicator_Left, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Indicator_Right = lv_img_create(parent);
    lv_img_set_src(ui_Indicator_Right, &ui_img_indicator_right_png);
    lv_obj_set_width(ui_Indicator_Right, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Indicator_Right, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Indicator_Right, 95);
    lv_obj_set_y(ui_Indicator_Right, -133);
    lv_obj_set_align(ui_Indicator_Right, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_Indicator_Right, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(ui_Indicator_Right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_opa(ui_Indicator_Right, 50, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_outline_width(ui_Indicator_Right, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t *left_ta = lv_obj_create(parent);
    lv_obj_set_size(left_ta, 50, 50);
    lv_obj_set_x(left_ta, -95); lv_obj_set_y(left_ta, -133);
    lv_obj_set_align(left_ta, LV_ALIGN_CENTER);
    lv_obj_clear_flag(left_ta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(left_ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(left_ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    uint8_t *left_id = lv_mem_alloc(sizeof(uint8_t));
    if (left_id) { *left_id = 0; lv_obj_add_event_cb(left_ta, indicator_longpress_cb, LV_EVENT_LONG_PRESSED, left_id); }

    lv_obj_t *right_ta = lv_obj_create(parent);
    lv_obj_set_size(right_ta, 50, 50);
    lv_obj_set_x(right_ta, 95); lv_obj_set_y(right_ta, -133);
    lv_obj_set_align(right_ta, LV_ALIGN_CENTER);
    lv_obj_clear_flag(right_ta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(right_ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(right_ta, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    uint8_t *right_id = lv_mem_alloc(sizeof(uint8_t));
    if (right_id) { *right_id = 1; lv_obj_add_event_cb(right_ta, indicator_longpress_cb, LV_EVENT_LONG_PRESSED, right_id); }
}
"""

with open(os.path.join(WDIR, "widget_indicator.c"), "w", encoding="utf-8") as f:
    f.write(ind_code)
print("OK widget_indicator.c")

# ─────────────────────────────────────────────────────────────────────────────
# widget_speed.c
# ─────────────────────────────────────────────────────────────────────────────
spd_code = '#include "widget_speed.h"\n'
spd_code += COMMON_HDR
spd_code += "static uint64_t last_speed_can_received = 0;\n\n"
spd_code += ext(4118, 4149)
spd_code = deprivatize(spd_code, ["update_speed_ui_immediate"])
spd_code += r"""
void widget_speed_create(lv_obj_t *parent)
{
    ui_Speed_Value = lv_label_create(parent);
    lv_obj_set_width(ui_Speed_Value, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Speed_Value, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_Speed_Value, LV_ALIGN_CENTER);
    lv_obj_set_x(ui_Speed_Value, 0);
    lv_obj_set_y(ui_Speed_Value, 30);
    lv_label_set_text(ui_Speed_Value, "---");
    strcpy(previous_values[SPEED_VALUE_ID - 1], "---");
    lv_obj_set_style_text_color(ui_Speed_Value, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Speed_Value, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Speed_Value, THEME_FONT_DASH_SPEED, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Kmh = lv_label_create(parent);
    lv_obj_set_width(ui_Kmh, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_Kmh, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_Kmh, 37);
    lv_obj_set_y(ui_Kmh, 64);
    lv_obj_set_align(ui_Kmh, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Kmh, values_config[SPEED_VALUE_ID - 1].use_mph ? "mph" : "k/mh");
    lv_obj_set_style_text_color(ui_Kmh, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_Kmh, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Kmh, THEME_FONT_SMALL, LV_PART_MAIN | LV_STATE_DEFAULT);
}

uint64_t *widget_speed_get_last_can_time(void) { return &last_speed_can_received; }
"""
with open(os.path.join(WDIR, "widget_speed.c"), "w", encoding="utf-8") as f:
    f.write(spd_code)
print("OK widget_speed.c")

# ─────────────────────────────────────────────────────────────────────────────
# widget_gear.c
# ─────────────────────────────────────────────────────────────────────────────
gear_code = '#include "widget_gear.h"\n'
gear_code += COMMON_HDR
gear_code += "static uint64_t last_gear_can_received = 0;\n\n"
gear_code += "static bool should_show_gear_icon(uint32_t raw_value);\n"
gear_code += "static void update_gear_ui_immediate(const char *gear_str, uint32_t raw_value);\n\n"
for s, e in [(7323,7323),(7484,7487),(4152,4267),(6920,7068),(7071,7320),(7325,7480),(7490,7679)]:
    gear_code += ext(s, e)
gear_code = deprivatize(gear_code, ["update_gear_ui_immediate"])

gear_code += r"""
void widget_gear_create(lv_obj_t *parent)
{
    extern const lv_img_dsc_t Smart_Car_Key;

    ui_Gear_Panel = lv_obj_create(parent);
    lv_obj_set_size(ui_Gear_Panel, 90, 90);
    lv_obj_set_x(ui_Gear_Panel, 0); lv_obj_set_y(ui_Gear_Panel, 180);
    lv_obj_set_align(ui_Gear_Panel, LV_ALIGN_CENTER);
    lv_obj_clear_flag(ui_Gear_Panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Gear_Panel, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Gear_Panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Gear_Panel, THEME_COLOR_PANEL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Gear_Panel, 3, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Gear_Panel, 7, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Gear_Label = lv_label_create(parent);
    lv_obj_set_x(ui_Gear_Label, 0); lv_obj_set_y(ui_Gear_Label, 152);
    lv_obj_set_align(ui_Gear_Label, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Gear_Label, "GEAR");
    lv_obj_set_style_text_color(ui_Gear_Label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Gear_Label, THEME_FONT_DASH_LABEL, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_GEAR_Value = lv_label_create(parent);
    lv_obj_set_width(ui_GEAR_Value, 115);
    lv_obj_set_x(ui_GEAR_Value, 10); lv_obj_set_y(ui_GEAR_Value, 198);
    lv_obj_set_align(ui_GEAR_Value, LV_ALIGN_CENTER);
    lv_label_set_text(ui_GEAR_Value, "-");
    strcpy(previous_values[GEAR_VALUE_ID - 1], "-");
    lv_obj_set_style_text_color(ui_GEAR_Value, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_GEAR_Value, THEME_FONT_DASH_GEAR, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_transform_zoom(ui_GEAR_Value, 210, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_GEAR_Icon = lv_img_create(parent);
    lv_img_set_src(ui_GEAR_Icon, &Smart_Car_Key);
    lv_img_set_zoom(ui_GEAR_Icon, 225);
    lv_img_set_pivot(ui_GEAR_Icon, 15, 29);
    lv_obj_set_x(ui_GEAR_Icon, 0); lv_obj_set_y(ui_GEAR_Icon, 194);
    lv_obj_set_align(ui_GEAR_Icon, LV_ALIGN_CENTER);
    lv_obj_add_flag(ui_GEAR_Icon, LV_OBJ_FLAG_HIDDEN);
}

uint64_t *widget_gear_get_last_can_time(void) { return &last_gear_can_received; }
"""
with open(os.path.join(WDIR, "widget_gear.c"), "w", encoding="utf-8") as f:
    f.write(gear_code)
print("OK widget_gear.c")

# ─────────────────────────────────────────────────────────────────────────────
# widget_bar.c
# ─────────────────────────────────────────────────────────────────────────────
bar_code = '#include "widget_bar.h"\n'
bar_code += COMMON_HDR
bar_code += "static uint64_t last_bar_can_received[2] = {0, 0};\n"
bar_code += "float previous_bar_values[2] = {0, 0};\n\n"
for s, e in [(691,799),(1051,1161),(2050,2132),(2220,2487),(3947,4079)]:
    bar_code += ext(s, e)
bar_code = deprivatize(bar_code, ["update_bar_ui_immediate"])

bar_code += r"""
void widget_bar_create(lv_obj_t *parent)
{
    if (values_config[BAR1_VALUE_ID - 1].bar_max <= values_config[BAR1_VALUE_ID - 1].bar_min) {
        values_config[BAR1_VALUE_ID - 1].bar_min = 0; values_config[BAR1_VALUE_ID - 1].bar_max = 100;
    }
    ui_Bar_1 = lv_bar_create(parent);
    lv_bar_set_range(ui_Bar_1, values_config[BAR1_VALUE_ID-1].bar_min, values_config[BAR1_VALUE_ID-1].bar_max);
    lv_bar_set_value(ui_Bar_1, values_config[BAR1_VALUE_ID-1].bar_min, LV_ANIM_OFF);
    lv_obj_set_width(ui_Bar_1, 300); lv_obj_set_height(ui_Bar_1, 30);
    lv_obj_set_x(ui_Bar_1, -240); lv_obj_set_y(ui_Bar_1, 209);
    lv_obj_set_align(ui_Bar_1, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Bar_1, THEME_COLOR_PANEL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Bar_1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Bar_1, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Bar_1, THEME_COLOR_PANEL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Bar_1, 5, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Bar_1, THEME_COLOR_GREEN_BRIGHT, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Bar_1, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    ui_Bar_1_Label = lv_label_create(parent);
    lv_obj_set_x(ui_Bar_1_Label, -240); lv_obj_set_y(ui_Bar_1_Label, 181);
    lv_obj_set_align(ui_Bar_1_Label, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Bar_1_Label, label_texts[BAR1_VALUE_ID - 1]);
    lv_obj_set_style_text_color(ui_Bar_1_Label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Bar_1_Label, THEME_FONT_DASH_LABEL, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Bar_1_Value = lv_label_create(parent);
    lv_obj_set_width(ui_Bar_1_Value, 80); lv_obj_set_height(ui_Bar_1_Value, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_Bar_1_Value, LV_ALIGN_CENTER);
    lv_obj_set_x(ui_Bar_1_Value, -140); lv_obj_set_y(ui_Bar_1_Value, 181);
    lv_label_set_text(ui_Bar_1_Value, "---");
    lv_obj_set_style_text_color(ui_Bar_1_Value, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Bar_1_Value, THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Bar_1_Value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (!values_config[BAR1_VALUE_ID-1].show_bar_value) lv_obj_add_flag(ui_Bar_1_Value, LV_OBJ_FLAG_HIDDEN);

    if (values_config[BAR2_VALUE_ID - 1].bar_max <= values_config[BAR2_VALUE_ID - 1].bar_min) {
        values_config[BAR2_VALUE_ID - 1].bar_min = 0; values_config[BAR2_VALUE_ID - 1].bar_max = 100;
    }
    ui_Bar_2 = lv_bar_create(parent);
    lv_bar_set_range(ui_Bar_2, values_config[BAR2_VALUE_ID-1].bar_min, values_config[BAR2_VALUE_ID-1].bar_max);
    lv_bar_set_value(ui_Bar_2, values_config[BAR2_VALUE_ID-1].bar_min, LV_ANIM_OFF);
    lv_obj_set_width(ui_Bar_2, 300); lv_obj_set_height(ui_Bar_2, 30);
    lv_obj_set_x(ui_Bar_2, 240); lv_obj_set_y(ui_Bar_2, 209);
    lv_obj_set_align(ui_Bar_2, LV_ALIGN_CENTER);
    lv_obj_set_style_radius(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Bar_2, THEME_COLOR_PANEL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Bar_2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ui_Bar_2, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(ui_Bar_2, THEME_COLOR_PANEL, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(ui_Bar_2, 5, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(ui_Bar_2, THEME_COLOR_GREEN_BRIGHT, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Bar_2, 255, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    ui_Bar_2_Label = lv_label_create(parent);
    lv_obj_set_x(ui_Bar_2_Label, 240); lv_obj_set_y(ui_Bar_2_Label, 181);
    lv_obj_set_align(ui_Bar_2_Label, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Bar_2_Label, label_texts[BAR2_VALUE_ID - 1]);
    lv_obj_set_style_text_color(ui_Bar_2_Label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Bar_2_Label, THEME_FONT_DASH_LABEL, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_Bar_2_Value = lv_label_create(parent);
    lv_obj_set_width(ui_Bar_2_Value, 80); lv_obj_set_height(ui_Bar_2_Value, LV_SIZE_CONTENT);
    lv_obj_set_align(ui_Bar_2_Value, LV_ALIGN_CENTER);
    lv_obj_set_x(ui_Bar_2_Value, 340); lv_obj_set_y(ui_Bar_2_Value, 181);
    lv_label_set_text(ui_Bar_2_Value, "---");
    lv_obj_set_style_text_color(ui_Bar_2_Value, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_Bar_2_Value, THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(ui_Bar_2_Value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
    if (!values_config[BAR2_VALUE_ID-1].show_bar_value) lv_obj_add_flag(ui_Bar_2_Value, LV_OBJ_FLAG_HIDDEN);
}

uint64_t *widget_bar_get_last_can_time(uint8_t bar_idx) { return &last_bar_can_received[bar_idx & 1]; }
"""
with open(os.path.join(WDIR, "widget_bar.c"), "w", encoding="utf-8") as f:
    f.write(bar_code)
print("OK widget_bar.c")

# ─────────────────────────────────────────────────────────────────────────────
# widget_panel.c
# ─────────────────────────────────────────────────────────────────────────────
panel_code = '#include "widget_panel.h"\n'
panel_code += COMMON_HDR
panel_code += "static uint64_t last_panel_can_received[8] = {0};\n\n"
for s, e in [(155,163),(3789,3944),(5229,5300),(5528,5561),(5982,5999)]:
    panel_code += ext(s, e)
panel_code = deprivatize(panel_code, [
    "update_panel_ui_immediate",
    "init_styles",
    "init_boxes_and_arcs",
    "create_transparent_click_zone",
])

panel_code += r"""
void widget_panel_create(lv_obj_t *parent)
{
    init_boxes_and_arcs();
    for (uint8_t i = 0; i < 8; i++) {
        ui_Label[i] = lv_label_create(ui_Box[i]);
        lv_label_set_text(ui_Label[i], label_texts[i]);
        lv_obj_set_style_text_color(ui_Label[i], THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(ui_Label[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(ui_Label[i], THEME_FONT_DASH_LABEL, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(ui_Label[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(ui_Label[i], 145);
        lv_label_set_long_mode(ui_Label[i], LV_LABEL_LONG_CLIP);
        lv_coord_t relative_y = label_positions[i][1] - box_positions[i][1];
        lv_obj_set_x(ui_Label[i], 0); lv_obj_set_y(ui_Label[i], relative_y);
        lv_obj_set_align(ui_Label[i], LV_ALIGN_CENTER);

        ui_Value[i] = lv_label_create(parent);
        lv_label_set_text(ui_Value[i], "---");
        strcpy(previous_values[i], "---");
        lv_obj_set_style_text_color(ui_Value[i], THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(ui_Value[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(ui_Value[i], THEME_FONT_DASH_VALUE, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(ui_Value[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(ui_Value[i], 140);
        lv_label_set_long_mode(ui_Value[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_x(ui_Value[i], value_positions[i][0]);
        lv_obj_set_y(ui_Value[i], value_positions[i][1]);
        lv_obj_set_align(ui_Value[i], LV_ALIGN_CENTER);

        create_transparent_click_zone(parent, ui_Value[i], i + 1);

        ui_CustomText[i] = lv_label_create(parent);
        lv_label_set_text(ui_CustomText[i], values_config[i].custom_text);
        lv_obj_set_style_text_color(ui_CustomText[i], THEME_COLOR_TEXT_MUTED, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(ui_CustomText[i], 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(ui_CustomText[i], THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(ui_CustomText[i], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_width(ui_CustomText[i], 60);
        lv_label_set_long_mode(ui_CustomText[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_x(ui_CustomText[i], box_positions[i][0] + 41);
        lv_obj_set_y(ui_CustomText[i], box_positions[i][1] + 32);
        lv_obj_set_align(ui_CustomText[i], LV_ALIGN_CENTER);
        if (strlen(values_config[i].custom_text) == 0) lv_obj_add_flag(ui_CustomText[i], LV_OBJ_FLAG_HIDDEN);
    }
}

uint64_t *widget_panel_get_last_can_time(uint8_t idx) { return &last_panel_can_received[idx & 7]; }
"""
with open(os.path.join(WDIR, "widget_panel.c"), "w", encoding="utf-8") as f:
    f.write(panel_code)
print("OK widget_panel.c")

# ─────────────────────────────────────────────────────────────────────────────
# widget_rpm_bar.c
# ─────────────────────────────────────────────────────────────────────────────
rpm_code = '#include "widget_rpm_bar.h"\n'
rpm_code += COMMON_HDR
rpm_code += "static uint64_t last_rpm_can_received = 0;\n\n"
rpm_code += "static void stop_limiter_effect_demo(void);\n"
rpm_code += "static void stop_real_limiter_effect(void);\n"
rpm_code += "static void update_rpm_lights(int rpm_value);\n"
rpm_code += "static void update_rpm_ui_immediate(const char *rpm_str, int rpm_value);\n\n"
for s, e in [(186,198),(442,443),(474,690),(800,871),(872,958),(960,1050),
             (1162,1402),(1403,1577),(1578,1760),(1761,2018),(2019,2048),
             (2134,2218),(3489,3739),(3741,3786),(4082,4115),(5301,5399),(5400,5525)]:
    rpm_code += ext(s, e)
rpm_code = deprivatize(rpm_code, [
    "update_rpm_ui_immediate",
    "check_rpm_color_update",
])
rpm_code += r"""
void widget_rpm_bar_create(lv_obj_t *parent)
{
    create_rpm_bar_gauge(parent);
    update_rpm_lines(parent);
    update_redline_position();

    ui_RPM_Value = lv_label_create(parent);
    lv_obj_set_width(ui_RPM_Value, LV_SIZE_CONTENT);
    lv_obj_set_height(ui_RPM_Value, LV_SIZE_CONTENT);
    lv_obj_set_x(ui_RPM_Value, 0); lv_obj_set_y(ui_RPM_Value, -127);
    lv_obj_set_align(ui_RPM_Value, LV_ALIGN_CENTER);
    lv_label_set_text(ui_RPM_Value, "---");
    strcpy(previous_values[RPM_VALUE_ID - 1], "---");
    lv_obj_set_style_text_color(ui_RPM_Value, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(ui_RPM_Value, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_RPM_Value, THEME_FONT_DASH_RPM, LV_PART_MAIN | LV_STATE_DEFAULT);

    ui_RPM_Label = lv_label_create(parent);
    lv_obj_set_x(ui_RPM_Label, 0); lv_obj_set_y(ui_RPM_Label, -164);
    lv_obj_set_align(ui_RPM_Label, LV_ALIGN_CENTER);
    lv_label_set_text(ui_RPM_Label, "RPM");
    lv_obj_set_style_text_color(ui_RPM_Label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ui_RPM_Label, THEME_FONT_DASH_LABEL, LV_PART_MAIN | LV_STATE_DEFAULT);

    if (values_config[RPM_VALUE_ID - 1].rpm_lights_enabled) {
        create_rpm_lights_circles(parent);
    }
}

uint64_t *widget_rpm_bar_get_last_can_time(void) { return &last_rpm_can_received; }
"""
with open(os.path.join(WDIR, "widget_rpm_bar.c"), "w", encoding="utf-8") as f:
    f.write(rpm_code)
print("OK widget_rpm_bar.c")

# ─────────────────────────────────────────────────────────────────────────────
# widget_dispatcher.c  (process_can_message + check_can_timeouts + init_values)
# ─────────────────────────────────────────────────────────────────────────────
disp_code = '#include "widget_dispatcher.h"\n'
disp_code += COMMON_HDR
disp_code += '#include "widget_panel.h"\n'
disp_code += '#include "widget_rpm_bar.h"\n'
disp_code += '#include "widget_speed.h"\n'
disp_code += '#include "widget_gear.h"\n'
disp_code += '#include "widget_bar.h"\n'
disp_code += '#include "widget_indicator.h"\n'
disp_code += '#include "widget_warning.h"\n\n'

for s, e in [(112,143),(3377,3484),(4461,5222),(233,334),(6893,6917)]:
    disp_code += ext(s, e)

with open(os.path.join(WDIR, "widget_dispatcher.c"), "w", encoding="utf-8") as f:
    f.write(disp_code)
print("OK widget_dispatcher.c")

print("\nAll 8 widget .c files written successfully.")
