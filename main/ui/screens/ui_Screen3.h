#ifndef UI_SCREEN3_H
#define UI_SCREEN3_H

#include "cJSON.h"
#include "driver/twai.h"
#include "lvgl.h"
#include "ui_comp.h"
#include "ui_comp_hook.h"
#include "ui_events.h"
#include "ui_helpers.h"
#include <stdbool.h>
#include <stdint.h>

#define MAX_VALUES 13 /* Maximum number of values that can be configured */
#define RPM_VALUE_ID 9
#define SPEED_VALUE_ID 10
#define GEAR_VALUE_ID 11
#define BAR1_VALUE_ID 12
#define BAR2_VALUE_ID 13
#define MAX_RPM_LINES 200 /* Maximum number of RPM tick marks per row */
#ifndef EXAMPLE_MAX_CHAR_SIZE
#define EXAMPLE_MAX_CHAR_SIZE 64
#endif

#ifdef __cplusplus
extern "C" {
#endif

extern void device_settings_longpress_cb(lv_event_t *e);
extern void build_twai_filter_from_signals(twai_filter_config_t *out_filter);

// Style functions
void init_common_style(void);
lv_style_t *get_common_style(void);
lv_style_t *get_box_style(void);

extern lv_obj_t *g_label_input[];
extern lv_obj_t *g_can_id_input[];
extern lv_obj_t *g_endian_dropdown[];
extern lv_obj_t *g_bit_start_dropdown[];
extern lv_obj_t *g_bit_length_dropdown[];
extern lv_obj_t *g_scale_input[];
extern lv_obj_t *g_offset_input[];
extern lv_obj_t *g_decimals_dropdown[];
extern lv_obj_t *g_type_dropdown[];

extern lv_obj_t *ui_MenuScreen;
extern lv_obj_t *ui_RDM_Logo_Text;
extern lv_obj_t *brightness_bar;

typedef enum { BIG_ENDIAN_ORDER = 0, LITTLE_ENDIAN_ORDER = 1 } endian_t;

extern uint8_t current_value_id;
extern char value_offset_texts[13][64];
extern char previous_values[13][64];
extern bool reset_can_tracking;

/* RPM configuration globals */
extern int rpm_gauge_max;
extern int rpm_redline_value;

/* LVGL UI objects — defined in ui_Screen3.c */
extern lv_obj_t *ui_Label[13];
extern lv_obj_t *ui_Value[13];
extern lv_obj_t *ui_Box[8];
extern lv_obj_t *ui_CustomText[8];
extern lv_obj_t *config_bars[13];
extern lv_obj_t *rpm_bar_gauge;
extern lv_obj_t *rpm_redline_zone;
extern lv_obj_t *keyboard;
extern lv_timer_t *menu_button_hide_timer;

/* RPM tick-mark rendering objects (defined in widget_rpm_bar.c) */
extern int num_rpm_lines;
extern lv_obj_t *rpm_labels[MAX_RPM_LINES];
extern lv_obj_t *rpm_lines[MAX_RPM_LINES * 2];

/* Shared LVGL style */
extern lv_style_t common_style;

// Initialize UI Screen3
void ui_Screen3_screen_init(void);

/** Apply a temporary layout for live preview. */
void ui_Screen3_preview_layout(cJSON *root);

// Color wheel popup functions
void create_rpm_color_wheel_popup(void);
void create_limiter_color_wheel_popup(void);
void create_bar_low_color_wheel_popup(uint8_t value_id);
void create_bar_high_color_wheel_popup(uint8_t value_id);
void create_bar_in_range_color_wheel_popup(uint8_t value_id);

// Bar update struct shared between ui_Screen3.c and main.c
#ifndef BAR_UPDATE_T_DEFINED
#define BAR_UPDATE_T_DEFINED
typedef struct {
	uint8_t bar_index;
	int32_t bar_value;
	double final_value;
	int config_index;
	bool is_timeout;
} bar_update_t;
#endif
void update_bar_ui(void *param);

// Fuel sender functions (defined in main.c)
void fuel_sender_adc_init(void);
float fuel_sender_read_voltage(void);
float fuel_sender_get_filtered_v(uint8_t bar_idx);
void fuel_sender_capture_empty(uint8_t value_id);
void fuel_sender_capture_full(uint8_t value_id);

/* Coordinator-level callbacks used by widget modules */
void value_long_press_event_cb(lv_event_t *e);
void keyboard_ready_event_cb(lv_event_t *e);
void screen3_touch_event_cb(lv_event_t *e);

// Indicator config management functions
void create_indicator_config_menu(uint8_t indicator_idx);
void update_indicator_ui(void *param);
/** Apply analog (wire) indicator state; only updates indicators with
 * input_source == Wire (0). */
void indicator_apply_analog_state(bool left_on, bool right_on);

#ifdef __cplusplus
}
#endif

#endif // UI_SCREEN3_H
