#ifndef MENU_SCREEN_H
#define MENU_SCREEN_H

#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Main menu functions
void load_menu_screen_for_value(uint8_t value_id);
void close_menu_event_cb(lv_event_t * e);
void cancel_menu_event_cb(lv_event_t * e);
void create_menu_objects(lv_obj_t * parent, uint8_t value_id);
void save_and_cleanup_cb(lv_timer_t * timer);

// External function declarations that are called from menu
extern void keyboard_ready_event_cb(lv_event_t * e);
extern void warning_high_threshold_event_cb(lv_event_t * e);
extern void warning_low_threshold_event_cb(lv_event_t * e);
extern void warning_high_color_event_cb(lv_event_t * e);
extern void warning_low_color_event_cb(lv_event_t * e);
extern void create_rpm_bar_gauge(lv_obj_t * parent);
extern void update_rpm_lines(lv_obj_t *parent);
extern void update_redline_position(void);
extern void rpm_gauge_roller_event_cb(lv_event_t * e);
extern void apply_common_roller_styles(lv_obj_t * roller);
extern void rpm_ecu_dropdown_event_cb(lv_event_t * e);
extern void rpm_color_dropdown_event_cb(lv_event_t * e);
extern void rpm_redline_roller_event_cb(lv_event_t * e);
extern void bar_range_input_event_cb(lv_event_t * e);
extern void bar_low_value_event_cb(lv_event_t * e);
extern void bar_high_value_event_cb(lv_event_t * e);

// Global variables for menu preview objects that need live updates
extern lv_obj_t * menu_panel_value_labels[8];
extern lv_obj_t * menu_panel_boxes[8];
extern lv_obj_t * menu_panel_labels[8];
extern lv_obj_t * menu_bar_objects[2];
extern lv_obj_t * menu_bar_labels[2];

#ifdef __cplusplus
}
#endif

#endif // MENU_SCREEN_H 