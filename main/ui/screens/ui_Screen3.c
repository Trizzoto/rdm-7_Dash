#include "screens/ui_Screen3.h"
#include "../theme.h"
#include "../callbacks/ui_callbacks.h"
#include "../config/create_config_controls.h"
#include "../menu/menu_screen.h"
#include "../ui.h"
#include "../ui_helpers.h"
#include "../ui_preconfig.h"
#include "device_id.h"
#include "device_settings.h"
#include "driver/ledc.h"
#include "driver/twai.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include <stdint.h>
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "ota_handler.h"
#include "ui.h"
#include "ui_helpers.h"
#include "ui_preconfig.h"
#include "ui_wifi.h"
#include "version.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RPM_VALUE_ID 9
#define SPEED_VALUE_ID 10
#define GEAR_VALUE_ID 11
#define BAR1_VALUE_ID 12
#define BAR2_VALUE_ID 13
#define MAX_VALUES 13
// Bars should refresh with the same cadence as panels; any change is allowed,
// we gate frequency with a 25 ms per-bar throttle similar to panels.
#define BAR_UPDATE_THRESHOLD 0.0
#define LONG_PRESS_COOLDOWN 500
#define LABEL_TEXT_MAX_LEN 32
#define MAX_SPEED_CHANGE_PER_UPDATE                                            \
	20.0					  // Maximum realistic speed change between updates
#define MAX_VALID_SPEED 400.0 // Maximum valid speed value
#define MAX_RPM_LINES 200	  // Maximum number of ticks per set

#ifndef EXAMPLE_MAX_CHAR_SIZE
#define EXAMPLE_MAX_CHAR_SIZE 64
#endif

// External declarations
extern TaskHandle_t canTaskHandle;
extern twai_timing_config_t g_t_config;
extern twai_general_config_t g_config;
extern twai_filter_config_t f_config;
extern volatile bool can_task_should_stop;
extern void can_receive_task(void *pvParameter);
extern void save_values_config_to_nvs();

lv_obj_t *ui_Label[13] = {NULL};
lv_obj_t *ui_Value[13] = {NULL};
lv_obj_t *ui_Box[8] = {NULL};
lv_obj_t *ui_CustomText[8] = {NULL}; // Custom text labels for panels
lv_obj_t *config_bars[13] = {NULL};
lv_obj_t *ui_MenuScreen = NULL;
lv_obj_t *keyboard = NULL; // Global keyboard object
lv_obj_t *rpm_bar_gauge = NULL;
extern lv_obj_t
	*menu_rpm_value_label; // RPM value label in menu screen for demo updates

// External references to menu preview objects for live updates
extern lv_obj_t *menu_speed_value_label;
extern lv_obj_t *menu_speed_units_label;
extern lv_obj_t *menu_gear_value_label;
extern lv_obj_t *menu_panel_value_labels[8];
extern lv_obj_t *menu_bar_objects[2];

lv_obj_t *ui_Gear_Label = NULL;
lv_obj_t *ui_Bar_1_Value = NULL;
lv_obj_t *ui_Bar_2_Value = NULL;

// Menu button timer for tap-to-show functionality (ui_Menu_Button is declared in ui.h)
lv_timer_t *menu_button_hide_timer = NULL;

// Touch tracking for quick tap detection
static uint32_t touch_press_time = 0;
static lv_obj_t *ui_Setup_Menu_Screen = NULL;
lv_obj_t *g_label_input[MAX_VALUES];
lv_obj_t *g_can_id_input[MAX_VALUES];
lv_obj_t *g_endian_dropdown[MAX_VALUES];
lv_obj_t *g_bit_start_dropdown[MAX_VALUES];
lv_obj_t *g_bit_length_dropdown[MAX_VALUES];
lv_obj_t *g_scale_input[MAX_VALUES];
lv_obj_t *g_offset_input[MAX_VALUES];
lv_obj_t *g_decimals_dropdown[MAX_VALUES];
lv_obj_t *g_type_dropdown[MAX_VALUES];

int rpm_gauge_max = 7000;		   // Default max RPM value
int rpm_redline_value = 6000;	   // Default redline RPM value
lv_obj_t *rpm_redline_zone = NULL; // Red rectangle for redline zone
value_config_t values_config[13];
uint8_t current_value_id;

typedef struct {
	uint8_t panel_index;
	char value_str[EXAMPLE_MAX_CHAR_SIZE];
	double final_value;
} panel_update_t;

// bar_update_t is now declared in ui_Screen3.h so main.c can use it too.
// Keep the local definition only if the header hasn't already defined it.
#ifndef BAR_UPDATE_T_DEFINED
#define BAR_UPDATE_T_DEFINED
typedef struct {
	uint8_t bar_index;	// 0 for BAR1, 1 for BAR2.
	int32_t bar_value;	// The clamped value for display.
	double final_value; // The processed (scaled) CANbus value.
	int config_index;	// The index into values_config for this bar.
	bool is_timeout;	// True if this update is due to a timeout
} bar_update_t;
#endif

typedef struct {
	char rpm_str[EXAMPLE_MAX_CHAR_SIZE];
	int rpm_value;
} rpm_update_t;

typedef struct {
	char speed_str[EXAMPLE_MAX_CHAR_SIZE];
} speed_update_t;

typedef struct {
	char gear_str[EXAMPLE_MAX_CHAR_SIZE];
	uint32_t raw_value;  // Raw CAN value for icon checking
} gear_update_t;

char label_texts[13][64] = {"PANEL 1", "PANEL 2", "PANEL 3", "PANEL 4",
							"PANEL 5", "PANEL 6", "PANEL 7", "PANEL 8",
							"RPM",	   "SPEED",	  "GEAR",	 "BAR 1",
							"BAR 2"}; // default texts
char value_offset_texts[13][64] = {
	"0", "0", "0", "0", "0", "0", "0",
	"0", "0", "0", "0", "0", "0"}; // default offsets
uint8_t endianess[13] = {
	1}; // Storage for endianness: 0 = Big Endian, 1 = Small Endian

static const lv_coord_t label_positions[8][2] = {
	{-312, -54}, {-146, -54}, {-312, 54}, {-146, 54},
	{146, -54},	 {312, -54},  {146, 54},  {312, 54}};
static const lv_coord_t value_positions[8][2] = {
	{-312, -17}, {-146, -17}, {-312, 91}, {-146, 91},
	{146, -17},	 {312, -17},  {146, 91},  {312, 91}};
static const lv_coord_t box_positions[8][2] = {
	{-312, -26}, {-146, -26}, {-312, 82}, {-146, 82},
	{146, -26},	 {312, -26},  {146, 82},  {312, 82}};

static const struct {
	int16_t x;
	int16_t y;
} warning_positions[] = {
	{-352, -148}, // Warning 1
	{-292, -148}, // Warning 2
	{-232, -148}, // Warning 3
	{-172, -148}, // Warning 4
	{172, -148},  // Warning 5
	{232, -148},  // Warning 6
	{292, -148},  // Warning 7
	{352, -148}	  // Warning 8
};

warning_config_t warning_configs[8];
static lv_obj_t *warning_circles[8] = {NULL};
static lv_obj_t *warning_labels[8] = {NULL};
static uint64_t last_signal_times[8] = {0};
static bool toggle_debounce[8] = {false};
static uint64_t toggle_start_time[8] = {0};
// Limiter circles removed - only bar flash effect is supported
static lv_obj_t *rpm_lights_circles[8] = {
	NULL}; // Separate circles for RPM Lights
static lv_timer_t *limiter_demo_timer = NULL;
static lv_timer_t *limiter_flash_timer = NULL;
static bool limiter_demo_active = false;
static bool limiter_flash_state = false;
static lv_color_t original_rpm_color;
static uint8_t current_effect_type = 0;

static int current_canbus_rpm = 0;	  // Store the current CAN bus RPM value
static int saved_rpm_before_demo = 0; // Save RPM value before demo starts

// CAN timeout tracking
static uint64_t last_panel_can_received[8] = {0};
static uint64_t last_speed_can_received = 0;
static uint64_t last_gear_can_received = 0;
static uint64_t last_bar_can_received[2] = {0};
static uint64_t last_rpm_can_received = 0;

// Limiter activation tracking
static bool limiter_active = false;
static bool real_limiter_active = false;
static int last_rpm_for_limiter_check = -1;
static bool previous_bit_states[8] = {false}; // Track previous bit states for toggle mode warnings

// Forward declarations for optimization functions
static void batch_update_rpm_circles_color(lv_color_t color);

typedef struct {
	uint8_t warning_idx;
	lv_obj_t **input_objects;
	lv_obj_t **preview_objects;
	lv_obj_t *preconfig_warning_dd; // Reference to warning dropdown in preconfig container
} warning_save_data_t;

// Indicator config variables
indicator_config_t indicator_configs[2]; // Left and Right indicators
static bool indicator_toggle_debounce[2] = {false};
static uint64_t indicator_toggle_start_time[2] = {0};
static bool previous_indicator_bit_states[2] = {false};

// Animation variables
static lv_timer_t *indicator_animation_timer = NULL;
static bool indicator_animation_state = false; // true for bright, false for dim
static bool previous_indicator_states[2] = {
	false}; // Track previous states to avoid redundant updates

void init_values_config_defaults(void) {
	for (int i = 0; i < 13; i++) {
		values_config[i].enabled = false;
		values_config[i].can_id = 0;
		values_config[i].endianess = 0;
		values_config[i].bit_start = 0;
		values_config[i].bit_length = 0;
		values_config[i].decimals = 0;
		values_config[i].value_offset = 0;
		values_config[i].scale = 1;
		values_config[i].is_signed = false;
		// Initialize custom text field to empty string
		memset(values_config[i].custom_text, 0,
			   sizeof(values_config[i].custom_text));

		if (i < 8) {
			values_config[i].warning_high_threshold = 0;
			values_config[i].warning_low_threshold = 0;
			values_config[i].warning_high_color =
				THEME_COLOR_RED; // Default red
			values_config[i].warning_low_color =
				THEME_COLOR_BLUE_DARK; // Default blue
			values_config[i].warning_high_enabled = false;
			values_config[i].warning_low_enabled = false;
		}
	}

	values_config[RPM_VALUE_ID - 1].enabled = true;
	values_config[RPM_VALUE_ID - 1].rpm_bar_color =
		THEME_COLOR_RED;								// Default red
	values_config[RPM_VALUE_ID - 1].rpm_limiter_effect = 0; // Default: None
	values_config[RPM_VALUE_ID - 1].rpm_limiter_value =
		7000; // Default: 7000 RPM
	values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
		THEME_COLOR_RED; // Default: Red
	values_config[RPM_VALUE_ID - 1].rpm_lights_enabled =
		false; // Default: Disabled
	values_config[RPM_VALUE_ID - 1].rpm_background_enabled =
		false; // Default: Disabled
	values_config[RPM_VALUE_ID - 1].rpm_background_value =
		7000; // Default: 7000 RPM
	values_config[RPM_VALUE_ID - 1].rpm_background_color =
		THEME_COLOR_GREEN; // Default: Green
	values_config[SPEED_VALUE_ID - 1].enabled = true;
	values_config[GEAR_VALUE_ID - 1].enabled = true;
	values_config[GEAR_VALUE_ID - 1].gear_detection_mode =
		1; // Default to MaxxECU
	// Initialize custom gear values to UINT32_MAX (not configured)
	for (int j = 0; j < 14; j++) {
		values_config[GEAR_VALUE_ID - 1].gear_custom_values[j] = UINT32_MAX;
	}
	// Initialize custom icon values to UINT32_MAX (not configured)
	for (int j = 0; j < 7; j++) {
		values_config[GEAR_VALUE_ID - 1].custom_icon_values[j] = UINT32_MAX;
	}
	// Initialize Speed/RPM Ratio default values
	values_config[GEAR_VALUE_ID - 1].tire_circumference_mm = 2000.0f; // Default: 2000mm (typical car tire)
	values_config[GEAR_VALUE_ID - 1].final_drive_ratio = 3.420f; // Default: 3.42 (common ratio)
	values_config[GEAR_VALUE_ID - 1].reverse_gear_ratio = 3.50f; // Default: 3.50 (typical reverse gear ratio)
	// Initialize gear ratios with typical 6-speed manual transmission ratios
	values_config[GEAR_VALUE_ID - 1].gear_ratios[0] = 3.36f;  // 1st gear
	values_config[GEAR_VALUE_ID - 1].gear_ratios[1] = 2.07f;  // 2nd gear
	values_config[GEAR_VALUE_ID - 1].gear_ratios[2] = 1.40f;  // 3rd gear
	values_config[GEAR_VALUE_ID - 1].gear_ratios[3] = 1.00f;  // 4th gear
	values_config[GEAR_VALUE_ID - 1].gear_ratios[4] = 0.84f;  // 5th gear
	values_config[GEAR_VALUE_ID - 1].gear_ratios[5] = 0.56f;  // 6th gear
	values_config[GEAR_VALUE_ID - 1].gear_ratios[6] = 0.0f;   // 7th gear (unused)
	values_config[GEAR_VALUE_ID - 1].gear_ratios[7] = 0.0f;   // 8th gear (unused)
	values_config[GEAR_VALUE_ID - 1].gear_ratios[8] = 0.0f;   // 9th gear (unused)
	values_config[GEAR_VALUE_ID - 1].gear_ratios[9] = 0.0f;   // 10th gear (unused)
	values_config[BAR1_VALUE_ID - 1].enabled = true;
	values_config[BAR2_VALUE_ID - 1].enabled = true;

	// Set default ranges for bars
	values_config[BAR1_VALUE_ID - 1].bar_min = 0;
	values_config[BAR1_VALUE_ID - 1].bar_max = 100;
	values_config[BAR2_VALUE_ID - 1].bar_min = 0;
	values_config[BAR2_VALUE_ID - 1].bar_max = 100;

	// Set default bar colors for BAR1 and BAR2
	values_config[BAR1_VALUE_ID - 1].bar_low_color =
		THEME_COLOR_BLUE_DARK; // Blue
	values_config[BAR1_VALUE_ID - 1].bar_high_color =
		THEME_COLOR_RED; // Red
	values_config[BAR1_VALUE_ID - 1].bar_in_range_color =
		THEME_COLOR_GREEN_BRIGHT; // Green
	values_config[BAR1_VALUE_ID - 1].show_bar_value =
		true; // Show value by default
	values_config[BAR1_VALUE_ID - 1].invert_bar_value =
		false; // Don't invert by default

	values_config[BAR2_VALUE_ID - 1].bar_low_color =
		THEME_COLOR_BLUE_DARK; // Blue
	values_config[BAR2_VALUE_ID - 1].bar_high_color =
		THEME_COLOR_RED; // Red
	values_config[BAR2_VALUE_ID - 1].bar_in_range_color =
		THEME_COLOR_GREEN_BRIGHT; // Green
	values_config[BAR2_VALUE_ID - 1].show_bar_value =
		true; // Show value by default
	values_config[BAR2_VALUE_ID - 1].invert_bar_value =
		false; // Don't invert by default
}

static void init_warning_configs(void) {
	// Initialize warning configurations with defaults
	for (int i = 0; i < 8; i++) {
		warning_configs[i].can_id = 0x000;
		warning_configs[i].bit_position = 0;
		warning_configs[i].endianess = 1; // Default to Little Endian
		warning_configs[i].active_color = THEME_COLOR_RED;
		snprintf(warning_configs[i].label, sizeof(warning_configs[i].label),
				 "Warning %d", i + 1);
		warning_configs[i].is_momentary = true;
		warning_configs[i].current_state = false;
		warning_configs[i].invert_toggle = false;
	}
}

void init_indicator_configs(void) {
	// Initialize indicator configurations with defaults
	for (int i = 0; i < 2; i++) {
		indicator_configs[i].can_id = 0x000;
		indicator_configs[i].bit_position = 0;
		indicator_configs[i].is_momentary = true;
		indicator_configs[i].current_state = false;
		indicator_configs[i].animation_enabled = true; // Default to animated
		indicator_configs[i].input_source = 0;         // Default Wire
	}
}

void print_value_config(uint8_t value_id) {
	uint8_t idx = value_id - 1;
	printf("Value #%d Configuration:\n", value_id);
	printf("  Enabled: %s\n", values_config[idx].enabled ? "Yes" : "No");
	printf("  CAN ID: %u\n", values_config[idx].can_id);
	printf("  Endianess: %s\n",
		   values_config[idx].endianess == 0 ? "Big Endian" : "Little Endian");
	printf("  Bit Start: %d\n", values_config[idx].bit_start);
	printf("  Bit Length: %d\n", values_config[idx].bit_length);
	printf("  Decimals: %d\n", values_config[idx].decimals);
	printf("  Value Offset: %g\n", values_config[idx].value_offset);
	printf("  Scale: %g\n", values_config[idx].scale);
	printf("-------------------------------------------\n");
}

/////////////////////////////////////////////	FORWARD DECLERATIONS
////////////////////////////////////////////////
static void free_warning_idx_event_cb(lv_event_t * e);
static void invert_warning_toggle_event_cb(lv_event_t * e);
static void update_warning_ui_immediate(uint8_t warning_idx);
void update_rpm_lines(lv_obj_t *parent);
void set_rpm_value(int rpm);
void update_redline_position(void);
void create_warning_config_menu(uint8_t warning_idx);
void create_indicator_config_menu(uint8_t indicator_idx);
static void value_long_press_event_cb(lv_event_t *e);
void keyboard_ready_event_cb(lv_event_t *e);
void rpm_color_dropdown_event_cb(lv_event_t *e);
void rpm_limiter_effect_dropdown_event_cb(lv_event_t *e);
void rpm_limiter_roller_event_cb(lv_event_t *e);
void rpm_limiter_color_dropdown_event_cb(lv_event_t *e);
void rpm_lights_switch_event_cb(lv_event_t *e);
void rpm_background_switch_event_cb(lv_event_t *e);
void rpm_background_color_dropdown_event_cb(lv_event_t *e);
void rpm_background_threshold_roller_event_cb(lv_event_t *e);
void create_rpm_background_color_wheel_popup(void);
void bar_low_color_event_cb(lv_event_t *e);
void bar_high_color_event_cb(lv_event_t *e);
void bar_in_range_color_event_cb(lv_event_t *e);
// Limiter circles color update function removed - only bar flash effect is
// supported
static void update_rpm_lights(int rpm_value);
// Limiter circles creation function removed - only bar flash effect is
// supported
void create_rpm_lights_circles(lv_obj_t *parent);
static void update_menu_rpm_value_text(int rpm_value);
// Limiter circles clear function removed - only bar flash effect is supported
static void clear_rpm_lights_circles(void);

void start_limiter_effect_demo(uint8_t effect_type);
void stop_limiter_effect_demo(void);
static void start_real_limiter_effect(uint8_t effect_type);
static void stop_real_limiter_effect(void);
void update_panel_ui(void *param);
void update_bar_ui(void *param);
void update_rpm_ui(void *param);
void update_speed_ui(void *param);
void update_gear_ui(void *param);
void update_warning_ui(void *param);
void update_indicator_ui(void *param);
void speed_preview_timer_cb(
	lv_timer_t *timer); // Speed preview update timer callback
void calibration_speed_roller_event_cb(
	lv_event_t *e); // Calibration speed roller callback
void speed_rpm_gear_update_timer_cb(lv_timer_t *timer);
static void save_indicator_config_cb(lv_event_t *e);
static void screen3_touch_event_cb(lv_event_t *e);
static void menu_button_hide_timer_cb(lv_timer_t *timer);
static void menu_button_clicked_cb(lv_event_t *e);
static void setup_menu_close_btn_cb(lv_event_t *e);
static void back_indicator_config_cb(lv_event_t *e);
void update_config_preview(uint8_t indicator_idx);
static void indicator_can_id_changed_cb(lv_event_t *e);
static void indicator_bit_pos_changed_cb(lv_event_t *e);
static void indicator_toggle_mode_changed_cb(lv_event_t *e);
static void indicator_animation_changed_cb(lv_event_t *e);
static void update_indicator_ui_immediate(uint8_t indicator_idx);
void indicator_animation_timer_cb(lv_timer_t *timer);

static bool rpm_color_needs_update = false;
static lv_color_t new_rpm_color;
char previous_values[13][64] = {0};
bool reset_can_tracking =
	false; // Flag to reset CAN tracking variables after screen recreation
float previous_bar_values[2] = {0, 0};
static uint32_t last_long_press_time = 0;

/////////////////////////////////////////////	CALLBACKS
////////////////////////////////////////////////

void keyboard_ready_event_cb(lv_event_t *e) {
	lv_obj_t *keyboard = lv_event_get_target(e);
	// Hide the keyboard
	lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void value_long_press_event_cb(lv_event_t *e) {
	uint32_t current_time = lv_tick_get();
	if (current_time - last_long_press_time < LONG_PRESS_COOLDOWN)
		return;
	last_long_press_time = current_time;

	uint8_t *p_id = (uint8_t *)lv_event_get_user_data(e);
	if (!p_id)
		return;

	current_value_id = *p_id;
	printf("Long press detected on value %u\n", current_value_id);
	load_menu_screen_for_value(current_value_id);
}

void rpm_gauge_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);
	rpm_gauge_max = 3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

	// Stop any active demos before UI changes
	stop_limiter_effect_demo();

	// Clear RPM lights circles as they will be invalidated by UI changes
	clear_rpm_lights_circles();

	// Update the RPM bar gauge range to match the new max
	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
		lv_bar_set_range(rpm_bar_gauge, 0, rpm_gauge_max);
	}

	update_rpm_lines(lv_scr_act());
	update_redline_position();
}

void rpm_redline_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);
	rpm_redline_value = 3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

	// Stop any active limiter demo before UI changes
	stop_limiter_effect_demo();

	update_redline_position();
}

void rpm_ecu_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);
	// Indices: 0 = Custom, 1 = MaxxECU, 2 = Haltech

	if (selected == 0) {
		// ========== CUSTOM ==========
		printf("ECU Presets: Custom (no changes)\n");
		// Do nothing, or set defaults if you prefer
	} else if (selected == 1) {
		// ========== MAXXECU ==========
		printf("ECU Presets: MaxxECU\n");
		values_config[RPM_VALUE_ID - 1].can_id = 520; // 0x208
		values_config[RPM_VALUE_ID - 1].endianess =
			1; // 0=Big,1=Little (check your usage)
		values_config[RPM_VALUE_ID - 1].bit_start = 0;
		values_config[RPM_VALUE_ID - 1].bit_length = 16;
		values_config[RPM_VALUE_ID - 1].scale = 1.0f;
		values_config[RPM_VALUE_ID - 1].value_offset = 0.0f;
		values_config[RPM_VALUE_ID - 1].decimals = 0;

		// Now update the UI fields for RPM (ID=9 => index=8)
		// --------------------------------------------------
		if (g_can_id_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%u",
					 values_config[RPM_VALUE_ID - 1].can_id);
			lv_textarea_set_text(g_can_id_input[RPM_VALUE_ID - 1], buf);
		}

		// Endianness: your dropdown presumably has "Big Endian\nLittle Endian"
		// 0 => Big, 1 => Little
		if (g_endian_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_endian_dropdown[RPM_VALUE_ID - 1],
									 values_config[RPM_VALUE_ID - 1].endianess);
		}

		// Bit start
		if (g_bit_start_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_bit_start_dropdown[RPM_VALUE_ID - 1],
									 values_config[RPM_VALUE_ID - 1].bit_start);
		}

		// Bit length (the dropdown might list 1..64, so we subtract 1 for
		// zero-based index)
		if (g_bit_length_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_length_dropdown[RPM_VALUE_ID - 1],
				values_config[RPM_VALUE_ID - 1].bit_length - 1);
		}

		// Scale
		if (g_scale_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[RPM_VALUE_ID - 1].scale);
			lv_textarea_set_text(g_scale_input[RPM_VALUE_ID - 1], buf);
		}

		// Value offset
		if (g_offset_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[RPM_VALUE_ID - 1].value_offset);
			lv_textarea_set_text(g_offset_input[RPM_VALUE_ID - 1], buf);
		}

		// Decimals
		if (g_decimals_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_decimals_dropdown[RPM_VALUE_ID - 1],
									 values_config[RPM_VALUE_ID - 1].decimals);
		}
	} else if (selected == 2) {
		// ========== HALTECH ==========
		printf("ECU Presets: Haltech\n");
		values_config[RPM_VALUE_ID - 1].can_id = 360; // 0x209
		values_config[RPM_VALUE_ID - 1].endianess = 0;
		values_config[RPM_VALUE_ID - 1].bit_start = 0;
		values_config[RPM_VALUE_ID - 1].bit_length = 16;
		values_config[RPM_VALUE_ID - 1].scale = 1.0f;
		values_config[RPM_VALUE_ID - 1].value_offset = 0.0f;
		values_config[RPM_VALUE_ID - 1].decimals = 0;

		// Update UI fields similarly
		// --------------------------------------------------
		if (g_can_id_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%u",
					 values_config[RPM_VALUE_ID - 1].can_id);
			lv_textarea_set_text(g_can_id_input[RPM_VALUE_ID - 1], buf);
		}

		if (g_endian_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_endian_dropdown[RPM_VALUE_ID - 1],
									 values_config[RPM_VALUE_ID - 1].endianess);
		}

		if (g_bit_start_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_bit_start_dropdown[RPM_VALUE_ID - 1],
									 values_config[RPM_VALUE_ID - 1].bit_start);
		}

		if (g_bit_length_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_length_dropdown[RPM_VALUE_ID - 1],
				values_config[RPM_VALUE_ID - 1].bit_length - 1);
		}

		if (g_scale_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[RPM_VALUE_ID - 1].scale);
			lv_textarea_set_text(g_scale_input[RPM_VALUE_ID - 1], buf);
		}

		if (g_offset_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[RPM_VALUE_ID - 1].value_offset);
			lv_textarea_set_text(g_offset_input[RPM_VALUE_ID - 1], buf);
		}

		if (g_decimals_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_decimals_dropdown[RPM_VALUE_ID - 1],
									 values_config[RPM_VALUE_ID - 1].decimals);
		}
	} else if (selected == 3) {
		// ========== FORD BA/BF/FG ==========
		printf("ECU Presets: Ford BA/BF/FG\n");
		values_config[RPM_VALUE_ID - 1].can_id = 0x3E8; // 1000 decimal
		values_config[RPM_VALUE_ID - 1].endianess = 1;	// Little Endian
		values_config[RPM_VALUE_ID - 1].bit_start = 0;
		values_config[RPM_VALUE_ID - 1].bit_length = 16;
		values_config[RPM_VALUE_ID - 1].scale = 0.25f;
		values_config[RPM_VALUE_ID - 1].value_offset = 0.0f;
		values_config[RPM_VALUE_ID - 1].decimals = 2;

		// Update UI fields
		// --------------------------------------------------
		if (g_can_id_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%X",
					 values_config[RPM_VALUE_ID - 1].can_id);
			lv_textarea_set_text(g_can_id_input[RPM_VALUE_ID - 1], buf);
		}

		if (g_endian_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_endian_dropdown[RPM_VALUE_ID - 1],
									 values_config[RPM_VALUE_ID - 1].endianess);
		}

		if (g_bit_start_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_bit_start_dropdown[RPM_VALUE_ID - 1],
									 values_config[RPM_VALUE_ID - 1].bit_start);
		}

		if (g_bit_length_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_length_dropdown[RPM_VALUE_ID - 1],
				values_config[RPM_VALUE_ID - 1].bit_length - 1);
		}

		if (g_scale_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[RPM_VALUE_ID - 1].scale);
			lv_textarea_set_text(g_scale_input[RPM_VALUE_ID - 1], buf);
		}

		if (g_offset_input[RPM_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[RPM_VALUE_ID - 1].value_offset);
			lv_textarea_set_text(g_offset_input[RPM_VALUE_ID - 1], buf);
		}

		if (g_decimals_dropdown[RPM_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_decimals_dropdown[RPM_VALUE_ID - 1],
									 values_config[RPM_VALUE_ID - 1].decimals);
		}
	}

	// Print config to confirm
	print_value_config(RPM_VALUE_ID);
}

void bar_range_input_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *textarea = lv_event_get_target(e);
		const char *txt = lv_textarea_get_text(textarea);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);

		bool is_min = lv_obj_get_user_data(textarea) !=
					  NULL; // Check if this is min input
		int32_t value = atoi(txt);

		if (value_id == BAR1_VALUE_ID) {
			if (is_min) {
				values_config[value_id - 1].bar_min = value;
				lv_bar_set_range(ui_Bar_1, value,
								 values_config[value_id - 1].bar_max);
			} else {
				values_config[value_id - 1].bar_max = value;
				lv_bar_set_range(ui_Bar_1, values_config[value_id - 1].bar_min,
								 value);
			}
		} else if (value_id == BAR2_VALUE_ID) {
			if (is_min) {
				values_config[value_id - 1].bar_min = value;
				lv_bar_set_range(ui_Bar_2, value,
								 values_config[value_id - 1].bar_max);
			} else {
				values_config[value_id - 1].bar_max = value;
				lv_bar_set_range(ui_Bar_2, values_config[value_id - 1].bar_min,
								 value);
			}
		}
	}
}

void bar_low_value_event_cb(lv_event_t *e) {
	lv_obj_t *ta = lv_event_get_target(e);
	const char *txt = lv_textarea_get_text(ta);
	int low_val = atoi(txt);

	// Retrieve value_id from the event's user data
	uint8_t *id_ptr = lv_event_get_user_data(e);
	uint8_t value_id = *id_ptr;

	// Update the configuration structure (make sure 'bar_low' is a valid field)
	values_config[value_id - 1].bar_low = low_val;

	// Retrieve the preview bar pointer (stored in the config_bars[] global
	// array)
	lv_obj_t *menu_bar = config_bars[value_id - 1];
	if (menu_bar) {
		int current_val = lv_bar_get_value(menu_bar);
		if (current_val < low_val) {
			// Use configured low color
			lv_obj_set_style_bg_color(menu_bar,
									  values_config[value_id - 1].bar_low_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else if (current_val > values_config[value_id - 1].bar_high) {
			// Use configured high color
			lv_obj_set_style_bg_color(
				menu_bar, values_config[value_id - 1].bar_high_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else {
			// Use configured in-range color
			lv_obj_set_style_bg_color(
				menu_bar, values_config[value_id - 1].bar_in_range_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
	}
}

void bar_high_value_event_cb(lv_event_t *e) {
	lv_obj_t *ta = lv_event_get_target(e);
	const char *txt = lv_textarea_get_text(ta);
	int high_val = atoi(txt);

	// Retrieve value_id from the event's user data
	uint8_t *id_ptr = lv_event_get_user_data(e);
	uint8_t value_id = *id_ptr;

	// Update the configuration structure with the new bar high threshold
	values_config[value_id - 1].bar_high = high_val;

	// Retrieve the preview bar pointer from the global config_bars array
	lv_obj_t *menu_bar = config_bars[value_id - 1];
	if (menu_bar) {
		int current_val = lv_bar_get_value(menu_bar);
		if (current_val < values_config[value_id - 1].bar_low) {
			// Use configured low color
			lv_obj_set_style_bg_color(menu_bar,
									  values_config[value_id - 1].bar_low_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else if (current_val > high_val) {
			// Use configured high color
			lv_obj_set_style_bg_color(
				menu_bar, values_config[value_id - 1].bar_high_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
		} else {
			// Use configured in-range color
			lv_obj_set_style_bg_color(
				menu_bar, values_config[value_id - 1].bar_in_range_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
	}
}

// Forward declaration for color wheel popup
void create_rpm_color_wheel_popup(void);
void create_limiter_color_wheel_popup(void);

void rpm_color_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	// Determine new color based on selection - SUPER VIBRANT COLORS
	switch (selected) {
	case 0:
		new_rpm_color = THEME_COLOR_GREEN;
		break; // Bright Green
	case 1:
		new_rpm_color = THEME_COLOR_CYAN;
		break; // Bright Cyan
	case 2:
		new_rpm_color = THEME_COLOR_YELLOW;
		break; // Bright Yellow
	case 3:
		new_rpm_color = THEME_COLOR_ORANGE;
		break; // Bright Orange
	case 4:
		new_rpm_color = THEME_COLOR_RED;
		break; // Bright Red
	case 5:
		new_rpm_color = THEME_COLOR_BLUE;
		break; // Bright Blue
	case 6:
		new_rpm_color = THEME_COLOR_PURPLE;
		break; // Bright Purple
	case 7:
		new_rpm_color = THEME_COLOR_MAGENTA;
		break; // Bright Magenta
	case 8:
		new_rpm_color = THEME_COLOR_PINK;
		break; // Bright Hot Pink
	case 9:	   // Custom color - open color wheel popup
		create_rpm_color_wheel_popup();
		return; // Don't update color yet, wait for color wheel selection
	default:
		new_rpm_color = THEME_COLOR_GREEN;
		break;
	}

	// Don't update colors when real limiter effect is active to avoid conflicts
	// with flashing
	if (!real_limiter_active) {
		rpm_color_needs_update = true;
	}
	values_config[RPM_VALUE_ID - 1].rpm_bar_color = new_rpm_color;
}

static void check_rpm_color_update(lv_timer_t *timer) {
	// Don't update colors when real limiter effect is active to avoid conflicts
	// with flashing
	if (rpm_color_needs_update && !real_limiter_active) {
		if (rpm_bar_gauge) {
			lv_obj_set_style_bg_color(rpm_bar_gauge, new_rpm_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
			// Set gradient color to same as main color for solid appearance
			lv_obj_set_style_bg_grad_color(rpm_bar_gauge, new_rpm_color,
										   LV_PART_INDICATOR |
											   LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9) {
			lv_obj_set_style_bg_color(ui_Panel9, new_rpm_color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		rpm_color_needs_update = false;
	}
}

// RPM Limiter Effect event callbacks
void rpm_limiter_effect_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	// Map dropdown selection to effect type (0=None, 2=Bar Flash, 3=Bar &
	// Circles Flash, 4=Circles Flash, 5=Bar Solid, 6=Bar & Circles Solid,
	// 7=Circles Solid)
	uint8_t effect_type = 0;
	if (selected == 1) {
		effect_type = 2; // Bar Flash only
	} else if (selected == 2) {
		effect_type = 3; // Bar & Circles Flash (combined effect)
	} else if (selected == 3) {
		effect_type = 4; // Circles Flash only
	} else if (selected == 4) {
		effect_type = 5; // Bar Solid only
	} else if (selected == 5) {
		effect_type = 6; // Bar & Circles Solid
	} else if (selected == 6) {
		effect_type = 7; // Circles Solid only
	}

	// Update configuration
	values_config[RPM_VALUE_ID - 1].rpm_limiter_effect = effect_type;

	// Demo the selected effect for 1 second
	start_limiter_effect_demo(effect_type);
}

void rpm_limiter_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);

	int32_t rpm_value = 3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

	// Update configuration
	values_config[RPM_VALUE_ID - 1].rpm_limiter_value = rpm_value;
}

void rpm_limiter_color_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	switch (selected) {
	case 0: // Green
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
			THEME_COLOR_GREEN;
		break;
	case 1: // Light Blue
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
			THEME_COLOR_CYAN;
		break;
	case 2: // Yellow
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
			THEME_COLOR_YELLOW;
		break;
	case 3: // Orange
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
			THEME_COLOR_ORANGE;
		break;
	case 4: // Red
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
			THEME_COLOR_RED;
		break;
	case 5: // Dark Blue
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
			THEME_COLOR_BLUE;
		break;
	case 6: // Purple
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
			THEME_COLOR_PURPLE;
		break;
	case 7: // Magenta
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
			THEME_COLOR_MAGENTA;
		break;
	case 8: // Pink
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
			THEME_COLOR_PINK;
		break;
	case 9: // Custom
		create_limiter_color_wheel_popup();
		break;
	}

	// Limiter circles color update removed - only bar flash effect is supported
}

void rpm_lights_switch_event_cb(lv_event_t *e) {
	lv_obj_t *switch_obj = lv_event_get_target(e);
	bool is_checked = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);

	// Update configuration
	values_config[RPM_VALUE_ID - 1].rpm_lights_enabled = is_checked;

	// If disabled, hide all RPM lights circles
	if (!is_checked) {
		for (int i = 0; i < 8; i++) {
			if (rpm_lights_circles[i] &&
				lv_obj_is_valid(rpm_lights_circles[i])) {
				lv_obj_add_flag(rpm_lights_circles[i], LV_OBJ_FLAG_HIDDEN);
			}
		}
	} else {
		// If enabled, create RPM lights circles if they don't exist
		if (rpm_lights_circles[0] == NULL) {
			lv_obj_t *current_screen = lv_scr_act();
			create_rpm_lights_circles(current_screen);
		}

		// Update RPM lights based on current RPM value
		update_rpm_lights(current_canbus_rpm);
	}
}

void rpm_background_switch_event_cb(lv_event_t *e) {
	lv_obj_t *switch_obj = lv_event_get_target(e);
	bool is_checked = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);

	// Update configuration
	values_config[RPM_VALUE_ID - 1].rpm_background_enabled = is_checked;
}

void rpm_background_color_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	// Determine new color based on selection - same colors as limiter
	lv_color_t new_background_color;
	switch (selected) {
	case 0:
		new_background_color = THEME_COLOR_GREEN;
		break; // Green
	case 1:
		new_background_color = THEME_COLOR_CYAN;
		break; // Light Blue
	case 2:
		new_background_color = THEME_COLOR_YELLOW;
		break; // Yellow
	case 3:
		new_background_color = THEME_COLOR_ORANGE;
		break; // Orange
	case 4:
		new_background_color = THEME_COLOR_RED;
		break; // Red
	case 5:
		new_background_color = THEME_COLOR_BLUE;
		break; // Blue
	case 6:
		new_background_color = THEME_COLOR_PURPLE;
		break; // Purple
	case 7:
		new_background_color = THEME_COLOR_MAGENTA;
		break; // Magenta
	case 8:
		new_background_color = THEME_COLOR_PINK;
		break; // Pink
	case 9:	   // Custom color - open color wheel popup
		create_rpm_background_color_wheel_popup();
		return; // Don't update color yet, wait for color wheel selection
	default:
		new_background_color = THEME_COLOR_GREEN;
		break;
	}

	// Update configuration
	values_config[RPM_VALUE_ID - 1].rpm_background_color = new_background_color;
}

void rpm_background_threshold_roller_event_cb(lv_event_t *e) {
	lv_obj_t *roller = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(roller);

	int32_t threshold_value = 3000 + (selected * 200); // 200 RPM steps from 3000 to 12000

	// Update configuration
	values_config[RPM_VALUE_ID - 1].rpm_background_value = threshold_value;
}

void bar_low_color_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	switch (selected) {
	case 0:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_BLUE_DARK;
		break; // Blue
	case 1:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_RED;
		break; // Red
	case 2:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_GREEN_BRIGHT;
		break; // Green
	case 3:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_YELLOW;
		break; // Yellow
	case 4:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_ORANGE;
		break; // Orange
	case 5:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_PURPLE;
		break; // Purple
	case 6:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_CYAN;
		break; // Cyan
	case 7:
		values_config[value_id - 1].bar_low_color = THEME_COLOR_MAGENTA;
		break; // Magenta
	case 8:	   // Custom color - open color wheel popup
		create_bar_low_color_wheel_popup(value_id);
		return; // Don't update color yet, wait for color wheel selection
	}
}

void bar_high_color_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	switch (selected) {
	case 0:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_BLUE_DARK;
		break; // Blue
	case 1:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_RED;
		break; // Red
	case 2:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_GREEN_BRIGHT;
		break; // Green
	case 3:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_YELLOW;
		break; // Yellow
	case 4:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_ORANGE;
		break; // Orange
	case 5:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_PURPLE;
		break; // Purple
	case 6:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_CYAN;
		break; // Cyan
	case 7:
		values_config[value_id - 1].bar_high_color = THEME_COLOR_MAGENTA;
		break; // Magenta
	case 8:	   // Custom color - open color wheel popup
		create_bar_high_color_wheel_popup(value_id);
		return; // Don't update color yet, wait for color wheel selection
	}
}

void bar_in_range_color_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);

	switch (selected) {
	case 0:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_BLUE_DARK;
		break; // Blue
	case 1:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_RED;
		break; // Red
	case 2:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_GREEN_BRIGHT;
		break; // Green
	case 3:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_YELLOW;
		break; // Yellow
	case 4:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_ORANGE;
		break; // Orange
	case 5:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_PURPLE;
		break; // Purple
	case 6:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_CYAN;
		break; // Cyan
	case 7:
		values_config[value_id - 1].bar_in_range_color = THEME_COLOR_MAGENTA;
		break; // Magenta
	case 8:	   // Custom color - open color wheel popup
		create_bar_in_range_color_wheel_popup(value_id);
		return; // Don't update color yet, wait for color wheel selection
	}
}

// Limiter circles color update function removed - only bar flash effect is
// supported

static void update_rpm_lights(int rpm_value) {
	// Only update if RPM lights are enabled and circles exist and not in
	// limiter demo mode and no real limiter effect is active
	if (!values_config[RPM_VALUE_ID - 1].rpm_lights_enabled ||
		rpm_lights_circles[0] == NULL || limiter_demo_active ||
		real_limiter_active) {
		return;
	}

	extern int rpm_gauge_max;
	if (rpm_gauge_max <= 0)
		return; // Avoid division by zero

	// Calculate which zone we're in (0-4)
	float rpm_percentage = (float)rpm_value / (float)rpm_gauge_max;
	if (rpm_percentage < 0)
		rpm_percentage = 0;
	if (rpm_percentage > 1)
		rpm_percentage = 1;

	int zone = (int)(rpm_percentage * 5);
	if (zone > 4)
		zone = 4;

	// Circle order: [7,0] [6,1] [5,2] [4,3] (outermost to innermost pairs)
	int circle_pairs[4][2] = {
		{7, 0}, // Outermost pair
		{6, 1}, // Second pair
		{5, 2}, // Third pair
		{4, 3}	// Innermost pair
	};

	// All lights use the RPM bar color
	lv_color_t rpm_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;

	// Update all circles
	for (int pair = 0; pair < 4; pair++) {
		bool should_show = (zone > pair); // Show if we've passed this zone
		lv_color_t color = rpm_color;	  // All lights use RPM color

		for (int j = 0; j < 2; j++) {
			int circle_idx = circle_pairs[pair][j];
			if (rpm_lights_circles[circle_idx] &&
				lv_obj_is_valid(rpm_lights_circles[circle_idx])) {
				if (should_show) {
					lv_obj_set_style_bg_color(rpm_lights_circles[circle_idx],
											  color,
											  LV_PART_MAIN | LV_STATE_DEFAULT);
					lv_obj_clear_flag(rpm_lights_circles[circle_idx],
									  LV_OBJ_FLAG_HIDDEN);
				} else {
					lv_obj_add_flag(rpm_lights_circles[circle_idx],
									LV_OBJ_FLAG_HIDDEN);
				}
			}
		}
	}
}

void create_rpm_lights_circles(lv_obj_t *parent) {
	// Use the same positions as warning circles but these are for RPM Lights
	// (background layer)
	const struct {
		int16_t x;
		int16_t y;
	} rpm_lights_positions[] = {
		{-352, -148}, // Position 1
		{-292, -148}, // Position 2
		{-232, -148}, // Position 3
		{-172, -148}, // Position 4
		{172, -148},  // Position 5
		{232, -148},  // Position 6
		{292, -148},  // Position 7
		{352, -148}	  // Position 8
	};

	for (int i = 0; i < 8; i++) {
		rpm_lights_circles[i] = lv_obj_create(parent);
		lv_obj_set_width(rpm_lights_circles[i], 15);
		lv_obj_set_height(rpm_lights_circles[i], 15);
		lv_obj_set_x(rpm_lights_circles[i], rpm_lights_positions[i].x);
		lv_obj_set_y(rpm_lights_circles[i], rpm_lights_positions[i].y);
		lv_obj_set_align(rpm_lights_circles[i], LV_ALIGN_CENTER);
		lv_obj_clear_flag(rpm_lights_circles[i], LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_radius(rpm_lights_circles[i], 100,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_color(rpm_lights_circles[i],
								  values_config[RPM_VALUE_ID - 1].rpm_bar_color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(rpm_lights_circles[i], 255,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(rpm_lights_circles[i], 0,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		// No outline for rpm circles
		lv_obj_set_style_outline_width(rpm_lights_circles[i], 0,
									   LV_PART_MAIN | LV_STATE_DEFAULT);

		// Initially hidden
		lv_obj_add_flag(rpm_lights_circles[i], LV_OBJ_FLAG_HIDDEN);
	}
}

// Limiter circles creation function removed - only bar flash effect is
// supported

static void limiter_demo_timeout_cb(lv_timer_t *timer) {
	stop_limiter_effect_demo();
}

static void limiter_flash_cb(lv_timer_t *timer) {
	// Safety check: if demo is no longer active, stop the timer
	if (!limiter_demo_active) {
		return;
	}

	limiter_flash_state = !limiter_flash_state;

	// Handle flash effects (type 2 = Bar only, type 3 = Bar & Circles, type 4 =
	// Circles only)
	if (current_effect_type == 2 || current_effect_type == 3 ||
		current_effect_type == 4) { // Flash effects
		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
			// Keep RPM bar at max value during demo, just like real limiter
			// effect
			extern int rpm_gauge_max;
			// Map RPM to extended bar range to properly fill the extended bar
			// width
			const float bar_extension_ratio = 782.5f / 765.0f;
			int32_t extended_rpm_max =
				(int32_t)(rpm_gauge_max * bar_extension_ratio);
			int32_t scaled_rpm =
				(rpm_gauge_max * extended_rpm_max) / rpm_gauge_max;
			lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);

			if (limiter_flash_state) {
				// Flash RPM bar and panel for Bar Flash (type 2) and Bar &
				// Circles Flash (type 3)
				if (current_effect_type == 2 || current_effect_type == 3) {
					// Flash RPM bar to limiter color
					lv_obj_set_style_bg_color(
						rpm_bar_gauge,
						values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
						LV_PART_INDICATOR | LV_STATE_DEFAULT);
					// Set gradient color to same as limiter color for solid
					// appearance
					lv_obj_set_style_bg_grad_color(
						rpm_bar_gauge,
						values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
						LV_PART_INDICATOR | LV_STATE_DEFAULT);
					lv_obj_set_style_bg_grad_dir(
						rpm_bar_gauge, LV_GRAD_DIR_NONE,
						LV_PART_INDICATOR | LV_STATE_DEFAULT);
					// Flash Panel 9 (left side) to limiter color
					if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
						lv_obj_set_style_bg_color(
							ui_Panel9,
							values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
							LV_PART_MAIN | LV_STATE_DEFAULT);
					}
				}

				// Flash circles for Bar & Circles Flash (type 3) and Circles
				// Flash (type 4)
				if (current_effect_type == 3 || current_effect_type == 4) {
					// Ultra-fast batch update for perfect circle
					// synchronization
					batch_update_rpm_circles_color(
						values_config[RPM_VALUE_ID - 1].rpm_limiter_color);
				}
			} else {
				// Restore RPM bar and panel for Bar Flash (type 2) and Bar &
				// Circles Flash (type 3)
				if (current_effect_type == 2 || current_effect_type == 3) {
					// Restore RPM bar to original color (but keep at max value)
					lv_obj_set_style_bg_color(rpm_bar_gauge, original_rpm_color,
											  LV_PART_INDICATOR |
												  LV_STATE_DEFAULT);
					// Set gradient color to same as original color for solid
					// appearance
					lv_obj_set_style_bg_grad_color(
						rpm_bar_gauge, original_rpm_color,
						LV_PART_INDICATOR | LV_STATE_DEFAULT);
					lv_obj_set_style_bg_grad_dir(
						rpm_bar_gauge, LV_GRAD_DIR_NONE,
						LV_PART_INDICATOR | LV_STATE_DEFAULT);
					// Restore Panel 9 to original color
					if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
						lv_obj_set_style_bg_color(ui_Panel9, original_rpm_color,
												  LV_PART_MAIN |
													  LV_STATE_DEFAULT);
					}
				}

				// Restore circles for Bar & Circles Flash (type 3) and Circles
				// Flash (type 4)
				if (current_effect_type == 3 || current_effect_type == 4) {
					// Ultra-fast batch restore for perfect circle
					// synchronization
					batch_update_rpm_circles_color(original_rpm_color);
				}
			}
		}
	}
}

static void clear_rpm_lights_circles(void) {
	// Clear the RPM lights circles array when RPM gauge is recreated
	for (int i = 0; i < 8; i++) {
		rpm_lights_circles[i] = NULL;
	}
}

// Ultra-fast batch update function for perfect circle synchronization
static void batch_update_rpm_circles_color(lv_color_t color) {
	if (!values_config[RPM_VALUE_ID - 1].rpm_lights_enabled ||
		!rpm_lights_circles[0]) {
		return; // Early exit if RPM lights are disabled or not initialized
	}

	// Single function call to update all circles with the same color for
	// perfect timing
	for (int i = 0; i < 8; i++) {
		if (rpm_lights_circles[i] && lv_obj_is_valid(rpm_lights_circles[i]) &&
			!lv_obj_has_flag(rpm_lights_circles[i], LV_OBJ_FLAG_HIDDEN)) {
			lv_obj_set_style_bg_color(rpm_lights_circles[i], color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	}
}

static void update_menu_rpm_value_text(int rpm_value) {
	// Update the RPM value text in menu screen during demos
	if (menu_rpm_value_label && lv_obj_is_valid(menu_rpm_value_label)) {
		// Apply same 102.3% scaling to the actual RPM value for consistency
		int display_rpm_value = (int)((float)rpm_value * 1.0229f);
		char rpm_text[16];
		snprintf(rpm_text, sizeof(rpm_text), "%d", display_rpm_value);
		lv_label_set_text(menu_rpm_value_label, rpm_text);
	}
}

void start_limiter_effect_demo(uint8_t effect_type) {
	// Stop any existing demo
	stop_limiter_effect_demo();

	if (effect_type == 0)
		return; // None selected

	// Save current RPM value before starting demo
	saved_rpm_before_demo = current_canbus_rpm;

	limiter_demo_active = true;
	limiter_flash_state = false;
	current_effect_type = effect_type;

	// Save original RPM color for effects
	original_rpm_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;

	// Immediately set panels to RPM color to avoid initial white flash
	if (ui_Panel9) {
		lv_obj_set_style_bg_color(ui_Panel9, original_rpm_color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	// Panel 10 stays white always - removed from demo effects

	// Set RPM to max for demo
	if (rpm_bar_gauge) {
		extern int rpm_gauge_max;
		// Map RPM to extended bar range to properly fill the extended bar width
		const float bar_extension_ratio = 782.5f / 765.0f;
		int32_t extended_rpm_max =
			(int32_t)(rpm_gauge_max * bar_extension_ratio);
		int32_t scaled_rpm = (rpm_gauge_max * extended_rpm_max) / rpm_gauge_max;
		lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
		// Update menu RPM value text to show max RPM
		update_menu_rpm_value_text(rpm_gauge_max);
	}

	// Handle different effect types
	if (effect_type == 5) {
		// Bar Solid effect - just set the limiter color immediately, no
		// flashing
		if (rpm_bar_gauge) {
			lv_obj_set_style_bg_color(
				rpm_bar_gauge,
				values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_color(
				rpm_bar_gauge,
				values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9) {
			lv_obj_set_style_bg_color(
				ui_Panel9, values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		// Only create the demo timeout timer for solid effect (no flash timer
		// needed)
		limiter_demo_timer = lv_timer_create(limiter_demo_timeout_cb, 1000,
											 NULL);		  // 1 second timeout
		lv_timer_set_repeat_count(limiter_demo_timer, 1); // Run only once
	} else if (effect_type == 6) {
		// Bar & Circles Solid effect - set both bar and circles to limiter
		// color immediately, no flashing
		if (rpm_bar_gauge) {
			lv_obj_set_style_bg_color(
				rpm_bar_gauge,
				values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_color(
				rpm_bar_gauge,
				values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
				LV_PART_INDICATOR | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9) {
			lv_obj_set_style_bg_color(
				ui_Panel9, values_config[RPM_VALUE_ID - 1].rpm_limiter_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		// Set circles to limiter color
		batch_update_rpm_circles_color(
			values_config[RPM_VALUE_ID - 1].rpm_limiter_color);
		// Only create the demo timeout timer for solid effect (no flash timer
		// needed)
		limiter_demo_timer = lv_timer_create(limiter_demo_timeout_cb, 1000,
											 NULL);		  // 1 second timeout
		lv_timer_set_repeat_count(limiter_demo_timer, 1); // Run only once
	} else if (effect_type == 7) {
		// Circles Solid effect - only set circles to limiter color immediately,
		// no flashing, bar stays at original color Set circles to limiter color
		batch_update_rpm_circles_color(
			values_config[RPM_VALUE_ID - 1].rpm_limiter_color);
		// Only create the demo timeout timer for solid effect (no flash timer
		// needed)
		limiter_demo_timer = lv_timer_create(limiter_demo_timeout_cb, 1000,
											 NULL);		  // 1 second timeout
		lv_timer_set_repeat_count(limiter_demo_timer, 1); // Run only once
	} else {
		// Flash effects supported (type 2 = Bar only, type 3 = Bar & Circles,
		// type 4 = Circles only)

		// Create LVGL timers for perfect synchronization with real limiter
		limiter_flash_timer =
			lv_timer_create(limiter_flash_cb, 100,
							NULL); // 100ms flash rate - same as real limiter
		limiter_demo_timer = lv_timer_create(limiter_demo_timeout_cb, 1000,
											 NULL);		  // 1 second timeout
		lv_timer_set_repeat_count(limiter_demo_timer, 1); // Run only once
	}
}

void stop_limiter_effect_demo(void) {
	if (!limiter_demo_active)
		return;

	limiter_demo_active = false;
	limiter_flash_state = false; // Reset flash state

	// Stop and delete LVGL timers
	if (limiter_demo_timer) {
		lv_timer_del(limiter_demo_timer);
		limiter_demo_timer = NULL;
	}
	if (limiter_flash_timer) {
		lv_timer_del(limiter_flash_timer);
		limiter_flash_timer = NULL;
	}

	// Limiter circles hiding removed - only bar flash effect is supported

	// Restore original RPM bar color and side panels
	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
		lv_obj_set_style_bg_color(rpm_bar_gauge, original_rpm_color,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		// Set gradient color to same as original color for solid appearance
		lv_obj_set_style_bg_grad_color(rpm_bar_gauge, original_rpm_color,
									   LV_PART_INDICATOR | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
									 LV_PART_INDICATOR | LV_STATE_DEFAULT);
	}
	// Restore Panel 9 to original RPM color
	if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
		lv_obj_set_style_bg_color(ui_Panel9, original_rpm_color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	// Restore RPM value to saved value (either CAN bus value or 0)
	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
		// Map RPM to extended bar range to properly fill the extended bar width
		const float bar_extension_ratio = 782.5f / 765.0f;
		int32_t extended_rpm_max =
			(int32_t)(rpm_gauge_max * bar_extension_ratio);
		int32_t scaled_rpm =
			(saved_rpm_before_demo * extended_rpm_max) / rpm_gauge_max;
		lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
	}

	// Restore menu RPM value text to saved value
	update_menu_rpm_value_text(saved_rpm_before_demo);

	// If RPM lights were enabled, update them for the restored RPM value
	if (values_config[RPM_VALUE_ID - 1].rpm_lights_enabled) {
		update_rpm_lights(saved_rpm_before_demo);
	}
}

// Real limiter effect implementation (triggered by actual RPM)
static lv_timer_t *real_limiter_flash_timer = NULL;
static bool real_limiter_flash_state = false;
static uint8_t real_limiter_effect_type = 0;

static void real_limiter_flash_cb(lv_timer_t *timer) {
	// Toggle flash state
	real_limiter_flash_state = !real_limiter_flash_state;

	// Get current colors
	lv_color_t rpm_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;
	lv_color_t limiter_color =
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color;

	// Handle flash effects (type 2 = Bar only, type 3 = Bar & Circles, type 4 =
	// Circles only)

	// Flash RPM bar and panel for Bar Flash (type 2) and Bar & Circles Flash
	// (type 3)
	if (real_limiter_effect_type == 2 || real_limiter_effect_type == 3) {
		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
			lv_color_t current_color =
				real_limiter_flash_state ? limiter_color : rpm_color;
			lv_obj_set_style_bg_color(rpm_bar_gauge, current_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
			// Set gradient color to same as current color for solid appearance
			lv_obj_set_style_bg_grad_color(rpm_bar_gauge, current_color,
										   LV_PART_INDICATOR |
											   LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
			lv_obj_set_style_bg_color(
				ui_Panel9, real_limiter_flash_state ? limiter_color : rpm_color,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	}

	// Flash circles for Bar & Circles Flash (type 3) and Circles Flash (type 4)
	if (real_limiter_effect_type == 3 || real_limiter_effect_type == 4) {
		// Ultra-fast batch update for perfect circle synchronization
		lv_color_t circle_color =
			real_limiter_flash_state ? limiter_color : rpm_color;
		batch_update_rpm_circles_color(circle_color);
	}
}

static void start_real_limiter_effect(uint8_t effect_type) {
	// Don't start if already active or no effect selected
	if (real_limiter_active || effect_type == 0)
		return;

	// Additional check: if timer already exists, we're already running
	if (real_limiter_flash_timer != NULL)
		return;

	real_limiter_active = true;
	real_limiter_flash_state = false;
	real_limiter_effect_type = effect_type;

	if (effect_type == 5) {
		// Bar Solid effect - just set the limiter color immediately, no
		// flashing timer needed
		lv_color_t limiter_color =
			values_config[RPM_VALUE_ID - 1].rpm_limiter_color;

		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
			lv_obj_set_style_bg_color(rpm_bar_gauge, limiter_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_color(rpm_bar_gauge, limiter_color,
										   LV_PART_INDICATOR |
											   LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
			lv_obj_set_style_bg_color(ui_Panel9, limiter_color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		// No timer needed for solid effect
	} else if (effect_type == 6) {
		// Bar & Circles Solid effect - set both bar and circles to limiter
		// color immediately, no flashing timer needed
		lv_color_t limiter_color =
			values_config[RPM_VALUE_ID - 1].rpm_limiter_color;

		if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
			lv_obj_set_style_bg_color(rpm_bar_gauge, limiter_color,
									  LV_PART_INDICATOR | LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_color(rpm_bar_gauge, limiter_color,
										   LV_PART_INDICATOR |
											   LV_STATE_DEFAULT);
			lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
										 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		}
		if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
			lv_obj_set_style_bg_color(ui_Panel9, limiter_color,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		// Set circles to limiter color
		batch_update_rpm_circles_color(limiter_color);
		// No timer needed for solid effect
	} else if (effect_type == 7) {
		// Circles Solid effect - only set circles to limiter color immediately,
		// no flashing timer needed, bar stays at normal color
		lv_color_t limiter_color =
			values_config[RPM_VALUE_ID - 1].rpm_limiter_color;

		// Set circles to limiter color
		batch_update_rpm_circles_color(limiter_color);
		// No timer needed for solid effect
	} else {
		// Flash effects supported (type 2 = Bar only, type 3 = Bar & Circles,
		// type 4 = Circles only)

		// Create LVGL timer instead of ESP timer for better coordination
		real_limiter_flash_timer = lv_timer_create(real_limiter_flash_cb, 100,
												   NULL); // 100ms flash rate
	}
}

static void stop_real_limiter_effect(void) {
	if (!real_limiter_active)
		return;

	real_limiter_active = false;

	// Stop and delete LVGL timer
	if (real_limiter_flash_timer) {
		lv_timer_del(real_limiter_flash_timer);
		real_limiter_flash_timer = NULL;
	}

	// Restore original colors and RPM bar value
	lv_color_t rpm_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;

	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
		lv_obj_set_style_bg_color(rpm_bar_gauge, rpm_color,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
		// Set gradient color to same as RPM color for solid appearance
		lv_obj_set_style_bg_grad_color(rpm_bar_gauge, rpm_color,
									   LV_PART_INDICATOR | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
									 LV_PART_INDICATOR | LV_STATE_DEFAULT);
		// Restore the bar to show the current actual CAN bus RPM value
		// Map RPM to extended bar range to properly fill the extended bar width
		extern int rpm_gauge_max;
		const float bar_extension_ratio = 782.5f / 765.0f;
		int32_t extended_rpm_max =
			(int32_t)(rpm_gauge_max * bar_extension_ratio);
		int32_t scaled_rpm =
			(current_canbus_rpm * extended_rpm_max) / rpm_gauge_max;
		lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
	}
	if (ui_Panel9 && lv_obj_is_valid(ui_Panel9)) {
		lv_obj_set_style_bg_color(ui_Panel9, rpm_color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	// Restore circles to original color for effects that involve circles (types
	// 3, 4, 6, 7)
	if (real_limiter_effect_type == 3 || real_limiter_effect_type == 4 ||
		real_limiter_effect_type == 6 || real_limiter_effect_type == 7) {
		batch_update_rpm_circles_color(rpm_color);
		// Update RPM lights to properly restore the state based on current RPM
		// value This ensures circles show the correct state rather than all
		// being lit solid
		update_rpm_lights(current_canbus_rpm);
	}
}

// Global variables for color wheel popup
static lv_obj_t *color_wheel_popup = NULL;
static lv_obj_t *color_wheel = NULL;
static lv_color_t selected_custom_color;

// Global variables for RPM background color wheel popup
static lv_obj_t *rpm_background_color_wheel_popup = NULL;
static lv_obj_t *rpm_background_color_wheel = NULL;
static lv_color_t selected_rpm_background_custom_color;

// Global variables for RPM background functionality
static bool rpm_background_active = false;
static lv_color_t original_screen_bg_color;
static bool original_screen_bg_color_saved = false;

// Color wheel popup event callbacks
static void color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	// But don't update if real limiter effect is active to avoid conflicts
	if (!real_limiter_active) {
		new_rpm_color = selected_custom_color;
		rpm_color_needs_update = true;
	}
	values_config[RPM_VALUE_ID - 1].rpm_bar_color = selected_custom_color;

	// Close the popup
	if (color_wheel_popup) {
		lv_obj_del(color_wheel_popup);
		color_wheel_popup = NULL;
		color_wheel = NULL;
	}
}

static void color_wheel_cancel_event_cb(lv_event_t *e) {
	// Just close the popup without applying changes
	if (color_wheel_popup) {
		lv_obj_del(color_wheel_popup);
		color_wheel_popup = NULL;
		color_wheel = NULL;
	}
}

static void color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_custom_color = lv_colorwheel_get_rgb(colorwheel);

	// Show live preview by updating the RPM bar immediately
	// But don't update if real limiter effect is active to avoid conflicts
	if (!real_limiter_active) {
		new_rpm_color = selected_custom_color;
		rpm_color_needs_update = true;
	}
}

// RPM Background color wheel popup event callbacks
static void rpm_background_color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	values_config[RPM_VALUE_ID - 1].rpm_background_color =
		selected_rpm_background_custom_color;

	// Close the popup
	if (rpm_background_color_wheel_popup) {
		lv_obj_del(rpm_background_color_wheel_popup);
		rpm_background_color_wheel_popup = NULL;
		rpm_background_color_wheel = NULL;
	}
}

static void rpm_background_color_wheel_cancel_event_cb(lv_event_t *e) {
	// Just close the popup without applying changes
	if (rpm_background_color_wheel_popup) {
		lv_obj_del(rpm_background_color_wheel_popup);
		rpm_background_color_wheel_popup = NULL;
		rpm_background_color_wheel = NULL;
	}
}

static void rpm_background_color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_rpm_background_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

void create_rpm_background_color_wheel_popup(void) {
	// Don't create multiple popups
	if (rpm_background_color_wheel_popup)
		return;

	// Create popup background
	rpm_background_color_wheel_popup = lv_obj_create(lv_scr_act());
	lv_obj_set_size(rpm_background_color_wheel_popup, 400, 350);
	lv_obj_center(rpm_background_color_wheel_popup);
	lv_obj_set_style_bg_color(rpm_background_color_wheel_popup,
							  THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(rpm_background_color_wheel_popup,
								  THEME_COLOR_BORDER_MED,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(rpm_background_color_wheel_popup, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(rpm_background_color_wheel_popup, 10,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_width(rpm_background_color_wheel_popup, 15,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(rpm_background_color_wheel_popup,
								  THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(rpm_background_color_wheel_popup, 150,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title label
	lv_obj_t *title_label = lv_label_create(rpm_background_color_wheel_popup);
	lv_label_set_text(title_label, "Select Custom Background Colour");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);

	// Create color wheel
	rpm_background_color_wheel =
		lv_colorwheel_create(rpm_background_color_wheel_popup, true);
	lv_obj_set_size(rpm_background_color_wheel, 200, 200);
	lv_obj_align(rpm_background_color_wheel, LV_ALIGN_CENTER, 0, -10);

	// Set initial color to current background color
	lv_color_t current_color =
		values_config[RPM_VALUE_ID - 1].rpm_background_color;
	lv_colorwheel_set_rgb(rpm_background_color_wheel, current_color);
	selected_rpm_background_custom_color = current_color;

	// Add color wheel change event
	lv_obj_add_event_cb(rpm_background_color_wheel,
						rpm_background_color_wheel_value_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	// OK button
	lv_obj_t *ok_btn = lv_btn_create(rpm_background_color_wheel_popup);
	lv_obj_set_size(ok_btn, 80, 35);
	lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_set_style_bg_color(ok_btn, THEME_COLOR_BTN_SAVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *ok_label = lv_label_create(ok_btn);
	lv_label_set_text(ok_label, "OK");
	lv_obj_set_style_text_color(ok_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(ok_label);
	lv_obj_add_event_cb(ok_btn, rpm_background_color_wheel_ok_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Cancel button
	lv_obj_t *cancel_btn = lv_btn_create(rpm_background_color_wheel_popup);
	lv_obj_set_size(cancel_btn, 80, 35);
	lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_CANCEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *cancel_label = lv_label_create(cancel_btn);
	lv_label_set_text(cancel_label, "Cancel");
	lv_obj_set_style_text_color(cancel_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(cancel_label);
	lv_obj_add_event_cb(cancel_btn, rpm_background_color_wheel_cancel_event_cb,
						LV_EVENT_CLICKED, NULL);
}

void create_rpm_color_wheel_popup(void) {
	// Don't create multiple popups
	if (color_wheel_popup)
		return;

	// Create popup background
	color_wheel_popup = lv_obj_create(lv_scr_act());
	lv_obj_set_size(color_wheel_popup, 400, 350);
	lv_obj_center(color_wheel_popup);
	lv_obj_set_style_bg_color(color_wheel_popup, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(color_wheel_popup, THEME_COLOR_BORDER_MED,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(color_wheel_popup, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(color_wheel_popup, 10,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_width(color_wheel_popup, 15,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(color_wheel_popup, THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(color_wheel_popup, 150,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title label
	lv_obj_t *title_label = lv_label_create(color_wheel_popup);
	lv_label_set_text(title_label, "Select Custom RPM Colour");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);

	// Create color wheel
	color_wheel = lv_colorwheel_create(color_wheel_popup, true);
	lv_obj_set_size(color_wheel, 200, 200);
	lv_obj_align(color_wheel, LV_ALIGN_CENTER, 0, -10);

	// Set initial color to current RPM color
	lv_color_t current_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;
	lv_colorwheel_set_rgb(color_wheel, current_color);
	selected_custom_color = current_color;

	// Add color wheel change event
	lv_obj_add_event_cb(color_wheel, color_wheel_value_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	// OK button
	lv_obj_t *ok_btn = lv_btn_create(color_wheel_popup);
	lv_obj_set_size(ok_btn, 80, 35);
	lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_set_style_bg_color(ok_btn, THEME_COLOR_BTN_SAVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *ok_label = lv_label_create(ok_btn);
	lv_label_set_text(ok_label, "OK");
	lv_obj_set_style_text_color(ok_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(ok_label);

	lv_obj_add_event_cb(ok_btn, color_wheel_ok_event_cb, LV_EVENT_CLICKED,
						NULL);

	// Cancel button
	lv_obj_t *cancel_btn = lv_btn_create(color_wheel_popup);
	lv_obj_set_size(cancel_btn, 80, 35);
	lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_CANCEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *cancel_label = lv_label_create(cancel_btn);
	lv_label_set_text(cancel_label, "Cancel");
	lv_obj_set_style_text_color(cancel_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(cancel_label);

	lv_obj_add_event_cb(cancel_btn, color_wheel_cancel_event_cb,
						LV_EVENT_CLICKED, NULL);
}

// Global variables for limiter color wheel popup
static lv_obj_t *limiter_color_wheel_popup = NULL;
static lv_obj_t *limiter_color_wheel = NULL;
static lv_color_t selected_limiter_custom_color;

// Global variables for bar color wheel popups
static lv_obj_t *bar_low_color_wheel_popup = NULL;
static lv_obj_t *bar_low_color_wheel = NULL;
static lv_color_t selected_bar_low_custom_color;
static uint8_t bar_low_color_value_id = 0;

static lv_obj_t *bar_high_color_wheel_popup = NULL;
static lv_obj_t *bar_high_color_wheel = NULL;
static lv_color_t selected_bar_high_custom_color;
static uint8_t bar_high_color_value_id = 0;

static lv_obj_t *bar_in_range_color_wheel_popup = NULL;
static lv_obj_t *bar_in_range_color_wheel = NULL;
static lv_color_t selected_bar_in_range_custom_color;
static uint8_t bar_in_range_color_value_id = 0;

// Limiter color wheel popup event callbacks
static void limiter_color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
		selected_limiter_custom_color;

	// Limiter circles color update removed - only bar flash effect is supported

	// Close the popup
	if (limiter_color_wheel_popup) {
		lv_obj_del(limiter_color_wheel_popup);
		limiter_color_wheel_popup = NULL;
		limiter_color_wheel = NULL;
	}
}

static void limiter_color_wheel_cancel_event_cb(lv_event_t *e) {
	// Just close the popup without applying changes
	if (limiter_color_wheel_popup) {
		lv_obj_del(limiter_color_wheel_popup);
		limiter_color_wheel_popup = NULL;
		limiter_color_wheel = NULL;
	}
}

static void limiter_color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_limiter_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

// Bar color wheel popup event callbacks
static void bar_low_color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	values_config[bar_low_color_value_id - 1].bar_low_color =
		selected_bar_low_custom_color;

	// Close the popup
	if (bar_low_color_wheel_popup) {
		lv_obj_del(bar_low_color_wheel_popup);
		bar_low_color_wheel_popup = NULL;
		bar_low_color_wheel = NULL;
	}
}

static void bar_low_color_wheel_cancel_event_cb(lv_event_t *e) {
	// Just close the popup without applying changes
	if (bar_low_color_wheel_popup) {
		lv_obj_del(bar_low_color_wheel_popup);
		bar_low_color_wheel_popup = NULL;
		bar_low_color_wheel = NULL;
	}
}

static void bar_low_color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_bar_low_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

static void bar_high_color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	values_config[bar_high_color_value_id - 1].bar_high_color =
		selected_bar_high_custom_color;

	// Close the popup
	if (bar_high_color_wheel_popup) {
		lv_obj_del(bar_high_color_wheel_popup);
		bar_high_color_wheel_popup = NULL;
		bar_high_color_wheel = NULL;
	}
}

static void bar_high_color_wheel_cancel_event_cb(lv_event_t *e) {
	// Just close the popup without applying changes
	if (bar_high_color_wheel_popup) {
		lv_obj_del(bar_high_color_wheel_popup);
		bar_high_color_wheel_popup = NULL;
		bar_high_color_wheel = NULL;
	}
}

static void bar_high_color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_bar_high_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

static void bar_in_range_color_wheel_ok_event_cb(lv_event_t *e) {
	// Apply the selected color from the color wheel
	values_config[bar_in_range_color_value_id - 1].bar_in_range_color =
		selected_bar_in_range_custom_color;

	// Close the popup
	if (bar_in_range_color_wheel_popup) {
		lv_obj_del(bar_in_range_color_wheel_popup);
		bar_in_range_color_wheel_popup = NULL;
		bar_in_range_color_wheel = NULL;
	}
}

static void bar_in_range_color_wheel_cancel_event_cb(lv_event_t *e) {
	// Just close the popup without applying changes
	if (bar_in_range_color_wheel_popup) {
		lv_obj_del(bar_in_range_color_wheel_popup);
		bar_in_range_color_wheel_popup = NULL;
		bar_in_range_color_wheel = NULL;
	}
}

static void bar_in_range_color_wheel_value_changed_cb(lv_event_t *e) {
	// Update the selected color as user moves the color wheel
	lv_obj_t *colorwheel = lv_event_get_target(e);
	selected_bar_in_range_custom_color = lv_colorwheel_get_rgb(colorwheel);
}

void create_limiter_color_wheel_popup(void) {
	// Don't create multiple popups
	if (limiter_color_wheel_popup)
		return;

	// Create popup background
	limiter_color_wheel_popup = lv_obj_create(lv_scr_act());
	lv_obj_set_size(limiter_color_wheel_popup, 400, 350);
	lv_obj_center(limiter_color_wheel_popup);
	lv_obj_set_style_bg_color(limiter_color_wheel_popup, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(limiter_color_wheel_popup,
								  THEME_COLOR_BORDER_MED,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(limiter_color_wheel_popup, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(limiter_color_wheel_popup, 10,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_width(limiter_color_wheel_popup, 15,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(limiter_color_wheel_popup,
								  THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(limiter_color_wheel_popup, 150,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title label
	lv_obj_t *title_label = lv_label_create(limiter_color_wheel_popup);
	lv_label_set_text(title_label, "Select Custom Limiter Colour");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);

	// Create color wheel
	limiter_color_wheel = lv_colorwheel_create(limiter_color_wheel_popup, true);
	lv_obj_set_size(limiter_color_wheel, 200, 200);
	lv_obj_align(limiter_color_wheel, LV_ALIGN_CENTER, 0, -10);

	// Set initial color to current limiter color
	lv_color_t current_color =
		values_config[RPM_VALUE_ID - 1].rpm_limiter_color;
	lv_colorwheel_set_rgb(limiter_color_wheel, current_color);
	selected_limiter_custom_color = current_color;

	// Add color wheel change event
	lv_obj_add_event_cb(limiter_color_wheel,
						limiter_color_wheel_value_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	// OK button
	lv_obj_t *ok_btn = lv_btn_create(limiter_color_wheel_popup);
	lv_obj_set_size(ok_btn, 80, 35);
	lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_set_style_bg_color(ok_btn, THEME_COLOR_BTN_SAVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *ok_label = lv_label_create(ok_btn);
	lv_label_set_text(ok_label, "OK");
	lv_obj_set_style_text_color(ok_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(ok_label);

	lv_obj_add_event_cb(ok_btn, limiter_color_wheel_ok_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Cancel button
	lv_obj_t *cancel_btn = lv_btn_create(limiter_color_wheel_popup);
	lv_obj_set_size(cancel_btn, 80, 35);
	lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_CANCEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *cancel_label = lv_label_create(cancel_btn);
	lv_label_set_text(cancel_label, "Cancel");
	lv_obj_set_style_text_color(cancel_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(cancel_label);

	lv_obj_add_event_cb(cancel_btn, limiter_color_wheel_cancel_event_cb,
						LV_EVENT_CLICKED, NULL);
}

void create_bar_low_color_wheel_popup(uint8_t value_id) {
	// Don't create multiple popups
	if (bar_low_color_wheel_popup)
		return;

	// Store the value ID for the callback
	bar_low_color_value_id = value_id;

	// Create popup background
	bar_low_color_wheel_popup = lv_obj_create(lv_scr_act());
	lv_obj_set_size(bar_low_color_wheel_popup, 400, 350);
	lv_obj_center(bar_low_color_wheel_popup);
	lv_obj_set_style_bg_color(bar_low_color_wheel_popup, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(bar_low_color_wheel_popup,
								  THEME_COLOR_BORDER_MED,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(bar_low_color_wheel_popup, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(bar_low_color_wheel_popup, 10,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_width(bar_low_color_wheel_popup, 15,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(bar_low_color_wheel_popup,
								  THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(bar_low_color_wheel_popup, 150,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title label
	lv_obj_t *title_label = lv_label_create(bar_low_color_wheel_popup);
	lv_label_set_text(title_label, "Select Custom Bar Low Colour");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);

	// Create color wheel
	bar_low_color_wheel = lv_colorwheel_create(bar_low_color_wheel_popup, true);
	lv_obj_set_size(bar_low_color_wheel, 200, 200);
	lv_obj_align(bar_low_color_wheel, LV_ALIGN_CENTER, 0, -10);

	// Set initial color to current bar low color
	lv_color_t current_color = values_config[value_id - 1].bar_low_color;
	lv_colorwheel_set_rgb(bar_low_color_wheel, current_color);
	selected_bar_low_custom_color = current_color;

	// Add color wheel change event
	lv_obj_add_event_cb(bar_low_color_wheel,
						bar_low_color_wheel_value_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	// OK button
	lv_obj_t *ok_btn = lv_btn_create(bar_low_color_wheel_popup);
	lv_obj_set_size(ok_btn, 80, 35);
	lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_set_style_bg_color(ok_btn, THEME_COLOR_BTN_SAVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *ok_label = lv_label_create(ok_btn);
	lv_label_set_text(ok_label, "OK");
	lv_obj_set_style_text_color(ok_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(ok_label);

	lv_obj_add_event_cb(ok_btn, bar_low_color_wheel_ok_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Cancel button
	lv_obj_t *cancel_btn = lv_btn_create(bar_low_color_wheel_popup);
	lv_obj_set_size(cancel_btn, 80, 35);
	lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_CANCEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *cancel_label = lv_label_create(cancel_btn);
	lv_label_set_text(cancel_label, "Cancel");
	lv_obj_set_style_text_color(cancel_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(cancel_label);

	lv_obj_add_event_cb(cancel_btn, bar_low_color_wheel_cancel_event_cb,
						LV_EVENT_CLICKED, NULL);
}

void create_bar_high_color_wheel_popup(uint8_t value_id) {
	// Don't create multiple popups
	if (bar_high_color_wheel_popup)
		return;

	// Store the value ID for the callback
	bar_high_color_value_id = value_id;

	// Create popup background
	bar_high_color_wheel_popup = lv_obj_create(lv_scr_act());
	lv_obj_set_size(bar_high_color_wheel_popup, 400, 350);
	lv_obj_center(bar_high_color_wheel_popup);
	lv_obj_set_style_bg_color(bar_high_color_wheel_popup,
							  THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(bar_high_color_wheel_popup,
								  THEME_COLOR_BORDER_MED,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(bar_high_color_wheel_popup, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(bar_high_color_wheel_popup, 10,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_width(bar_high_color_wheel_popup, 15,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(bar_high_color_wheel_popup,
								  THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(bar_high_color_wheel_popup, 150,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title label
	lv_obj_t *title_label = lv_label_create(bar_high_color_wheel_popup);
	lv_label_set_text(title_label, "Select Custom Bar High Colour");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);

	// Create color wheel
	bar_high_color_wheel =
		lv_colorwheel_create(bar_high_color_wheel_popup, true);
	lv_obj_set_size(bar_high_color_wheel, 200, 200);
	lv_obj_align(bar_high_color_wheel, LV_ALIGN_CENTER, 0, -10);

	// Set initial color to current bar high color
	lv_color_t current_color = values_config[value_id - 1].bar_high_color;
	lv_colorwheel_set_rgb(bar_high_color_wheel, current_color);
	selected_bar_high_custom_color = current_color;

	// Add color wheel change event
	lv_obj_add_event_cb(bar_high_color_wheel,
						bar_high_color_wheel_value_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	// OK button
	lv_obj_t *ok_btn = lv_btn_create(bar_high_color_wheel_popup);
	lv_obj_set_size(ok_btn, 80, 35);
	lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_set_style_bg_color(ok_btn, THEME_COLOR_BTN_SAVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *ok_label = lv_label_create(ok_btn);
	lv_label_set_text(ok_label, "OK");
	lv_obj_set_style_text_color(ok_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(ok_label);

	lv_obj_add_event_cb(ok_btn, bar_high_color_wheel_ok_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Cancel button
	lv_obj_t *cancel_btn = lv_btn_create(bar_high_color_wheel_popup);
	lv_obj_set_size(cancel_btn, 80, 35);
	lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_CANCEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *cancel_label = lv_label_create(cancel_btn);
	lv_label_set_text(cancel_label, "Cancel");
	lv_obj_set_style_text_color(cancel_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(cancel_label);

	lv_obj_add_event_cb(cancel_btn, bar_high_color_wheel_cancel_event_cb,
						LV_EVENT_CLICKED, NULL);
}

void create_bar_in_range_color_wheel_popup(uint8_t value_id) {
	// Don't create multiple popups
	if (bar_in_range_color_wheel_popup)
		return;

	// Store the value ID for the callback
	bar_in_range_color_value_id = value_id;

	// Create popup background
	bar_in_range_color_wheel_popup = lv_obj_create(lv_scr_act());
	lv_obj_set_size(bar_in_range_color_wheel_popup, 400, 350);
	lv_obj_center(bar_in_range_color_wheel_popup);
	lv_obj_set_style_bg_color(bar_in_range_color_wheel_popup,
							  THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(bar_in_range_color_wheel_popup,
								  THEME_COLOR_BORDER_MED,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(bar_in_range_color_wheel_popup, 2,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(bar_in_range_color_wheel_popup, 10,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_width(bar_in_range_color_wheel_popup, 15,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(bar_in_range_color_wheel_popup,
								  THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(bar_in_range_color_wheel_popup, 150,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title label
	lv_obj_t *title_label = lv_label_create(bar_in_range_color_wheel_popup);
	lv_label_set_text(title_label, "Select Custom Bar In-Range Colour");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(title_label, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);

	// Create color wheel
	bar_in_range_color_wheel =
		lv_colorwheel_create(bar_in_range_color_wheel_popup, true);
	lv_obj_set_size(bar_in_range_color_wheel, 200, 200);
	lv_obj_align(bar_in_range_color_wheel, LV_ALIGN_CENTER, 0, -10);

	// Set initial color to current bar in-range color
	lv_color_t current_color = values_config[value_id - 1].bar_in_range_color;
	lv_colorwheel_set_rgb(bar_in_range_color_wheel, current_color);
	selected_bar_in_range_custom_color = current_color;

	// Add color wheel change event
	lv_obj_add_event_cb(bar_in_range_color_wheel,
						bar_in_range_color_wheel_value_changed_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	// OK button
	lv_obj_t *ok_btn = lv_btn_create(bar_in_range_color_wheel_popup);
	lv_obj_set_size(ok_btn, 80, 35);
	lv_obj_align(ok_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_set_style_bg_color(ok_btn, THEME_COLOR_BTN_SAVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ok_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *ok_label = lv_label_create(ok_btn);
	lv_label_set_text(ok_label, "OK");
	lv_obj_set_style_text_color(ok_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(ok_label);

	lv_obj_add_event_cb(ok_btn, bar_in_range_color_wheel_ok_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Cancel button
	lv_obj_t *cancel_btn = lv_btn_create(bar_in_range_color_wheel_popup);
	lv_obj_set_size(cancel_btn, 80, 35);
	lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_CANCEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(cancel_btn, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *cancel_label = lv_label_create(cancel_btn);
	lv_label_set_text(cancel_label, "Cancel");
	lv_obj_set_style_text_color(cancel_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(cancel_label);

	lv_obj_add_event_cb(cancel_btn, bar_in_range_color_wheel_cancel_event_cb,
						LV_EVENT_CLICKED, NULL);
}

void warning_high_threshold_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *textarea = lv_event_get_target(e);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
		const char *txt = lv_textarea_get_text(textarea);
		values_config[value_id - 1].warning_high_threshold = atof(txt);
		values_config[value_id - 1].warning_high_enabled = true;
	}
}

void warning_low_threshold_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *textarea = lv_event_get_target(e);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
		const char *txt = lv_textarea_get_text(textarea);
		values_config[value_id - 1].warning_low_threshold = atof(txt);
		values_config[value_id - 1].warning_low_enabled = true;
	}
}

void warning_high_color_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *dropdown = lv_event_get_target(e);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
		uint16_t selected = lv_dropdown_get_selected(dropdown);
		values_config[value_id - 1].warning_high_color =
			selected == 0 ? THEME_COLOR_RED : THEME_COLOR_BLUE_DARK;
	}
}

void warning_low_color_event_cb(lv_event_t *e) {
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
		lv_obj_t *dropdown = lv_event_get_target(e);
		uint8_t value_id = *(uint8_t *)lv_event_get_user_data(e);
		uint16_t selected = lv_dropdown_get_selected(dropdown);
		values_config[value_id - 1].warning_low_color =
			selected == 0 ? THEME_COLOR_RED : THEME_COLOR_BLUE_DARK;
	}
}

static void warning_longpress_cb(lv_event_t *e) {
	uint8_t warning_idx = *(uint8_t *)lv_event_get_user_data(e);
	create_warning_config_menu(warning_idx);
}

static void indicator_longpress_cb(lv_event_t *e) {
	void *user_data = lv_event_get_user_data(e);
	if (!user_data) {
		printf("Error: No user data in indicator longpress callback\n");
		return;
	}

	uint8_t indicator_idx = *(uint8_t *)user_data;
	printf("Indicator longpress detected for indicator %d\n", indicator_idx);
	create_indicator_config_menu(indicator_idx);
}

static void label_text_cb(lv_event_t *e) {
	lv_obj_t *textarea = lv_event_get_target(e);
	warning_save_data_t *data =
		(warning_save_data_t *)lv_event_get_user_data(e);
	const char *txt = lv_textarea_get_text(textarea);
	if (data->preview_objects && data->preview_objects[1]) {
		lv_label_set_text(data->preview_objects[1], txt);
	}
}

static void color_dropdown_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	warning_save_data_t *data =
		(warning_save_data_t *)lv_event_get_user_data(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);
	lv_color_t color;
	switch (selected) {
	case 0:
		color = THEME_COLOR_GREEN;
		break; // Green
	case 1:
		color = THEME_COLOR_BLUE_PURE;
		break; // Blue
	case 2:
		color = THEME_COLOR_ORANGE_WEB;
		break; // Orange
	case 3:
		color = THEME_COLOR_RED;
		break; // Red
	case 4:
		color = THEME_COLOR_YELLOW;
		break; // Yellow
	default:
		color = THEME_COLOR_GREEN;
		break;
	}
	if (data->preview_objects && data->preview_objects[0]) {
		lv_obj_set_style_bg_color(data->preview_objects[0], color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	}
}

// Structure for preconfigured warnings
typedef struct {
	const char *label;
	const char *can_id_hex;
	uint8_t bit_position;
	uint8_t endianess;	  // 0 = Big, 1 = Little
	uint8_t color_index;  // 0=Green, 1=Blue, 2=Orange, 3=Red, 4=Yellow
	uint8_t is_momentary; // 0 = On/Off, 1 = Momentary
} warning_preconfig_t;

// Ford BA/BF/FG preconfigured warnings
static const warning_preconfig_t ford_babf_warnings[] = {
	{"Foglights On", "128", 2, 1, 2, 1},	 // Orange, Momentary
	{"High Beams On", "128", 3, 1, 0, 1},	 // Green, Momentary
	{"Driver Door", "403", 0, 1, 2, 1},		 // Orange, Momentary
	{"Passenger Door", "403", 1, 1, 2, 1},	 // Orange, Momentary
	{"Rear Doors", "403", 3, 1, 2, 1},		 // Orange, Momentary
	{"Cruise On", "425", 0, 1, 0, 1},		 // Green, Momentary
	{"Cruise Set", "425", 1, 1, 1, 1},		 // Blue, Momentary
	{"Low Oil Press", "427", 41, 1, 3, 1},	 // Red, Momentary
	{"Alternator Fail", "427", 42, 1, 3, 1}, // Red, Momentary
	{"Engine Light", "427", 43, 1, 2, 1},	 // Orange, Momentary
	{"Handbrake On", "437", 18, 1, 3, 1}	 // Red, Momentary
};

// Callback for applying preconfigured warning
static void apply_preconfig_warning_cb(lv_event_t *e) {
	warning_save_data_t *save_data =
		(warning_save_data_t *)lv_event_get_user_data(e);
	if (!save_data || !save_data->input_objects) {
		printf("Error: Invalid save data for preconfig\n");
		return;
	}

	lv_obj_t **inputs = save_data->input_objects;

	// Get the warning dropdown from save_data
	lv_obj_t *warning_dd = save_data->preconfig_warning_dd;
	if (!warning_dd) {
		printf("Error: Warning dropdown not found in save_data\n");
		return;
	}

	uint16_t selected_warning = lv_dropdown_get_selected(warning_dd);
	if (selected_warning >=
		sizeof(ford_babf_warnings) / sizeof(ford_babf_warnings[0])) {
		printf("Error: Invalid warning selection\n");
		return;
	}

	const warning_preconfig_t *preconfig =
		&ford_babf_warnings[selected_warning];

	// Apply the preconfig
	lv_textarea_set_text(inputs[0], preconfig->can_id_hex);
	lv_dropdown_set_selected(inputs[1], preconfig->bit_position);
	// Endianess removed - always defaults to Little Endian (1)
	lv_textarea_set_text(inputs[3], preconfig->label);
	lv_dropdown_set_selected(inputs[4], preconfig->color_index);
	lv_dropdown_set_selected(inputs[5], preconfig->is_momentary);

	// Get color based on color_index
	lv_color_t color;
	switch (preconfig->color_index) {
	case 0:
		color = THEME_COLOR_GREEN;
		break; // Green
	case 1:
		color = THEME_COLOR_BLUE_PURE;
		break; // Blue
	case 2:
		color = THEME_COLOR_ORANGE_WEB;
		break; // Orange
	case 3:
		color = THEME_COLOR_RED;
		break; // Red
	case 4:
		color = THEME_COLOR_YELLOW;
		break; // Yellow
	default:
		color = THEME_COLOR_GREEN;
		break;
	}

	// Update preview color
	if (save_data->preview_objects && save_data->preview_objects[0]) {
		lv_obj_set_style_bg_color(save_data->preview_objects[0], color,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	// Update preview label
	if (save_data->preview_objects && save_data->preview_objects[1]) {
		lv_label_set_text(save_data->preview_objects[1], preconfig->label);
	}

	printf("Applied preconfig: Ford BA/BF/FG - %s\n", preconfig->label);
}

static void save_warning_config_cb(lv_event_t *e) {
	warning_save_data_t *save_data =
		(warning_save_data_t *)lv_event_get_user_data(e);
	if (!save_data) {
		printf("Error: Invalid save data\n");
		return;
	}

	uint8_t warning_idx = save_data->warning_idx;
	lv_obj_t **inputs = save_data->input_objects;

	if (!inputs) {
		printf("Error: Invalid input objects\n");
		lv_mem_free(save_data);
		return;
	}

	// Get values from inputs
	const char *can_id_text = lv_textarea_get_text(inputs[0]);
	uint8_t bit_pos = lv_dropdown_get_selected(inputs[1]);
	uint8_t endianess = 1; // Default to Little Endian (not needed for warnings, but kept for compatibility)
	const char *label_text = lv_textarea_get_text(inputs[3]);

	// Convert CAN ID from hex string to integer
	uint32_t can_id = 0;
	if (can_id_text && *can_id_text) {
		if (strncmp(can_id_text, "0x", 2) == 0) {
			sscanf(can_id_text + 2, "%x", &can_id);
		} else {
			sscanf(can_id_text, "%x", &can_id);
		}
	}

	// Update warning configuration
	warning_configs[warning_idx].can_id = can_id;
	warning_configs[warning_idx].bit_position = bit_pos;
	warning_configs[warning_idx].endianess = endianess;
	if (label_text) {
		strncpy(warning_configs[warning_idx].label, label_text,
				sizeof(warning_configs[warning_idx].label) - 1);
		warning_configs[warning_idx]
			.label[sizeof(warning_configs[warning_idx].label) - 1] = '\0';
	}

	// Handle highlighted color selection
	if (inputs[4]) {
		uint8_t selected_color = lv_dropdown_get_selected(inputs[4]);
		switch (selected_color) {
		case 0:
			warning_configs[warning_idx].active_color = THEME_COLOR_GREEN;
			break; // Green
		case 1:
			warning_configs[warning_idx].active_color = THEME_COLOR_BLUE_PURE;
			break; // Blue
		case 2:
			warning_configs[warning_idx].active_color = THEME_COLOR_ORANGE_WEB;
			break; // Orange
		case 3:
			warning_configs[warning_idx].active_color = THEME_COLOR_RED;
			break; // Red
		case 4:
			warning_configs[warning_idx].active_color = THEME_COLOR_YELLOW;
			break; // Yellow
		default:
			warning_configs[warning_idx].active_color = THEME_COLOR_GREEN;
			break;
		}
	}

	// Save toggle mode setting
	if (inputs[5]) {
		bool was_momentary = warning_configs[warning_idx].is_momentary;
		warning_configs[warning_idx].is_momentary =
			(lv_dropdown_get_selected(inputs[5]) == 1);
		
		// If mode changed, reset state and previous_bit_state
		if (was_momentary != warning_configs[warning_idx].is_momentary) {
			warning_configs[warning_idx].current_state = false;
			previous_bit_states[warning_idx] = false; // Reset previous bit state for toggle mode
			// Update UI to reflect the reset state
			update_warning_ui_immediate(warning_idx);
		}
	}

	// Add callbacks for live preview updates
	lv_obj_add_event_cb(inputs[3], label_text_cb, LV_EVENT_VALUE_CHANGED,
						save_data);
	lv_obj_add_event_cb(inputs[4], color_dropdown_cb, LV_EVENT_VALUE_CHANGED,
						save_data);

	// Debug output
	printf("Warning %d configuration saved:\n", warning_idx + 1);
	printf("  CAN ID: 0x%X\n", can_id);
	printf("  Bit Position: %d\n", bit_pos);
	printf("  Label: %s\n", label_text ? label_text : "");
	printf("  Highlight Color: %06X\n",
		   warning_configs[warning_idx].active_color.full);
	printf("  Mode: %s\n",
		   warning_configs[warning_idx].is_momentary ? "Momentary" : "Toggle");

	// Update the label on Screen3 dynamically
	if (warning_labels[warning_idx]) {
		lv_label_set_text(warning_labels[warning_idx],
						  warning_configs[warning_idx].label);
	}

	// Clean up
	lv_mem_free(inputs);
	lv_mem_free(save_data);
	save_warning_configs_to_nvs();

	// Return to Screen3
	lv_scr_load(ui_Screen3);
}

// Structure to hold all input objects for the save callback
typedef struct {
	uint8_t indicator_idx;
	lv_obj_t *can_id_input;
	lv_obj_t *bit_pos_dropdown;
	lv_obj_t *toggle_mode_dropdown;
} indicator_save_data_t;

// Global variables for preview elements (to allow live updates)
static lv_obj_t *preview_indicator_config = NULL;
static lv_obj_t *preview_status_text_config = NULL;
static uint8_t preview_indicator_idx = 0;

/* Pointers for INPUT dropdown visibility: when Wire selected, hide CAN ID / bit / toggle / animation rows */
typedef struct {
	uint8_t indicator_idx;
	lv_obj_t *input_src_dropdown;
	lv_obj_t *can_id_label;
	lv_obj_t *can_id_0x;
	lv_obj_t *can_id_input;
	lv_obj_t *bit_pos_label;
	lv_obj_t *bit_pos_dropdown;
	lv_obj_t *toggle_mode_label;
	lv_obj_t *toggle_mode_dropdown;
	lv_obj_t *animation_label;
	lv_obj_t *animation_switch;
} indicator_input_visibility_t;

static void indicator_input_src_changed_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	indicator_input_visibility_t *vis = (indicator_input_visibility_t *)lv_event_get_user_data(e);
	if (!vis) return;
	uint16_t sel = lv_dropdown_get_selected(dropdown);
	bool show_can = (sel == 1); /* 0 = Wire, 1 = CAN BUS */
	if (vis->can_id_label) (show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->can_id_label, LV_OBJ_FLAG_HIDDEN);
	if (vis->can_id_0x) (show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->can_id_0x, LV_OBJ_FLAG_HIDDEN);
	if (vis->can_id_input) (show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->can_id_input, LV_OBJ_FLAG_HIDDEN);
	if (vis->bit_pos_label) (show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->bit_pos_label, LV_OBJ_FLAG_HIDDEN);
	if (vis->bit_pos_dropdown) (show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->bit_pos_dropdown, LV_OBJ_FLAG_HIDDEN);
	if (vis->toggle_mode_label) (show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->toggle_mode_label, LV_OBJ_FLAG_HIDDEN);
	if (vis->toggle_mode_dropdown) (show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->toggle_mode_dropdown, LV_OBJ_FLAG_HIDDEN);
	if (vis->animation_label) (show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->animation_label, LV_OBJ_FLAG_HIDDEN);
	if (vis->animation_switch) (show_can ? lv_obj_clear_flag : lv_obj_add_flag)(vis->animation_switch, LV_OBJ_FLAG_HIDDEN);
}

static void indicator_config_screen_delete_cb(lv_event_t *e) {
	indicator_input_visibility_t *vis = (indicator_input_visibility_t *)lv_event_get_user_data(e);
	if (vis) lv_mem_free(vis);
}

static void save_indicator_config_cb(lv_event_t *e) {
	indicator_input_visibility_t *vis = (indicator_input_visibility_t *)lv_event_get_user_data(e);
	if (vis && vis->input_src_dropdown && lv_obj_is_valid(vis->input_src_dropdown) && vis->indicator_idx < 2) {
		uint8_t new_src = (uint8_t)lv_dropdown_get_selected(vis->input_src_dropdown);
		indicator_configs[vis->indicator_idx].input_source = new_src;
		/* When switching to CAN BUS, turn indicator off until CAN message sets it on */
		if (new_src == 1) {
			indicator_configs[vis->indicator_idx].current_state = false;
			previous_indicator_states[vis->indicator_idx] = false;
		}
		save_indicator_configs_to_nvs();
		/* Sync main screen indicator opacity to current state before returning */
		update_indicator_ui_immediate(0);
		update_indicator_ui_immediate(1);
	}
	printf("Indicator configuration completed - returning to main screen\n");

	// Clear preview references
	preview_indicator_config = NULL;
	preview_status_text_config = NULL;
	preview_indicator_idx = 0;

	// Return to Screen3
	lv_scr_load(ui_Screen3);
}

static void back_indicator_config_cb(lv_event_t *e) {
	// Clear preview references
	preview_indicator_config = NULL;
	preview_status_text_config = NULL;
	preview_indicator_idx = 0;

	// Return to Screen3 without saving
	lv_scr_load(ui_Screen3);
}

// Callback for CAN ID input changes
static void indicator_can_id_changed_cb(lv_event_t *e) {
	lv_obj_t *input = lv_event_get_target(e);
	uint8_t *indicator_idx_ptr = (uint8_t *)lv_event_get_user_data(e);

	if (!input || !indicator_idx_ptr || *indicator_idx_ptr >= 2)
		return;

	uint8_t indicator_idx = *indicator_idx_ptr;
	const char *can_id_text = lv_textarea_get_text(input);

	// Convert CAN ID from hex string to integer
	uint32_t can_id = 0;
	if (can_id_text && *can_id_text) {
		if (strncmp(can_id_text, "0x", 2) == 0) {
			sscanf(can_id_text + 2, "%x", &can_id);
		} else {
			sscanf(can_id_text, "%x", &can_id);
		}
	}

	// Update configuration and save to NVS
	indicator_configs[indicator_idx].can_id = can_id;
	printf("Calling save_indicator_configs_to_nvs() for CAN ID change...\n");
	save_indicator_configs_to_nvs();

	printf("Indicator %d CAN ID updated to: 0x%X\n", indicator_idx, can_id);
}

// Callback for bit position dropdown changes
static void indicator_bit_pos_changed_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t *indicator_idx_ptr = (uint8_t *)lv_event_get_user_data(e);

	if (!dropdown || !indicator_idx_ptr || *indicator_idx_ptr >= 2)
		return;

	uint8_t indicator_idx = *indicator_idx_ptr;
	uint8_t bit_pos = lv_dropdown_get_selected(dropdown);

	// Update configuration and save to NVS
	indicator_configs[indicator_idx].bit_position = bit_pos;
	printf(
		"Calling save_indicator_configs_to_nvs() for bit position change...\n");
	save_indicator_configs_to_nvs();

	printf("Indicator %d bit position updated to: %d\n", indicator_idx,
		   bit_pos);
}

// Callback for toggle mode dropdown changes
static void indicator_toggle_mode_changed_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint8_t *indicator_idx_ptr = (uint8_t *)lv_event_get_user_data(e);

	if (!dropdown || !indicator_idx_ptr || *indicator_idx_ptr >= 2)
		return;

	uint8_t indicator_idx = *indicator_idx_ptr;
	bool is_momentary = (lv_dropdown_get_selected(dropdown) == 0);

	// Update configuration and save to NVS
	indicator_configs[indicator_idx].is_momentary = is_momentary;
	printf(
		"Calling save_indicator_configs_to_nvs() for toggle mode change...\n");
	save_indicator_configs_to_nvs();

	printf("Indicator %d toggle mode updated to: %s\n", indicator_idx,
		   is_momentary ? "Momentary" : "Toggle");
}

static void indicator_animation_changed_cb(lv_event_t *e) {
	lv_obj_t *switch_obj = lv_event_get_target(e);
	uint8_t *indicator_idx_ptr = (uint8_t *)lv_event_get_user_data(e);

	if (!switch_obj || !indicator_idx_ptr || *indicator_idx_ptr >= 2)
		return;

	uint8_t indicator_idx = *indicator_idx_ptr;
	bool is_enabled = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);

	// Update configuration and save to NVS
	indicator_configs[indicator_idx].animation_enabled = is_enabled;
	printf("Calling save_indicator_configs_to_nvs() for animation change...\n");
	save_indicator_configs_to_nvs();

	printf("Indicator %d animation updated to: %s\n", indicator_idx,
		   is_enabled ? "Enabled" : "Disabled");
}

void create_indicator_config_menu(uint8_t indicator_idx) {
	printf("Creating indicator config menu for indicator %d\n", indicator_idx);

	// Validate indicator index
	if (indicator_idx >= 2) {
		printf("Error: Invalid indicator index %d\n", indicator_idx);
		return;
	}

	// Initialize common style
	init_common_style();

	// Create the configuration screen
	lv_obj_t *config_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(config_screen, THEME_COLOR_BG, 0);
	lv_obj_set_style_bg_opa(config_screen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(config_screen, LV_OBJ_FLAG_SCROLLABLE);

	// Create main border/background panel
	lv_obj_t *main_border = lv_obj_create(config_screen);
	lv_obj_set_width(main_border, 780);
	lv_obj_set_height(main_border, 325);
	lv_obj_set_align(main_border, LV_ALIGN_CENTER);
	lv_obj_set_y(main_border, 67);
	lv_obj_clear_flag(main_border, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(main_border, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(main_border, THEME_COLOR_INACTIVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(main_border, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(main_border, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Create input border panel
	lv_obj_t *input_border = lv_obj_create(config_screen);
	lv_obj_set_width(input_border, 275);
	lv_obj_set_height(input_border, 310);
	lv_obj_set_x(input_border, -244);
	lv_obj_set_y(input_border, 67);
	lv_obj_set_align(input_border, LV_ALIGN_CENTER);
	lv_obj_clear_flag(input_border, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(input_border, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(input_border, THEME_COLOR_INPUT_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(input_border, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(input_border, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Create preview panel to contain the indicator and status
	lv_obj_t *preview_panel = lv_obj_create(config_screen);
	lv_obj_set_width(preview_panel, 300);
	lv_obj_set_height(preview_panel, 200);
	lv_obj_set_x(preview_panel, 130);
	lv_obj_set_y(preview_panel, 67);
	lv_obj_set_align(preview_panel, LV_ALIGN_CENTER);
	lv_obj_clear_flag(preview_panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(preview_panel, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(preview_panel, THEME_COLOR_SURFACE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(preview_panel, 255,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(preview_panel, 1,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(preview_panel, THEME_COLOR_CONTROL_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Create preview title
	lv_obj_t *preview_title = lv_label_create(preview_panel);
	lv_label_set_text(preview_title, "Live Preview");
	lv_obj_align(preview_title, LV_ALIGN_TOP_MID, 0, 10);
	lv_obj_set_style_text_color(preview_title, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(preview_title, THEME_FONT_BODY, 0);

	// Create preview indicator centered in the panel
	lv_obj_t *preview_indicator = lv_img_create(preview_panel);
	lv_obj_set_width(preview_indicator, LV_SIZE_CONTENT);
	lv_obj_set_height(preview_indicator, LV_SIZE_CONTENT);

	// Position the preview indicator based on which indicator is being
	// configured
	if (indicator_idx == 0) {
		// Left indicator
		lv_img_set_src(preview_indicator, &ui_img_indicator_left_png);
	} else {
		// Right indicator
		lv_img_set_src(preview_indicator, &ui_img_indicator_right_png);
	}

	lv_obj_align(preview_indicator, LV_ALIGN_CENTER, 0, -20);
	lv_obj_clear_flag(preview_indicator, LV_OBJ_FLAG_SCROLLABLE);

	// Set opacity based on current state
	if (indicator_idx < 2 && indicator_configs[indicator_idx].current_state) {
		lv_obj_set_style_opa(preview_indicator, 255,
							 LV_PART_MAIN |
								 LV_STATE_DEFAULT); // 100% opacity when active
	} else {
		lv_obj_set_style_opa(preview_indicator, 50,
							 LV_PART_MAIN |
								 LV_STATE_DEFAULT); // 50% opacity when inactive
	}

	// Create status text below the indicator
	lv_obj_t *status_text = lv_label_create(preview_panel);
	lv_label_set_text_fmt(
		status_text, "%s INDICATOR\n%s", indicator_idx == 0 ? "LEFT" : "RIGHT",
		(indicator_idx < 2 && indicator_configs[indicator_idx].current_state)
			? "ACTIVE"
			: "INACTIVE");
	lv_obj_align(status_text, LV_ALIGN_CENTER, 0, 30);

	// Set text color based on state
	if (indicator_idx < 2 && indicator_configs[indicator_idx].current_state) {
		lv_obj_set_style_text_color(status_text, THEME_COLOR_GREEN,
									0); // Green when active
	} else {
		lv_obj_set_style_text_color(status_text, THEME_COLOR_TEXT_MUTED,
									0); // Gray when inactive
	}

	lv_obj_set_style_text_align(status_text, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(status_text, THEME_FONT_SMALL, 0);

	// Store references for live preview updates
	preview_indicator_config = preview_indicator;
	preview_status_text_config = status_text;
	preview_indicator_idx = indicator_idx;

	// Create the keyboard
	keyboard = lv_keyboard_create(config_screen);
	lv_obj_set_parent(keyboard, lv_layer_top());
	lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(keyboard, keyboard_ready_event_cb, LV_EVENT_READY,
						NULL);

	// Create title
	lv_obj_t *title = lv_label_create(config_screen);
	lv_label_set_text_fmt(title, "%s Indicator Configuration",
						  indicator_idx == 0 ? "Left" : "Right");
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);
	lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(title, THEME_FONT_MEDIUM, 0);

	// Create a container for inputs
	lv_obj_t *inputs_container = lv_obj_create(config_screen);
	lv_obj_set_size(inputs_container, 800, 480);
	lv_obj_align(inputs_container, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_opa(inputs_container, 0, 0);
	lv_obj_set_style_border_opa(inputs_container, 0, 0);
	lv_obj_clear_flag(inputs_container, LV_OBJ_FLAG_SCROLLABLE);

	// Create input section title
	lv_obj_t *input_title = lv_label_create(inputs_container);
	lv_label_set_text(input_title, "Configuration Settings");
	lv_obj_align(input_title, LV_ALIGN_CENTER, -244, -75);
	lv_obj_set_style_text_color(input_title, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(input_title, THEME_FONT_BODY, 0);

	// INPUT: Wire / CANBUS dropdown (top row - functionality to be added)
	lv_obj_t *input_src_label = lv_label_create(inputs_container);
	lv_label_set_text(input_src_label, "INPUT:");
	lv_obj_set_width(input_src_label, 110);
	lv_obj_set_style_text_align(input_src_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(input_src_label, LV_ALIGN_CENTER, -312, -32);
	lv_obj_set_style_text_color(input_src_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(input_src_label, THEME_FONT_SMALL, 0);

	lv_obj_t *input_src_dropdown = lv_dropdown_create(inputs_container);
	lv_obj_add_style(input_src_dropdown, get_common_style(), LV_PART_MAIN);
	lv_dropdown_set_options(input_src_dropdown, "Wire\nCAN BUS");
	lv_obj_set_width(input_src_dropdown, 120);
	lv_obj_align(input_src_dropdown, LV_ALIGN_CENTER, -180, -32);
	lv_dropdown_set_selected(input_src_dropdown,
		indicator_idx < 2 ? indicator_configs[indicator_idx].input_source : 0);
	if (lv_dropdown_get_selected(input_src_dropdown) > 1)
		lv_dropdown_set_selected(input_src_dropdown, 0);

	// CAN ID input
	lv_obj_t *can_id_label = lv_label_create(inputs_container);
	lv_label_set_text(can_id_label, "CAN ID:");
	lv_obj_set_width(can_id_label, 110);
	lv_obj_set_style_text_align(can_id_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(can_id_label, LV_ALIGN_CENTER, -312, 8);
	lv_obj_set_style_text_color(can_id_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(can_id_label, THEME_FONT_SMALL, 0);

	// CAN ID "0x" prefix
	lv_obj_t *can_id_0x = lv_label_create(inputs_container);
	lv_label_set_text(can_id_0x, "0x");
	lv_obj_set_width(can_id_0x, 20);
	lv_obj_set_style_text_color(can_id_0x, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_align(can_id_0x, LV_ALIGN_CENTER, -200, 8);
	lv_obj_set_style_text_align(can_id_0x, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_style_text_font(can_id_0x, THEME_FONT_SMALL, 0);

	lv_obj_t *can_id_input = lv_textarea_create(inputs_container);
	lv_obj_add_style(can_id_input, get_common_style(), LV_PART_MAIN);
	lv_textarea_set_one_line(can_id_input, true);
	lv_obj_set_width(can_id_input, 120);
	lv_obj_align(can_id_input, LV_ALIGN_CENTER, -180, 8);
	lv_obj_add_event_cb(can_id_input, keyboard_event_cb, LV_EVENT_ALL, NULL);

	// Add real-time saving callback for CAN ID - use both VALUE_CHANGED and
	// DEFOCUSED events
	uint8_t *can_id_idx_ptr = lv_mem_alloc(sizeof(uint8_t));
	if (can_id_idx_ptr) {
		*can_id_idx_ptr = indicator_idx;
		lv_obj_add_event_cb(can_id_input, indicator_can_id_changed_cb,
							LV_EVENT_VALUE_CHANGED, can_id_idx_ptr);
		lv_obj_add_event_cb(can_id_input, indicator_can_id_changed_cb,
							LV_EVENT_DEFOCUSED, can_id_idx_ptr);
		printf("Added CAN ID callbacks for indicator %d\n", indicator_idx);
	}

	// Set placeholder and initial value
	char can_id_text[32];
	if (indicator_idx < 2) {
		snprintf(can_id_text, sizeof(can_id_text), "%X",
				 indicator_configs[indicator_idx].can_id);
	} else {
		snprintf(can_id_text, sizeof(can_id_text), "0");
	}
	lv_textarea_set_text(can_id_input, can_id_text);
	lv_textarea_set_placeholder_text(can_id_input, "Enter CAN ID");

	// Bit position dropdown
	lv_obj_t *bit_pos_label = lv_label_create(inputs_container);
	lv_label_set_text(bit_pos_label, "Bit Position:");
	lv_obj_set_width(bit_pos_label, 110);
	lv_obj_set_style_text_align(bit_pos_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(bit_pos_label, LV_ALIGN_CENTER, -312, 48);
	lv_obj_set_style_text_color(bit_pos_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(bit_pos_label, THEME_FONT_SMALL, 0);

	lv_obj_t *bit_pos_dropdown = lv_dropdown_create(inputs_container);
	lv_obj_add_style(bit_pos_dropdown, get_common_style(), LV_PART_MAIN);
	lv_dropdown_set_options(
		bit_pos_dropdown,
		"0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n"
		"16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n"
		"32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n"
		"48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60\n61\n62\n63");
	lv_obj_set_width(bit_pos_dropdown, 120);
	lv_obj_align(bit_pos_dropdown, LV_ALIGN_CENTER, -180, 48);

	// Add real-time saving callback for bit position
	uint8_t *bit_pos_idx_ptr = lv_mem_alloc(sizeof(uint8_t));
	if (bit_pos_idx_ptr) {
		*bit_pos_idx_ptr = indicator_idx;
		lv_obj_add_event_cb(bit_pos_dropdown, indicator_bit_pos_changed_cb,
							LV_EVENT_VALUE_CHANGED, bit_pos_idx_ptr);
		printf("Added bit position callback for indicator %d\n", indicator_idx);
	}

	if (indicator_idx < 2) {
		lv_dropdown_set_selected(bit_pos_dropdown,
								 indicator_configs[indicator_idx].bit_position);
	} else {
		lv_dropdown_set_selected(bit_pos_dropdown, 0);
	}

	// Toggle mode dropdown
	lv_obj_t *toggle_mode_label = lv_label_create(inputs_container);
	lv_label_set_text(toggle_mode_label, "Toggle Mode:");
	lv_obj_set_width(toggle_mode_label, 110);
	lv_obj_set_style_text_align(toggle_mode_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(toggle_mode_label, LV_ALIGN_CENTER, -312, 88);
	lv_obj_set_style_text_color(toggle_mode_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(toggle_mode_label, THEME_FONT_SMALL, 0);

	lv_obj_t *toggle_mode_dropdown = lv_dropdown_create(inputs_container);
	lv_obj_add_style(toggle_mode_dropdown, get_common_style(), LV_PART_MAIN);
	lv_dropdown_set_options(toggle_mode_dropdown, "Momentary\nToggle");
	lv_obj_set_width(toggle_mode_dropdown, 120);
	lv_obj_align(toggle_mode_dropdown, LV_ALIGN_CENTER, -180, 88);

	// Add real-time saving callback for toggle mode
	uint8_t *toggle_mode_idx_ptr = lv_mem_alloc(sizeof(uint8_t));
	if (toggle_mode_idx_ptr) {
		*toggle_mode_idx_ptr = indicator_idx;
		lv_obj_add_event_cb(toggle_mode_dropdown,
							indicator_toggle_mode_changed_cb,
							LV_EVENT_VALUE_CHANGED, toggle_mode_idx_ptr);
		printf("Added toggle mode callback for indicator %d\n", indicator_idx);
	}

	if (indicator_idx < 2) {
		lv_dropdown_set_selected(
			toggle_mode_dropdown,
			indicator_configs[indicator_idx].is_momentary ? 0 : 1);
	} else {
		lv_dropdown_set_selected(toggle_mode_dropdown, 0);
	}

	// Animation setting
	lv_obj_t *animation_label = lv_label_create(inputs_container);
	lv_label_set_text(animation_label, "Animation:");
	lv_obj_set_width(animation_label, 110);
	lv_obj_set_style_text_align(animation_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(animation_label, LV_ALIGN_CENTER, -312, 128);
	lv_obj_set_style_text_color(animation_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(animation_label, THEME_FONT_SMALL, 0);

	lv_obj_t *animation_switch = lv_switch_create(inputs_container);
	lv_obj_add_style(animation_switch, get_common_style(), LV_PART_MAIN);
	lv_obj_set_width(animation_switch, 50);
	lv_obj_align(animation_switch, LV_ALIGN_CENTER, -155, 128);

	// Add real-time saving callback for animation
	uint8_t *animation_idx_ptr = lv_mem_alloc(sizeof(uint8_t));
	if (animation_idx_ptr) {
		*animation_idx_ptr = indicator_idx;
		lv_obj_add_event_cb(animation_switch, indicator_animation_changed_cb,
							LV_EVENT_VALUE_CHANGED, animation_idx_ptr);
		printf("Added animation callback for indicator %d\n", indicator_idx);
	}

	if (indicator_idx < 2) {
		if (indicator_configs[indicator_idx].animation_enabled) {
			lv_obj_add_state(animation_switch, LV_STATE_CHECKED);
		} else {
			lv_obj_clear_state(animation_switch, LV_STATE_CHECKED);
		}
	} else {
		lv_obj_add_state(animation_switch,
						 LV_STATE_CHECKED); // Default to enabled
	}

	/* Wire vs CAN BUS: when Wire selected, hide CAN ID / bit / toggle / animation */
	indicator_input_visibility_t *vis = lv_mem_alloc(sizeof(indicator_input_visibility_t));
	if (vis) {
		vis->indicator_idx = indicator_idx;
		vis->input_src_dropdown = input_src_dropdown;
		vis->can_id_label = can_id_label;
		vis->can_id_0x = can_id_0x;
		vis->can_id_input = can_id_input;
		vis->bit_pos_label = bit_pos_label;
		vis->bit_pos_dropdown = bit_pos_dropdown;
		vis->toggle_mode_label = toggle_mode_label;
		vis->toggle_mode_dropdown = toggle_mode_dropdown;
		vis->animation_label = animation_label;
		vis->animation_switch = animation_switch;
		lv_obj_add_event_cb(input_src_dropdown, indicator_input_src_changed_cb,
				    LV_EVENT_VALUE_CHANGED, vis);
		lv_obj_add_event_cb(config_screen, indicator_config_screen_delete_cb,
				    LV_EVENT_DELETE, vis);
		/* Initial state: Wire (0) = hide CAN rows */
		if (lv_dropdown_get_selected(input_src_dropdown) == 0) {
			lv_obj_add_flag(can_id_label, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(can_id_0x, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(can_id_input, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(bit_pos_label, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(bit_pos_dropdown, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(toggle_mode_label, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(toggle_mode_dropdown, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(animation_label, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(animation_switch, LV_OBJ_FLAG_HIDDEN);
		}
	}

	// Save button (green) - same position and style as panel config menu
	lv_obj_t *save_btn = lv_btn_create(config_screen);
	lv_obj_t *save_label = lv_label_create(save_btn);
	lv_label_set_text(save_label, "Save");
	lv_obj_set_style_bg_color(save_btn, THEME_COLOR_BTN_SAVE, LV_PART_MAIN);
	lv_obj_set_align(save_btn, LV_ALIGN_CENTER);
	lv_obj_set_pos(save_btn, 200, 200);
	lv_obj_center(save_label);

	// Cancel button (red) - same position and style as panel config menu
	lv_obj_t *back_btn = lv_btn_create(config_screen);
	lv_obj_t *back_label = lv_label_create(back_btn);
	lv_label_set_text(back_label, "Cancel");
	lv_obj_set_style_bg_color(back_btn, THEME_COLOR_BTN_CANCEL, LV_PART_MAIN);
	lv_obj_set_align(back_btn, LV_ALIGN_CENTER);
	lv_obj_set_pos(back_btn, 300, 200);
	lv_obj_center(back_label);

	// Save button event handler (pass vis so input_source can be read and saved to NVS)
	lv_obj_add_event_cb(save_btn, save_indicator_config_cb, LV_EVENT_CLICKED, vis);

	// Back button event handler
	lv_obj_add_event_cb(back_btn, back_indicator_config_cb, LV_EVENT_CLICKED,
						NULL);

	// No need to store save data since settings save automatically

	// Yield to prevent blocking other tasks
	vTaskDelay(pdMS_TO_TICKS(1));

	// Load the config screen
	lv_scr_load(config_screen);
}

static void check_warning_timeouts(lv_timer_t *timer) {
	// Timeout function is no longer needed for momentary warnings
	// as they now follow the live bit state directly.
	// This function is kept for potential future use but does nothing.
	(void)timer; // Suppress unused parameter warning
}

// CAN timeout checking function
static void check_can_timeouts(lv_timer_t *timer) {
	(void)timer; // Suppress unused parameter warning
	uint64_t current_time = esp_timer_get_time() / 1000; // ms

	// Determine the most recent CAN activity across all values
	uint64_t last_any = last_rpm_can_received;
	for (int k = 0; k < 8; k++)
		if (last_panel_can_received[k] > last_any) last_any = last_panel_can_received[k];
	if (last_speed_can_received > last_any) last_any = last_speed_can_received;
	if (last_gear_can_received  > last_any) last_any = last_gear_can_received;
	for (int k = 0; k < 2; k++)
		if (last_bar_can_received[k] > last_any) last_any = last_bar_can_received[k];

	// If CAN bus is alive (any message within 4 s) give each value 10 s before
	// showing "---".  If the bus has been silent for >4 s, drop everything
	// to "---" quickly using the 4 s dead-bus threshold.
	bool can_active = (current_time - last_any) < 4000;
	const uint64_t CAN_TIMEOUT_MS = can_active ? 10000 : 4000;

	// Check panel timeouts (excluding RPM which is panel 9)
	for (int i = 0; i < 8; i++) {
		if (values_config[i].enabled &&
			(current_time - last_panel_can_received[i]) > CAN_TIMEOUT_MS) {

			// Set panel to "---" if it's not already
			if (strcmp(previous_values[i], "---") != 0) {
				strcpy(previous_values[i], "---");
				panel_update_t *p_upd = malloc(sizeof(panel_update_t));
				if (p_upd) {
					p_upd->panel_index = i;
					strcpy(p_upd->value_str, "---");
					p_upd->final_value =
						0; // Use 0 to ensure no threshold warnings
					lv_async_call(update_panel_ui, p_upd);
				}
			}
		}
	}

	// Check speed timeout
	if (values_config[SPEED_VALUE_ID - 1].enabled &&
		(current_time - last_speed_can_received) > CAN_TIMEOUT_MS) {

		// Set speed to "---" if it's not already
		if (strcmp(previous_values[SPEED_VALUE_ID - 1], "---") != 0) {
			strcpy(previous_values[SPEED_VALUE_ID - 1], "---");
			speed_update_t *s_upd = malloc(sizeof(speed_update_t));
			if (s_upd) {
				strcpy(s_upd->speed_str, "---");
				lv_async_call(update_speed_ui, s_upd);
			}
		}
	}

	// Check gear timeout (skip if in Speed/RPM Ratio mode - mode 4)
	if (values_config[GEAR_VALUE_ID - 1].enabled &&
		values_config[GEAR_VALUE_ID - 1].gear_detection_mode != 4 &&
		(current_time - last_gear_can_received) > CAN_TIMEOUT_MS) {

		// Set gear to "---" if it's not already
		if (strcmp(previous_values[GEAR_VALUE_ID - 1], "---") != 0) {
			strcpy(previous_values[GEAR_VALUE_ID - 1], "---");
			gear_update_t *g_upd = malloc(sizeof(gear_update_t));
			if (g_upd) {
				strcpy(g_upd->gear_str, "---");
				g_upd->raw_value = 0;
				lv_async_call(update_gear_ui, g_upd);
			}
		}
	}

	// Check bar timeouts (skip bars driven by the fuel sender ADC)
	for (int i = 0; i < 2; i++) {
		int value_index = (i == 0) ? BAR1_VALUE_ID - 1 : BAR2_VALUE_ID - 1;
		if (values_config[value_index].fuel_sender) continue;
		if (values_config[value_index].enabled &&
			(current_time - last_bar_can_received[i]) > CAN_TIMEOUT_MS) {

			// Set bar to minimum value (representing "no data")
			bar_update_t *b_upd = malloc(sizeof(bar_update_t));
			if (b_upd) {
				b_upd->bar_index = i;
				b_upd->bar_value =
					values_config[value_index].bar_min; // Use minimum value
				b_upd->final_value = values_config[value_index].bar_min;
				b_upd->config_index = value_index;
				b_upd->is_timeout = true;
				lv_async_call(update_bar_ui, b_upd);
			}
		}
	}

	// Check RPM timeout - instantly go to 0
	if (values_config[RPM_VALUE_ID - 1].enabled &&
		(current_time - last_rpm_can_received) > CAN_TIMEOUT_MS) {

		// Set RPM to "---" if it's not already
		if (strcmp(previous_values[RPM_VALUE_ID - 1], "---") != 0) {
			strcpy(previous_values[RPM_VALUE_ID - 1], "---");
			rpm_update_t *r_upd = malloc(sizeof(rpm_update_t));
			if (r_upd) {
				strcpy(r_upd->rpm_str, "---");
				r_upd->rpm_value = 0; // Use 0 for gauge
				lv_async_call(update_rpm_ui, r_upd);
			}
		}
	}
}

/////////////////////////////////////////////	PROCESSING
////////////////////////////////////////////////

void set_rpm_value(int rpm) {
	if (rpm < 0)
		rpm = 0;

	// Store the current CAN bus RPM value when not in demo mode
	if (!limiter_demo_active) {
		current_canbus_rpm = rpm;
	}

	if (rpm_bar_gauge && lv_obj_is_valid(rpm_bar_gauge)) {
		// Map RPM to extended bar range to properly fill the extended bar width
		// When RPM reaches rpm_gauge_max, the bar should be completely filled
		const float bar_extension_ratio = 782.5f / 765.0f;
		int32_t extended_rpm_max =
			(int32_t)(rpm_gauge_max * bar_extension_ratio);

		// Scale the RPM value to the extended range
		int32_t scaled_rpm = (rpm * extended_rpm_max) / rpm_gauge_max;

		lv_bar_set_value(rpm_bar_gauge, scaled_rpm, LV_ANIM_OFF);
	}

	// Update RPM lights based on current RPM value (only if not in demo mode)
	if (!limiter_demo_active) {
		update_rpm_lights(rpm);

		// Check if we should activate limiter effects based on real RPM
		// Only process if RPM actually changed to avoid redundant checks
		if (rpm != last_rpm_for_limiter_check) {
			last_rpm_for_limiter_check = rpm;

			uint8_t limiter_effect =
				values_config[RPM_VALUE_ID - 1].rpm_limiter_effect;
			int32_t limiter_threshold =
				values_config[RPM_VALUE_ID - 1].rpm_limiter_value;

			// Add hysteresis: activate at threshold, deactivate at threshold -
			// 200 RPM
			const int32_t HYSTERESIS = 200;

			if (limiter_effect > 0) {
				if (!limiter_active && rpm >= limiter_threshold) {
					// RPM exceeded limiter threshold, start effect
					limiter_active = true;
					start_real_limiter_effect(limiter_effect);
				} else if (limiter_active &&
						   rpm < (limiter_threshold - HYSTERESIS)) {
					// RPM dropped below threshold minus hysteresis, stop effect
					limiter_active = false;
					stop_real_limiter_effect();
				}
				// If RPM is between (threshold - HYSTERESIS) and threshold,
				// maintain current state
			}
		}

		// Check if we should activate RPM background effect based on real RPM
		if (values_config[RPM_VALUE_ID - 1].rpm_background_enabled) {
			int32_t background_threshold =
				values_config[RPM_VALUE_ID - 1].rpm_background_value;

			// Add hysteresis for background effect too: activate at threshold,
			// deactivate at threshold - 200 RPM
			const int32_t BACKGROUND_HYSTERESIS = 200;

			if (!rpm_background_active && rpm >= background_threshold) {
				// RPM exceeded background threshold, change background color
				rpm_background_active = true;

				// Save original background color if not already saved
				if (!original_screen_bg_color_saved && ui_Screen3 &&
					lv_obj_is_valid(ui_Screen3)) {
					// Get current background color (assuming it's the default)
					original_screen_bg_color =
						THEME_COLOR_BG; // Default black background
					original_screen_bg_color_saved = true;
				}

				// Set background to the configured RPM background color
				if (ui_Screen3 && lv_obj_is_valid(ui_Screen3)) {
					lv_obj_set_style_bg_color(
						ui_Screen3,
						values_config[RPM_VALUE_ID - 1].rpm_background_color,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}

				// Change panel backgrounds to grey when RPM background is
				// active
				for (int i = 0; i < 8; i++) {
					if (ui_Box[i] && lv_obj_is_valid(ui_Box[i])) {
						lv_obj_set_style_bg_color(
							ui_Box[i], THEME_COLOR_PANEL,
							LV_PART_MAIN | LV_STATE_DEFAULT);
					}
				}

				// Change text elements to grey when RPM background is active
				if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value)) {
					lv_obj_set_style_text_color(
						ui_RPM_Value, THEME_COLOR_PANEL,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Speed_Value && lv_obj_is_valid(ui_Speed_Value)) {
					lv_obj_set_style_text_color(
						ui_Speed_Value, THEME_COLOR_PANEL,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Kmh && lv_obj_is_valid(ui_Kmh)) {
					lv_obj_set_style_text_color(ui_Kmh, THEME_COLOR_PANEL,
												LV_PART_MAIN |
													LV_STATE_DEFAULT);
				}
				if (ui_Bar_1_Label && lv_obj_is_valid(ui_Bar_1_Label)) {
					lv_obj_set_style_text_color(
						ui_Bar_1_Label, THEME_COLOR_PANEL,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_1_Value && lv_obj_is_valid(ui_Bar_1_Value)) {
					lv_obj_set_style_text_color(
						ui_Bar_1_Value, THEME_COLOR_PANEL,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_2_Label && lv_obj_is_valid(ui_Bar_2_Label)) {
					lv_obj_set_style_text_color(
						ui_Bar_2_Label, THEME_COLOR_PANEL,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_2_Value && lv_obj_is_valid(ui_Bar_2_Value)) {
					lv_obj_set_style_text_color(
						ui_Bar_2_Value, THEME_COLOR_PANEL,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
			} else if (rpm_background_active &&
					   rpm < (background_threshold - BACKGROUND_HYSTERESIS)) {
				// RPM dropped below threshold minus hysteresis, restore
				// original background
				rpm_background_active = false;

				// Restore original background color
				if (ui_Screen3 && lv_obj_is_valid(ui_Screen3)) {
					lv_obj_set_style_bg_color(ui_Screen3,
											  original_screen_bg_color,
											  LV_PART_MAIN | LV_STATE_DEFAULT);
				}

				// Restore panel backgrounds to black when RPM background is
				// inactive
				for (int i = 0; i < 8; i++) {
					if (ui_Box[i] && lv_obj_is_valid(ui_Box[i])) {
						lv_obj_set_style_bg_color(
							ui_Box[i], THEME_COLOR_BG,
							LV_PART_MAIN | LV_STATE_DEFAULT);
					}
				}

				// Restore text elements to white when RPM background is
				// inactive
				if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value)) {
					lv_obj_set_style_text_color(
						ui_RPM_Value, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Speed_Value && lv_obj_is_valid(ui_Speed_Value)) {
					lv_obj_set_style_text_color(
						ui_Speed_Value, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Kmh && lv_obj_is_valid(ui_Kmh)) {
					lv_obj_set_style_text_color(ui_Kmh, THEME_COLOR_TEXT_PRIMARY,
												LV_PART_MAIN |
													LV_STATE_DEFAULT);
				}
				if (ui_Bar_1_Label && lv_obj_is_valid(ui_Bar_1_Label)) {
					lv_obj_set_style_text_color(
						ui_Bar_1_Label, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_1_Value && lv_obj_is_valid(ui_Bar_1_Value)) {
					lv_obj_set_style_text_color(
						ui_Bar_1_Value, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_2_Label && lv_obj_is_valid(ui_Bar_2_Label)) {
					lv_obj_set_style_text_color(
						ui_Bar_2_Label, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
				if (ui_Bar_2_Value && lv_obj_is_valid(ui_Bar_2_Value)) {
					lv_obj_set_style_text_color(
						ui_Bar_2_Value, THEME_COLOR_TEXT_PRIMARY,
						LV_PART_MAIN | LV_STATE_DEFAULT);
				}
			}
			// If RPM is between (threshold - HYSTERESIS) and threshold,
			// maintain current state
		} else if (rpm_background_active) {
			// Background feature is disabled but background is still active -
			// restore it
			rpm_background_active = false;
			if (ui_Screen3 && lv_obj_is_valid(ui_Screen3)) {
				lv_obj_set_style_bg_color(ui_Screen3, original_screen_bg_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
			}

			// Restore panel backgrounds to black when RPM background is
			// disabled
			for (int i = 0; i < 8; i++) {
				if (ui_Box[i] && lv_obj_is_valid(ui_Box[i])) {
					lv_obj_set_style_bg_color(ui_Box[i], THEME_COLOR_BG,
											  LV_PART_MAIN | LV_STATE_DEFAULT);
				}
			}

			// Restore text elements to white when RPM background is disabled
			if (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value)) {
				lv_obj_set_style_text_color(ui_RPM_Value,
											THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Speed_Value && lv_obj_is_valid(ui_Speed_Value)) {
				lv_obj_set_style_text_color(ui_Speed_Value,
											THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Kmh && lv_obj_is_valid(ui_Kmh)) {
				lv_obj_set_style_text_color(ui_Kmh, THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Bar_1_Label && lv_obj_is_valid(ui_Bar_1_Label)) {
				lv_obj_set_style_text_color(ui_Bar_1_Label,
											THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Bar_1_Value && lv_obj_is_valid(ui_Bar_1_Value)) {
				lv_obj_set_style_text_color(ui_Bar_1_Value,
											THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Bar_2_Label && lv_obj_is_valid(ui_Bar_2_Label)) {
				lv_obj_set_style_text_color(ui_Bar_2_Label,
											THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			if (ui_Bar_2_Value && lv_obj_is_valid(ui_Bar_2_Value)) {
				lv_obj_set_style_text_color(ui_Bar_2_Value,
											THEME_COLOR_TEXT_PRIMARY,
											LV_PART_MAIN | LV_STATE_DEFAULT);
			}
		}
	}
}

void update_redline_position(void) {
	if (!rpm_redline_zone)
		return;

	// Calculate redline position as percentage of max RPM
	float redline_percentage = (float)rpm_redline_value / (float)rpm_gauge_max;

	// Clamp to prevent going beyond the bar
	if (redline_percentage > 1.0f)
		redline_percentage = 1.0f;
	if (redline_percentage < 0.0f)
		redline_percentage = 0.0f;

	// Screen and RPM bar dimensions
	const lv_coord_t screen_width = 800; // Full screen width
	const lv_coord_t bar_width = 765;
	const lv_coord_t screen_right_edge =
		screen_width / 2; // Right edge relative to center

	// Calculate redline zone dimensions - extends from right edge of screen to
	// redline position
	lv_coord_t redline_rpm_position =
		-(bar_width / 2) +
		(redline_percentage * bar_width); // RPM position on bar
	lv_coord_t redline_width =
		screen_right_edge -
		redline_rpm_position; // From redline to right edge of screen

	// If redline is at or beyond max RPM, hide the zone
	if (redline_percentage >= 1.0f || redline_width <= 0) {
		lv_obj_add_flag(rpm_redline_zone, LV_OBJ_FLAG_HIDDEN);
		printf("Redline zone hidden (redline at or beyond max RPM)\n");
		return;
	}

	// Show and position the redline zone
	lv_obj_clear_flag(rpm_redline_zone, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_width(rpm_redline_zone, redline_width);
	// Position so it starts at redline RPM position and extends to right edge
	lv_obj_set_x(rpm_redline_zone, redline_rpm_position + (redline_width / 2));

	printf("Redline updated: %d RPM at %.1f%% (zone: from RPM pos %d to screen "
		   "edge, width=%d)\n",
		   rpm_redline_value, redline_percentage * 100, redline_rpm_position,
		   redline_width);
}

// Asynchronous callback for updating a panel's UI (e.g., the label text)
void update_panel_ui(void *param) {
	panel_update_t *update = (panel_update_t *)param;
	if (!update)
		return;

	uint8_t i = update->panel_index;
	if (ui_Value[i] && lv_obj_is_valid(ui_Value[i]) &&
		lv_obj_get_screen(ui_Value[i]) != NULL) {
		lv_label_set_text(ui_Value[i], update->value_str);
	}

	// Also update menu preview if it exists, is valid, and menu is visible
	if (menu_panel_value_labels[i] &&
		lv_obj_is_valid(menu_panel_value_labels[i]) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_label_set_text(menu_panel_value_labels[i], update->value_str);
	}

	// Also update menu panel box border effects if menu is visible
	if (menu_panel_boxes[i] && lv_obj_is_valid(menu_panel_boxes[i]) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		// Apply same border logic as main screen panels
		if (strcmp(update->value_str, "---") == 0) {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (values_config[i].warning_high_enabled &&
				   update->final_value >
					   values_config[i].warning_high_threshold) {
			// High warning threshold exceeded
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  values_config[i].warning_high_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (values_config[i].warning_low_enabled &&
				   update->final_value <
					   values_config[i].warning_low_threshold) {
			// Low warning threshold exceeded
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  values_config[i].warning_low_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else {
			// No thresholds exceeded, use default color
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		// Ensure border is visible
		lv_obj_set_style_border_width(menu_panel_boxes[i], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(menu_panel_boxes[i], 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	// Update border color based on thresholds
	if (ui_Box[i] && lv_obj_is_valid(ui_Box[i]) &&
		lv_obj_get_screen(ui_Box[i]) != NULL) {
		// Special case: if showing "---", always use default grey color
		if (strcmp(update->value_str, "---") == 0) {
			lv_obj_set_style_border_color(ui_Box[i], THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (values_config[i].warning_high_enabled &&
				   update->final_value >
					   values_config[i].warning_high_threshold) {
			// High warning threshold exceeded
			lv_obj_set_style_border_color(ui_Box[i],
										  values_config[i].warning_high_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (values_config[i].warning_low_enabled &&
				   update->final_value <
					   values_config[i].warning_low_threshold) {
			// Low warning threshold exceeded
			lv_obj_set_style_border_color(ui_Box[i],
										  values_config[i].warning_low_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else {
			// No thresholds exceeded, use default color
			lv_obj_set_style_border_color(ui_Box[i], THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		// Ensure border is visible
		lv_obj_set_style_border_width(ui_Box[i], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(ui_Box[i], 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	free(update);
}

// Immediate (no-alloc, no-async) panel update
static void update_panel_ui_immediate(uint8_t i, const char *value_str,
									  double final_value) {
	if (i >= 8)
		return;
	if (ui_Value[i] && lv_obj_is_valid(ui_Value[i]) &&
		lv_obj_get_screen(ui_Value[i]) != NULL) {
		lv_label_set_text(ui_Value[i], value_str);
	}
	if (menu_panel_value_labels[i] &&
		lv_obj_is_valid(menu_panel_value_labels[i]) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_label_set_text(menu_panel_value_labels[i], value_str);
	}
	if (menu_panel_boxes[i] && lv_obj_is_valid(menu_panel_boxes[i]) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		if (strcmp(value_str, "---") == 0) {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (values_config[i].warning_high_enabled &&
				   final_value > values_config[i].warning_high_threshold) {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  values_config[i].warning_high_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (values_config[i].warning_low_enabled &&
				   final_value < values_config[i].warning_low_threshold) {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  values_config[i].warning_low_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else {
			lv_obj_set_style_border_color(menu_panel_boxes[i],
										  THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		lv_obj_set_style_border_width(menu_panel_boxes[i], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(menu_panel_boxes[i], 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	if (ui_Box[i] && lv_obj_is_valid(ui_Box[i]) &&
		lv_obj_get_screen(ui_Box[i]) != NULL) {
		if (strcmp(value_str, "---") == 0) {
			lv_obj_set_style_border_color(ui_Box[i], THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (values_config[i].warning_high_enabled &&
				   final_value > values_config[i].warning_high_threshold) {
			lv_obj_set_style_border_color(ui_Box[i],
										  values_config[i].warning_high_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else if (values_config[i].warning_low_enabled &&
				   final_value < values_config[i].warning_low_threshold) {
			lv_obj_set_style_border_color(ui_Box[i],
										  values_config[i].warning_low_color,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		} else {
			lv_obj_set_style_border_color(ui_Box[i], THEME_COLOR_PANEL,
										  LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		lv_obj_set_style_border_width(ui_Box[i], 3,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_opa(ui_Box[i], 255,
									LV_PART_MAIN | LV_STATE_DEFAULT);
	}
}

// Asynchronous callback for updating a bar's display and color
void update_bar_ui(void *param) {
	bar_update_t *upd = (bar_update_t *)param;
	// Select the appropriate bar object (assuming bar_index 0 means ui_Bar_1
	// and 1 means ui_Bar_2)
	lv_obj_t *bar_obj = (upd->bar_index == 0) ? ui_Bar_1 : ui_Bar_2;

	// Check if the bar object is still valid.
	if (bar_obj == NULL || !lv_obj_is_valid(bar_obj) ||
		lv_obj_get_screen(bar_obj) == NULL) {
		free(upd);
		return;
	}

	lv_bar_set_value(bar_obj, upd->bar_value, LV_ANIM_OFF);

	// Use configured colors instead of hardcoded values
	lv_color_t new_color;
	if (upd->final_value < values_config[upd->config_index].bar_low) {
		new_color = values_config[upd->config_index].bar_low_color;
	} else if (upd->final_value > values_config[upd->config_index].bar_high) {
		new_color = values_config[upd->config_index].bar_high_color;
	} else {
		new_color = values_config[upd->config_index].bar_in_range_color;
	}

	lv_obj_set_style_bg_color(bar_obj, new_color,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);

	// Also update menu preview bar if it exists, is valid, and menu is visible
	lv_obj_t *menu_bar = menu_bar_objects[upd->bar_index];
	if (menu_bar && lv_obj_is_valid(menu_bar) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_bar_set_value(menu_bar, upd->bar_value, LV_ANIM_OFF);
		lv_obj_set_style_bg_color(menu_bar, new_color,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	}

	// Update Bar numeric value displays (only if show_bar_value is enabled)
	if (upd->bar_index == 0 && ui_Bar_1_Value &&
		lv_obj_is_valid(ui_Bar_1_Value) &&
		values_config[upd->config_index].show_bar_value) {
		char value_str[16];
		if (upd->is_timeout) {
			strcpy(value_str, "---");
		} else {
			// Format the value based on decimals configuration
			int decimals = values_config[upd->config_index].decimals;
			if (decimals == 0) {
				snprintf(value_str, sizeof(value_str), "%d", (int)upd->final_value);
			} else {
				snprintf(value_str, sizeof(value_str), "%.*f", decimals,
						 upd->final_value);
			}
		}
		lv_label_set_text(ui_Bar_1_Value, value_str);
	} else if (upd->bar_index == 1 && ui_Bar_2_Value &&
			   lv_obj_is_valid(ui_Bar_2_Value) &&
			   values_config[upd->config_index].show_bar_value) {
		char value_str[16];
		if (upd->is_timeout) {
			strcpy(value_str, "---");
		} else {
			// Format the value based on decimals configuration
			int decimals = values_config[upd->config_index].decimals;
			if (decimals == 0) {
				snprintf(value_str, sizeof(value_str), "%d", (int)upd->final_value);
			} else {
				snprintf(value_str, sizeof(value_str), "%.*f", decimals,
						 upd->final_value);
			}
		}
		lv_label_set_text(ui_Bar_2_Value, value_str);
	}

	free(upd);
}

// Immediate (no-alloc, no-async) bar update
static void update_bar_ui_immediate(int bar_index, int32_t bar_value,
									double final_value, int config_index) {
	lv_obj_t *bar_obj = (bar_index == 0) ? ui_Bar_1 : ui_Bar_2;
	if (bar_obj == NULL || !lv_obj_is_valid(bar_obj) ||
		lv_obj_get_screen(bar_obj) == NULL) {
		return;
	}
	lv_bar_set_value(bar_obj, bar_value, LV_ANIM_OFF);
	lv_color_t new_color;
	if (final_value < values_config[config_index].bar_low) {
		new_color = values_config[config_index].bar_low_color;
	} else if (final_value > values_config[config_index].bar_high) {
		new_color = values_config[config_index].bar_high_color;
	} else {
		new_color = values_config[config_index].bar_in_range_color;
	}
	lv_obj_set_style_bg_color(bar_obj, new_color,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);

	// Update Bar numeric value displays (only if show_bar_value is enabled)
	if (bar_index == 0 && ui_Bar_1_Value && lv_obj_is_valid(ui_Bar_1_Value) &&
		values_config[config_index].show_bar_value) {
		char value_str[16];
		// Format the value based on decimals configuration
		int decimals = values_config[config_index].decimals;
		if (decimals == 0) {
			snprintf(value_str, sizeof(value_str), "%d", (int)final_value);
		} else {
			snprintf(value_str, sizeof(value_str), "%.*f", decimals,
					 final_value);
		}
		lv_label_set_text(ui_Bar_1_Value, value_str);
	} else if (bar_index == 1 && ui_Bar_2_Value &&
			   lv_obj_is_valid(ui_Bar_2_Value) &&
			   values_config[config_index].show_bar_value) {
		char value_str[16];
		// Format the value based on decimals configuration
		int decimals = values_config[config_index].decimals;
		if (decimals == 0) {
			snprintf(value_str, sizeof(value_str), "%d", (int)final_value);
		} else {
			snprintf(value_str, sizeof(value_str), "%.*f", decimals,
					 final_value);
		}
		lv_label_set_text(ui_Bar_2_Value, value_str);
	}

	lv_obj_t *menu_bar = menu_bar_objects[bar_index];
	if (menu_bar && lv_obj_is_valid(menu_bar) && ui_MenuScreen &&
		lv_obj_is_valid(ui_MenuScreen) && lv_scr_act() == ui_MenuScreen) {
		lv_bar_set_value(menu_bar, bar_value, LV_ANIM_OFF);
		lv_obj_set_style_bg_color(menu_bar, new_color,
								  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	}
}

// Asynchronous callback for updating the RPM UI (label and gauge)
void update_rpm_ui(void *param) {
	rpm_update_t *r_upd = (rpm_update_t *)param;

	// Check if both the RPM label and RPM gauge are valid.
	if ((ui_RPM_Value == NULL || lv_obj_get_screen(ui_RPM_Value) == NULL) ||
		(rpm_bar_gauge == NULL || lv_obj_get_screen(rpm_bar_gauge) == NULL)) {
		free(r_upd);
		return;
	}

	lv_label_set_text(ui_RPM_Value, r_upd->rpm_str);
	set_rpm_value(r_upd->rpm_value);

	// Update menu RPM value text when CAN bus is active and no limiter demo is
	// running
	if (!limiter_demo_active) {
		update_menu_rpm_value_text(r_upd->rpm_value);
	}

	free(r_upd);
}

// Immediate RPM update
static void update_rpm_ui_immediate(const char *rpm_str, int rpm_value) {
	if ((ui_RPM_Value == NULL || lv_obj_get_screen(ui_RPM_Value) == NULL) ||
		(rpm_bar_gauge == NULL || lv_obj_get_screen(rpm_bar_gauge) == NULL)) {
		return;
	}
	lv_label_set_text(ui_RPM_Value, rpm_str);
	set_rpm_value(rpm_value);
	if (!limiter_demo_active) {
		update_menu_rpm_value_text(rpm_value);
	}
}

// Asynchronous callback for updating the Speed label
void update_speed_ui(void *param) {
	speed_update_t *s_upd = (speed_update_t *)param;

	if (ui_Speed_Value == NULL || lv_obj_get_screen(ui_Speed_Value) == NULL) {
		free(s_upd);
		return;
	}

	lv_label_set_text(ui_Speed_Value, s_upd->speed_str);

	// Also update menu preview if it exists, is valid, and menu is visible
	if (menu_speed_value_label && lv_obj_is_valid(menu_speed_value_label) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		lv_label_set_text(menu_speed_value_label, s_upd->speed_str);
	}

	free(s_upd);
}

// Immediate speed update
static void update_speed_ui_immediate(const char *speed_str) {
	if (ui_Speed_Value == NULL || lv_obj_get_screen(ui_Speed_Value) == NULL) {
		return;
	}
	lv_label_set_text(ui_Speed_Value, speed_str);
	if (menu_speed_value_label && lv_obj_is_valid(menu_speed_value_label) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		lv_label_set_text(menu_speed_value_label, speed_str);
	}
}

// Helper function to check if a gear value matches a custom icon and should show icon
static bool should_show_gear_icon(uint32_t raw_value) {
	// Check if custom gear mode is enabled
	if (values_config[GEAR_VALUE_ID - 1].gear_detection_mode != 0) {
		return false; // Only custom mode supports icons
	}
	
	// Check each custom icon value
	for (int i = 0; i < 7; i++) {
		uint32_t icon_value = values_config[GEAR_VALUE_ID - 1].custom_icon_values[i];
		uint8_t icon_type = values_config[GEAR_VALUE_ID - 1].custom_icon_types[i];
		
		// Check if this icon is KEY type (1) and is configured (not UINT32_MAX)
		// Allow 0 as a valid configured value
		if (icon_type == 1 && icon_value != UINT32_MAX) {
			// Exact match required for icon display
			if (icon_value == raw_value) {
				return true;
			}
		}
	}
	return false;
}

// Asynchronous callback for updating the Gear label
void update_gear_ui(void *param) {
	gear_update_t *g_upd = (gear_update_t *)param;

	if (ui_GEAR_Value == NULL || lv_obj_get_screen(ui_GEAR_Value) == NULL) {
		free(g_upd);
		return;
	}

	// Check if we should show icon instead of text
	bool show_icon = should_show_gear_icon(g_upd->raw_value);
	
	if (show_icon) {
		// Show icon, hide text
		if (ui_GEAR_Icon && lv_obj_is_valid(ui_GEAR_Icon)) {
			lv_obj_clear_flag(ui_GEAR_Icon, LV_OBJ_FLAG_HIDDEN);
		}
		lv_obj_add_flag(ui_GEAR_Value, LV_OBJ_FLAG_HIDDEN);
	} else {
		// Show text, hide icon
		if (ui_GEAR_Icon && lv_obj_is_valid(ui_GEAR_Icon)) {
			lv_obj_add_flag(ui_GEAR_Icon, LV_OBJ_FLAG_HIDDEN);
		}
		lv_obj_clear_flag(ui_GEAR_Value, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(ui_GEAR_Value, g_upd->gear_str);
	}

	// Also update menu preview if it exists, is valid, and menu is visible
	if (menu_gear_value_label && lv_obj_is_valid(menu_gear_value_label) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		// Update menu icon if it exists
		extern lv_obj_t * menu_gear_icon;
		if (menu_gear_icon && lv_obj_is_valid(menu_gear_icon)) {
			if (show_icon) {
				lv_obj_clear_flag(menu_gear_icon, LV_OBJ_FLAG_HIDDEN);
				lv_obj_add_flag(menu_gear_value_label, LV_OBJ_FLAG_HIDDEN);
			} else {
				lv_obj_add_flag(menu_gear_icon, LV_OBJ_FLAG_HIDDEN);
				lv_obj_clear_flag(menu_gear_value_label, LV_OBJ_FLAG_HIDDEN);
				lv_label_set_text(menu_gear_value_label, g_upd->gear_str);
			}
		} else {
			lv_label_set_text(menu_gear_value_label, g_upd->gear_str);
		}
	}

	free(g_upd);
}

// Immediate gear update
static void update_gear_ui_immediate(const char *gear_str, uint32_t raw_value) {
	if (ui_GEAR_Value == NULL || lv_obj_get_screen(ui_GEAR_Value) == NULL) {
		return;
	}
	
	// Check if we should show icon instead of text
	bool show_icon = should_show_gear_icon(raw_value);
	
	if (show_icon) {
		// Show icon, hide text
		if (ui_GEAR_Icon && lv_obj_is_valid(ui_GEAR_Icon)) {
			lv_obj_clear_flag(ui_GEAR_Icon, LV_OBJ_FLAG_HIDDEN);
		}
		lv_obj_add_flag(ui_GEAR_Value, LV_OBJ_FLAG_HIDDEN);
	} else {
		// Show text, hide icon
		if (ui_GEAR_Icon && lv_obj_is_valid(ui_GEAR_Icon)) {
			lv_obj_add_flag(ui_GEAR_Icon, LV_OBJ_FLAG_HIDDEN);
		}
		lv_obj_clear_flag(ui_GEAR_Value, LV_OBJ_FLAG_HIDDEN);
		lv_label_set_text(ui_GEAR_Value, gear_str);
	}
	
	if (menu_gear_value_label && lv_obj_is_valid(menu_gear_value_label) &&
		ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen) &&
		lv_scr_act() == ui_MenuScreen) {
		// Update menu icon if it exists
		extern lv_obj_t * menu_gear_icon;
		if (menu_gear_icon && lv_obj_is_valid(menu_gear_icon)) {
			if (show_icon) {
				lv_obj_clear_flag(menu_gear_icon, LV_OBJ_FLAG_HIDDEN);
				lv_obj_add_flag(menu_gear_value_label, LV_OBJ_FLAG_HIDDEN);
			} else {
				lv_obj_add_flag(menu_gear_icon, LV_OBJ_FLAG_HIDDEN);
				lv_obj_clear_flag(menu_gear_value_label, LV_OBJ_FLAG_HIDDEN);
				lv_label_set_text(menu_gear_value_label, gear_str);
			}
		} else {
			lv_label_set_text(menu_gear_value_label, gear_str);
		}
	}
}

// Asynchronous callback for updating a warning indicator
void update_warning_ui(void *param) {
	uint8_t warning_idx = *(uint8_t *)param;
	free(param);

	if (warning_circles[warning_idx] == NULL ||
		lv_obj_get_screen(warning_circles[warning_idx]) == NULL) {
		return;
	}

	lv_color_t new_color = warning_configs[warning_idx].current_state
							   ? warning_configs[warning_idx].active_color
							   : THEME_COLOR_INACTIVE; // Default "off" color.

	lv_obj_set_style_bg_color(warning_circles[warning_idx], new_color,
							  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Also update the warning label visibility
	if (warning_labels[warning_idx] &&
		lv_obj_is_valid(warning_labels[warning_idx])) {
		if (warning_configs[warning_idx].current_state) {
			lv_obj_clear_flag(warning_labels[warning_idx], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(warning_labels[warning_idx], LV_OBJ_FLAG_HIDDEN);
		}
	}
}

// Immediate warning update
static void update_warning_ui_immediate(uint8_t warning_idx) {
	if (warning_idx >= 8)
		return;
	if (warning_circles[warning_idx] == NULL ||
		lv_obj_get_screen(warning_circles[warning_idx]) == NULL) {
		return;
	}
	lv_color_t new_color = warning_configs[warning_idx].current_state
							   ? warning_configs[warning_idx].active_color
							   : THEME_COLOR_INACTIVE;
	lv_obj_set_style_bg_color(warning_circles[warning_idx], new_color,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	if (warning_labels[warning_idx] &&
		lv_obj_is_valid(warning_labels[warning_idx])) {
		if (warning_configs[warning_idx].current_state) {
			lv_obj_clear_flag(warning_labels[warning_idx], LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_add_flag(warning_labels[warning_idx], LV_OBJ_FLAG_HIDDEN);
		}
	}
}

// Immediate indicator update
static void update_indicator_ui_immediate(uint8_t indicator_idx) {
	if (indicator_idx >= 2)
		return;
	bool current_state = indicator_configs[indicator_idx].current_state;
	lv_obj_t *indicator_obj =
		(indicator_idx == 0) ? ui_Indicator_Left : ui_Indicator_Right;
	if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
		if (current_state) {
			if (indicator_configs[indicator_idx].animation_enabled) {
				lv_obj_set_style_opa(indicator_obj, 255,
									 LV_PART_MAIN | LV_STATE_DEFAULT);
				indicator_animation_state = false;
				if (indicator_animation_timer) {
					lv_timer_resume(indicator_animation_timer);
				}
			} else {
				lv_obj_set_style_opa(indicator_obj, 255,
									 LV_PART_MAIN | LV_STATE_DEFAULT);
			}
		} else {
			lv_obj_set_style_opa(indicator_obj, 50,
								 LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	}
	update_config_preview(indicator_idx);
}

// Function to update the config preview
void update_config_preview(uint8_t indicator_idx) {
	if (preview_indicator_config && lv_obj_is_valid(preview_indicator_config) &&
		preview_status_text_config &&
		lv_obj_is_valid(preview_status_text_config) &&
		preview_indicator_idx == indicator_idx) {

		printf("Updating config preview for indicator %d: %s\n", indicator_idx,
			   indicator_configs[indicator_idx].current_state ? "ACTIVE"
															  : "INACTIVE");

		// Update preview indicator opacity
		if (indicator_configs[indicator_idx].current_state) {
			lv_obj_set_style_opa(preview_indicator_config, 255,
								 LV_PART_MAIN |
									 LV_STATE_DEFAULT); // 100% opacity
		} else {
			lv_obj_set_style_opa(preview_indicator_config, 50,
								 LV_PART_MAIN |
									 LV_STATE_DEFAULT); // 50% opacity
		}

		// Update preview status text
		lv_label_set_text_fmt(preview_status_text_config, "%s INDICATOR\n%s",
							  indicator_idx == 0 ? "LEFT" : "RIGHT",
							  indicator_configs[indicator_idx].current_state
								  ? "ACTIVE"
								  : "INACTIVE");

		// Update text color based on state
		if (indicator_configs[indicator_idx].current_state) {
			lv_obj_set_style_text_color(preview_status_text_config,
										THEME_COLOR_GREEN,
										0); // Green when active
		} else {
			lv_obj_set_style_text_color(preview_status_text_config,
										THEME_COLOR_TEXT_MUTED,
										0); // Gray when inactive
		}
	}
}

/** Apply analog (wire) indicator state. Only updates indicators with input_source == Wire (0). */
void indicator_apply_analog_state(bool left_on, bool right_on) {
	for (int i = 0; i < 2; i++) {
		if (indicator_configs[i].input_source != 0)
			continue; /* CAN BUS - leave state to CAN */
		bool new_state = (i == 0) ? left_on : right_on;
		indicator_configs[i].current_state = new_state;
		previous_indicator_states[i] = new_state;
	}
	update_indicator_ui_immediate(0);
	update_indicator_ui_immediate(1);
}

// Asynchronous callback for updating an indicator
void update_indicator_ui(void *param) {
	uint8_t indicator_idx = *(uint8_t *)param;
	free(param);

	if (indicator_idx >= 2)
		return;

	// Check if state actually changed to avoid redundant updates
	bool current_state = indicator_configs[indicator_idx].current_state;
	if (previous_indicator_states[indicator_idx] == current_state) {
		return; // No change, skip update
	}

	// Update the previous state
	previous_indicator_states[indicator_idx] = current_state;

	// Update main indicator opacity based on state
	lv_obj_t *indicator_obj =
		(indicator_idx == 0) ? ui_Indicator_Left : ui_Indicator_Right;

	if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
		if (current_state) {
			printf("Indicator %d: Setting to ACTIVE\n", indicator_idx);
			if (indicator_configs[indicator_idx].animation_enabled) {
				printf("Indicator %d: Animation enabled - starting timer\n",
					   indicator_idx);
				// Always start with 100% opacity for immediate visibility
				lv_obj_set_style_opa(indicator_obj, 255,
									 LV_PART_MAIN | LV_STATE_DEFAULT);
				// Set animation state to false so first timer tick will toggle
				// to true (100%)
				indicator_animation_state = false; // Next toggle will be bright
				if (indicator_animation_timer) {
					lv_timer_resume(indicator_animation_timer);
				}
			} else {
				printf("Indicator %d: Solid mode - 100%% opacity\n",
					   indicator_idx);
				lv_obj_set_style_opa(indicator_obj, 255,
									 LV_PART_MAIN | LV_STATE_DEFAULT);
			}
		} else {
			printf("Indicator %d: Setting to INACTIVE (50%% opacity)\n",
				   indicator_idx);
			lv_obj_set_style_opa(indicator_obj, 50,
								 LV_PART_MAIN |
									 LV_STATE_DEFAULT); // 50% opacity (default)
		}
	} else {
		printf("Indicator %d: Object is null or invalid\n", indicator_idx);
	}

	// Also update the config preview if it's visible
	update_config_preview(indicator_idx);
}

static int64_t extract_bits(const uint8_t *data, uint8_t bit_offset,
							uint8_t bit_length, int endian, bool is_signed) {
	// Fast path for common cases
	if (bit_length == 0)
		return 0;
	if (bit_length > 64)
		bit_length = 64;

	uint64_t value = 0;

	if (endian == 0) { // Big Endian
		// Calculate byte boundaries
		uint8_t start_byte = bit_offset / 8;
		uint8_t end_byte = (bit_offset + bit_length - 1) / 8;
		uint8_t bytes_needed = end_byte - start_byte + 1;

		// Fast path for single byte extraction
		if (bytes_needed == 1) {
			uint8_t bit_pos = bit_offset % 8;
			uint8_t mask = ((1U << bit_length) - 1)
						   << (8 - bit_pos - bit_length);
			value = (data[start_byte] & mask) >> (8 - bit_pos - bit_length);
		}
		// Fast path for aligned multi-byte values (8, 16, 32 bits)
		else if ((bit_offset % 8) == 0 && (bit_length % 8) == 0) {
			for (int i = 0; i < bytes_needed && i < 8; i++) {
				value = (value << 8) | data[start_byte + i];
			}
		}
		// General case - optimized bit operations
		else {
			// Load all relevant bytes into a 64-bit word
			uint64_t word = 0;
			for (int i = 0; i < bytes_needed && i < 8; i++) {
				word = (word << 8) | data[start_byte + i];
			}
			// Shift and mask to extract the bits
			uint8_t shift = (bytes_needed * 8) - (bit_offset % 8) - bit_length;
			uint64_t mask = ((1ULL << bit_length) - 1);
			value = (word >> shift) & mask;
		}
	} else { // Little Endian
		// Calculate byte boundaries
		uint8_t start_byte = bit_offset / 8;
		uint8_t end_byte = (bit_offset + bit_length - 1) / 8;
		uint8_t bytes_needed = end_byte - start_byte + 1;

		// Fast path for single byte extraction
		if (bytes_needed == 1) {
			uint8_t bit_pos = bit_offset % 8;
			uint8_t mask = (1U << bit_length) - 1;
			value = (data[start_byte] >> bit_pos) & mask;
		}
		// Fast path for aligned multi-byte values (8, 16, 32 bits)
		else if ((bit_offset % 8) == 0 && (bit_length % 8) == 0) {
			for (int i = 0; i < bytes_needed && i < 8; i++) {
				value |= ((uint64_t)data[start_byte + i]) << (i * 8);
			}
		}
		// General case - optimized bit operations
		else {
			// Load all relevant bytes
			uint64_t word = 0;
			for (int i = 0; i < bytes_needed && i < 8; i++) {
				word |= ((uint64_t)data[start_byte + i]) << (i * 8);
			}
			// Shift and mask to extract the bits
			uint8_t shift = bit_offset % 8;
			uint64_t mask = ((1ULL << bit_length) - 1);
			value = (word >> shift) & mask;
		}
	}

	// Handle signed values with optimized sign extension
	if (is_signed && (value & ((uint64_t)1 << (bit_length - 1)))) {
		// Sign extend using arithmetic right shift
		value |= (~((1ULL << bit_length) - 1));
	}

	return (int64_t)value;
}

// ---------- Stage 2: O(1) dispatch mapping from CAN ID to affected indices
// ----------
typedef struct {
	uint32_t can_id;
	uint8_t num_warning;
	uint8_t warning_indices[8];
	uint8_t num_indicator;
	uint8_t indicator_indices[2];
	uint8_t num_values;
	uint8_t value_indices[13];
} can_dispatch_entry_t;

static can_dispatch_entry_t can_dispatch_entries[32];
static int can_dispatch_count = 0;
static int16_t can_id_to_dispatch_index[2048]; // -1 if none

static int get_or_add_dispatch_entry(uint32_t can_id) {
	uint32_t sid = can_id & 0x7FF;
	if (sid < 2048) {
		int16_t idx = can_id_to_dispatch_index[sid];
		if (idx >= 0)
			return idx;
	}
	if (can_dispatch_count >=
		(int)(sizeof(can_dispatch_entries) / sizeof(can_dispatch_entries[0]))) {
		return -1;
	}
	int idx = can_dispatch_count++;
	can_dispatch_entries[idx].can_id = sid;
	can_dispatch_entries[idx].num_warning = 0;
	can_dispatch_entries[idx].num_indicator = 0;
	can_dispatch_entries[idx].num_values = 0;
	if (sid < 2048)
		can_id_to_dispatch_index[sid] = (int16_t)idx;
	return idx;
}

void rebuild_can_dispatch(void) {
	for (int i = 0; i < 2048; i++)
		can_id_to_dispatch_index[i] = -1;
	can_dispatch_count = 0;

	for (int i = 0; i < 8; i++) {
		if (warning_configs[i].can_id != 0) {
			int idx = get_or_add_dispatch_entry(warning_configs[i].can_id);
			if (idx >= 0 && can_dispatch_entries[idx].num_warning < 8) {
				can_dispatch_entries[idx]
					.warning_indices[can_dispatch_entries[idx].num_warning++] =
					(uint8_t)i;
			}
		}
	}
	for (int i = 0; i < 2; i++) {
		if (indicator_configs[i].can_id != 0) {
			int idx = get_or_add_dispatch_entry(indicator_configs[i].can_id);
			if (idx >= 0 && can_dispatch_entries[idx].num_indicator < 2) {
				can_dispatch_entries[idx].indicator_indices
					[can_dispatch_entries[idx].num_indicator++] = (uint8_t)i;
			}
		}
	}
	for (int i = 0; i < 13; i++) {
		if (values_config[i].enabled && values_config[i].can_id != 0) {
			int idx = get_or_add_dispatch_entry(values_config[i].can_id);
			if (idx >= 0 && can_dispatch_entries[idx].num_values < 13) {
				can_dispatch_entries[idx]
					.value_indices[can_dispatch_entries[idx].num_values++] =
					(uint8_t)i;
			}
		}
	}
}

void process_can_message(const twai_message_t *message) {
	static uint64_t last_panel_updates[8] = {0};
	static uint64_t last_bar_updates[2] = {0};
	uint64_t current_time = esp_timer_get_time() / 1000; // ms
	uint32_t received_id = message->identifier;

	// Reset tracking variables after screen recreation to force UI updates
	if (reset_can_tracking) {
		reset_can_tracking = false;
		memset(last_panel_updates, 0, sizeof(last_panel_updates));
		memset(last_bar_updates, 0, sizeof(last_bar_updates));
		memset(previous_bit_states, 0, sizeof(previous_bit_states));
		// Reset CAN timeout tracking
		memset(last_panel_can_received, 0, sizeof(last_panel_can_received));
		memset(last_bar_can_received, 0, sizeof(last_bar_can_received));
		last_speed_can_received = 0;
		last_gear_can_received = 0;
		last_rpm_can_received = 0;
		// Force immediate updates by setting all times to 0
		for (int i = 0; i < 2; i++) {
			previous_bar_values[i] =
				-999; // Use impossible values to force bar updates
		}
	}

	// Fast-dispatch path using prebuilt mapping; fallback to linear scans if
	// not found
	int16_t didx =
		(received_id <= 0x7FF) ? can_id_to_dispatch_index[received_id] : -1;
	if (didx >= 0) {
		// Precompute 64-bit data value once for bit-extraction consumers
		uint64_t data_value = 0;
		for (int j = 0; j < message->data_length_code && j < 8; j++) {
			data_value |= (uint64_t)message->data[j] << (j * 8);
		}

		// Warnings
		for (uint8_t wi = 0; wi < can_dispatch_entries[didx].num_warning;
			 wi++) {
			int i = can_dispatch_entries[didx].warning_indices[wi];
			bool current_bit_state =
				(data_value >> warning_configs[i].bit_position) & 0x01;
			// Apply inversion if enabled
			if (warning_configs[i].invert_toggle) {
				current_bit_state = !current_bit_state;
			}
			if (warning_configs[i].is_momentary) {
				if (current_bit_state != warning_configs[i].current_state) {
					warning_configs[i].current_state = current_bit_state;
					if (current_bit_state) {
						last_signal_times[i] = current_time;
					}
				}
			} else {
				if (previous_bit_states[i] && !current_bit_state) {
					if (!toggle_debounce[i]) {
						toggle_debounce[i] = true;
						toggle_start_time[i] = current_time;
						warning_configs[i].current_state =
							!warning_configs[i].current_state;
					}
				} else if (!current_bit_state) {
					toggle_debounce[i] = false;
				}
			}
			previous_bit_states[i] = current_bit_state;
			update_warning_ui_immediate((uint8_t)i);
		}

		// Indicators (only when source is CAN BUS)
		for (uint8_t ii = 0; ii < can_dispatch_entries[didx].num_indicator;
			 ii++) {
			int i = can_dispatch_entries[didx].indicator_indices[ii];
			if (indicator_configs[i].input_source != 1)
				continue; /* Wire - state driven by analog */
			bool current_bit_state =
				(data_value >> indicator_configs[i].bit_position) & 0x01;
			if (indicator_configs[i].is_momentary) {
				if (current_bit_state != indicator_configs[i].current_state) {
					indicator_configs[i].current_state = current_bit_state;
				}
			} else {
				if (previous_indicator_bit_states[i] && !current_bit_state) {
					if (!indicator_toggle_debounce[i]) {
						indicator_toggle_debounce[i] = true;
						indicator_toggle_start_time[i] = current_time;
						indicator_configs[i].current_state =
							!indicator_configs[i].current_state;
					}
				} else if (!current_bit_state) {
					indicator_toggle_debounce[i] = false;
				}
			}
			previous_indicator_bit_states[i] = current_bit_state;
			update_indicator_ui_immediate((uint8_t)i);
		}

		// Values
		for (uint8_t vi = 0; vi < can_dispatch_entries[didx].num_values; vi++) {
			int i = can_dispatch_entries[didx].value_indices[vi];
			if (values_config[i].enabled) {
				uint8_t value_id = i + 1;
				int64_t raw_value = extract_bits(
					message->data, values_config[i].bit_start,
					values_config[i].bit_length, values_config[i].endianess,
					values_config[i].is_signed);
				double final_value =
					(double)raw_value * values_config[i].scale +
					values_config[i].value_offset;

				if (i < 8) {
					last_panel_can_received[i] = current_time;
					if (i == 6) {
						char new_value_str[EXAMPLE_MAX_CHAR_SIZE];
						if (values_config[i].decimals == 0) {
							snprintf(new_value_str, sizeof(new_value_str), "%d",
									 (int)final_value);
						} else {
							snprintf(new_value_str, sizeof(new_value_str),
									 "%.*f", values_config[i].decimals,
									 final_value);
						}
						if (strcmp(new_value_str, previous_values[i]) != 0) {
							strcpy(previous_values[i], new_value_str);
							update_panel_ui_immediate((uint8_t)i, new_value_str,
													  final_value);
						}
					} else {
						if (current_time - last_panel_updates[i] >= 25) {
							char new_value_str[EXAMPLE_MAX_CHAR_SIZE];
							if (values_config[i].decimals == 0) {
								snprintf(new_value_str, sizeof(new_value_str),
										 "%d", (int)final_value);
							} else {
								snprintf(new_value_str, sizeof(new_value_str),
										 "%.*f", values_config[i].decimals,
										 final_value);
							}
							if (strcmp(new_value_str, previous_values[i]) !=
								0) {
								strcpy(previous_values[i], new_value_str);
								update_panel_ui_immediate(
									(uint8_t)i, new_value_str, final_value);
							}
							last_panel_updates[i] = current_time;
						}
					}
					continue;
				}

				if (value_id == BAR1_VALUE_ID || value_id == BAR2_VALUE_ID) {
					int bar_index = value_id - BAR1_VALUE_ID;
					if (values_config[bar_index + BAR1_VALUE_ID - 1].fuel_sender) continue;
					last_bar_can_received[bar_index] = current_time;
					// Match panel refresh cadence: max 40 Hz (every 25 ms) and
					// update on any value change (threshold handled separately).
					if (current_time - last_bar_updates[bar_index] >= 25) {
						if (fabs(final_value - previous_bar_values[bar_index]) >=
							BAR_UPDATE_THRESHOLD) {
							previous_bar_values[bar_index] = final_value;
							lv_obj_t *bar_obj =
								(value_id == BAR1_VALUE_ID) ? ui_Bar_1 : ui_Bar_2;
							if (bar_obj) {
								int32_t bar_value = (int32_t)final_value;
								if (bar_value < values_config[i].bar_min)
									bar_value = values_config[i].bar_min;
								else if (bar_value > values_config[i].bar_max)
									bar_value = values_config[i].bar_max;

								// Apply inversion if enabled
								if (values_config[i].invert_bar_value) {
									bar_value = values_config[i].bar_max +
												values_config[i].bar_min -
												bar_value;
								}

								update_bar_ui_immediate(bar_index, bar_value,
														final_value, i);
							}
							last_bar_updates[bar_index] = current_time;
						}
					}
					continue;
				}

				if (value_id == RPM_VALUE_ID) {
					last_rpm_can_received = current_time;
					int rpm_value = (int)final_value;
					int gauge_rpm_value =
						rpm_value < 0
							? 0
							: (rpm_value > rpm_gauge_max ? rpm_gauge_max
														 : rpm_value);
					int display_rpm_value = (int)((float)rpm_value * 1.0229f);
					char rpm_str[EXAMPLE_MAX_CHAR_SIZE];
					snprintf(rpm_str, sizeof(rpm_str), "%d", display_rpm_value);
					static int last_rpm_value = -1;
					if (strcmp(rpm_str, previous_values[i]) != 0 ||
						rpm_value != last_rpm_value) {
						strcpy(previous_values[i], rpm_str);
						last_rpm_value = rpm_value;
						update_rpm_ui_immediate(rpm_str, gauge_rpm_value);
					}
				} else if (value_id == SPEED_VALUE_ID) {
					last_speed_can_received = current_time;
					double speed_value = final_value;
					char speed_str[EXAMPLE_MAX_CHAR_SIZE];
					snprintf(speed_str, sizeof(speed_str), "%.0f", speed_value);
					if (strcmp(speed_str, previous_values[i]) != 0) {
						strcpy(previous_values[i], speed_str);
						update_speed_ui_immediate(speed_str);
					}
				} else if (value_id == GEAR_VALUE_ID) {
					// Skip CAN gear processing if in Speed/RPM Ratio mode
					if (values_config[GEAR_VALUE_ID - 1].gear_detection_mode == 4) {
						continue;
					}
					last_gear_can_received = current_time;
					
					// Get raw value BEFORE scaling/offset for icon matching
					// This is the actual CAN bus value that should match custom_icon_values
					int64_t gear_raw_value = extract_bits(
						message->data, values_config[GEAR_VALUE_ID - 1].bit_start,
						values_config[GEAR_VALUE_ID - 1].bit_length, 
						values_config[GEAR_VALUE_ID - 1].endianess,
						values_config[GEAR_VALUE_ID - 1].is_signed);
					
					char gear_str[EXAMPLE_MAX_CHAR_SIZE];
					if (strcasecmp(label_texts[GEAR_VALUE_ID - 1], "GEAR") ==
						0) {
						uint8_t gear_mode = values_config[GEAR_VALUE_ID - 1]
												.gear_detection_mode;
						if (gear_mode == 1) {
							// MaxxECU gear detection: -3=Park, -1=Reverse, 0=Neutral, >0=Forward gears
							if (final_value == 0) {
								snprintf(gear_str, sizeof(gear_str), "N");
							} else if (final_value == -1) {
								snprintf(gear_str, sizeof(gear_str), "R");
							} else if (final_value >= 1 && final_value <= 10) {
								snprintf(gear_str, sizeof(gear_str), "%d",
										 (int)final_value);
							} else {
								snprintf(gear_str, sizeof(gear_str), "-");
							}
						} else if (gear_mode == 2) {
							if (final_value == 0) {
								snprintf(gear_str, sizeof(gear_str), "N");
							} else if (final_value == 255 ||
									   final_value == 0xFE) {
								snprintf(gear_str, sizeof(gear_str), "R");
							} else if (final_value >= 1 && final_value <= 8) {
								snprintf(gear_str, sizeof(gear_str), "%d",
										 (int)final_value);
							} else {
								snprintf(gear_str, sizeof(gear_str), "-");
							}
						} else {
							// Custom gear detection - check if received value
							// matches any configured gear value
							bool gear_found = false;
							for (int gear_idx = 0; gear_idx < 14;
								 gear_idx++) { // 0-13: P, R, N, D, 1-10
							if (values_config[GEAR_VALUE_ID - 1]
										.gear_custom_values[gear_idx] ==
									(uint32_t)gear_raw_value) {
								if (gear_idx == 0) {
									snprintf(gear_str, sizeof(gear_str),
											 "P");
								} else if (gear_idx == 1) {
									snprintf(gear_str, sizeof(gear_str),
											 "R");
								} else if (gear_idx == 2) {
									snprintf(gear_str, sizeof(gear_str),
											 "N");
								} else if (gear_idx == 3) {
									snprintf(gear_str, sizeof(gear_str),
											 "D");
								} else {
									snprintf(gear_str, sizeof(gear_str),
											 "%d", gear_idx - 3);
								}
								gear_found = true;
								break;
							}
						}
						if (!gear_found) {
							snprintf(
								gear_str, sizeof(gear_str),
								"-"); // Show dash if no gear value matches
						}
					}
				} else {
					snprintf(gear_str, sizeof(gear_str), "%.0f",
							 final_value);
				}
				// Always update if gear string changed OR if raw value changed (for icon matching)
				// This ensures icon appears even if gear_str is "-" but icon value matches
				bool gear_str_changed = (strcmp(gear_str, previous_values[GEAR_VALUE_ID - 1]) != 0);
				static uint32_t last_raw_value = 0;
				bool raw_value_changed = ((uint32_t)gear_raw_value != last_raw_value);
				
				if (gear_str_changed || raw_value_changed) {
					if (gear_str_changed) {
						strcpy(previous_values[GEAR_VALUE_ID - 1], gear_str);
					}
					last_raw_value = (uint32_t)gear_raw_value;
					// Use raw value (before scaling/offset) for icon matching
					update_gear_ui_immediate(gear_str, (uint32_t)gear_raw_value);
				}
				}
			}
		}
		return; // handled via fast path
	}

	// Process warning configurations (for warnings 0-7)
	for (int i = 0; i < 8; i++) {
		if (warning_configs[i].can_id == received_id) {
			// Create 64-bit value from message data
			uint64_t data_value = 0;
			for (int j = 0; j < message->data_length_code && j < 8; j++) {
				data_value |= (uint64_t)message->data[j] << (j * 8);
			}

			// Extract bit using 64-bit position
			bool current_bit_state =
				(data_value >> warning_configs[i].bit_position) & 0x01;
			// Apply inversion if enabled
			if (warning_configs[i].invert_toggle) {
				current_bit_state = !current_bit_state;
			}

			if (warning_configs[i].is_momentary) {
				// For momentary mode: activate warning directly based on bit
				// state (active high) This will show warning when bit is 1,
				// hide when bit is 0
				if (current_bit_state != warning_configs[i].current_state) {
					warning_configs[i].current_state = current_bit_state;
					if (current_bit_state) {
						last_signal_times[i] = current_time;
					}
				}
			} else {
				// For toggle mode: toggle on falling edge (1->0 transition)
				if (previous_bit_states[i] && !current_bit_state) {
					if (!toggle_debounce[i]) {
						toggle_debounce[i] = true;
						toggle_start_time[i] = current_time;
						warning_configs[i].current_state =
							!warning_configs[i].current_state;
					}
				} else if (!current_bit_state) {
					toggle_debounce[i] = false;
				}
			}
			previous_bit_states[i] = current_bit_state;

			// Update warning UI
			uint8_t *w_idx = malloc(sizeof(uint8_t));
			if (w_idx) {
				*w_idx = i;
				lv_async_call(update_warning_ui, w_idx);
			}
		}
	}

	// Process brightness dimmer switch
	extern brightness_dimmer_config_t dimmer_config;
	extern bool previous_dimmer_bit_state;
	if (dimmer_config.enabled && dimmer_config.can_id == received_id) {
		// Create 64-bit value from message data
		uint64_t data_value = 0;
		for (int j = 0; j < message->data_length_code && j < 8; j++) {
			data_value |= (uint64_t)message->data[j] << (j * 8);
		}
		
		// Extract bit
		bool current_bit_state = (data_value >> dimmer_config.bit_position) & 0x01;
		
		// Apply inversion if enabled
		if (dimmer_config.invert_toggle) {
			current_bit_state = !current_bit_state;
		}
		
		if (dimmer_config.is_momentary) {
			// Momentary mode: set brightness when bit is active
			if (current_bit_state) {
				extern void set_display_brightness(int percent);
				set_display_brightness(dimmer_config.brightness_value);
			}
		} else {
			// Toggle mode: toggle brightness on falling edge (1->0 transition)
			if (previous_dimmer_bit_state && !current_bit_state) {
				extern void set_display_brightness(int percent);
				extern uint8_t current_brightness;
				// Toggle: if currently at dimmer brightness, restore to 100%, otherwise set to dimmer brightness
				if (current_brightness == dimmer_config.brightness_value) {
					set_display_brightness(100);
				} else {
					set_display_brightness(dimmer_config.brightness_value);
				}
			}
		}
		previous_dimmer_bit_state = current_bit_state;
	}

	// Process indicator configurations (for left and right indicators; only when source is CAN BUS)
	for (int i = 0; i < 2; i++) {
		if (indicator_configs[i].input_source != 1)
			continue; /* Wire - state driven by analog */
		if (indicator_configs[i].can_id == received_id) {
			// Create 64-bit value from message data
			uint64_t data_value = 0;
			for (int j = 0; j < message->data_length_code && j < 8; j++) {
				data_value |= (uint64_t)message->data[j] << (j * 8);
			}

			// Extract bit using 64-bit position
			bool current_bit_state =
				(data_value >> indicator_configs[i].bit_position) & 0x01;

			if (indicator_configs[i].is_momentary) {
				// For momentary mode: activate indicator directly based on bit
				// state (active high)
				if (current_bit_state != indicator_configs[i].current_state) {
					indicator_configs[i].current_state = current_bit_state;
				}
			} else {
				// For toggle mode: toggle on falling edge (1->0 transition)
				if (previous_indicator_bit_states[i] && !current_bit_state) {
					if (!indicator_toggle_debounce[i]) {
						indicator_toggle_debounce[i] = true;
						indicator_toggle_start_time[i] = current_time;
						indicator_configs[i].current_state =
							!indicator_configs[i].current_state;
					}
				} else if (!current_bit_state) {
					indicator_toggle_debounce[i] = false;
				}
			}
			previous_indicator_bit_states[i] = current_bit_state;

			// Update indicator UI
			uint8_t *ind_idx = malloc(sizeof(uint8_t));
			if (ind_idx) {
				*ind_idx = i;
				lv_async_call(update_indicator_ui, ind_idx);
			}
		}
	}

	// Process value configurations
	for (int i = 0; i < 13; i++) {
		if (values_config[i].enabled &&
			values_config[i].can_id == received_id) {
			uint8_t value_id = i + 1;
			int64_t raw_value = extract_bits(
				message->data, values_config[i].bit_start,
				values_config[i].bit_length, values_config[i].endianess,
				values_config[i].is_signed);
			double final_value = (double)raw_value * values_config[i].scale +
								 values_config[i].value_offset;

			// Handle panels (values 1-8)
			if (i < 8) {
				// Update CAN received timestamp for this panel
				last_panel_can_received[i] = current_time;

				// Special handling for wideband (panel 7)
				if (i == 6) { // Panel 7 (wideband)
					char new_value_str[EXAMPLE_MAX_CHAR_SIZE];
					if (values_config[i].decimals == 0) {
						snprintf(new_value_str, sizeof(new_value_str), "%d",
								 (int)final_value);
					} else {
						snprintf(new_value_str, sizeof(new_value_str), "%.*f",
								 values_config[i].decimals, final_value);
					}
					if (strcmp(new_value_str, previous_values[i]) != 0) {
						strcpy(previous_values[i], new_value_str);
						panel_update_t *p_upd = malloc(sizeof(panel_update_t));
						if (p_upd) {
							p_upd->panel_index = i;
							strcpy(p_upd->value_str, new_value_str);
							p_upd->final_value = final_value;
							lv_async_call(update_panel_ui, p_upd);
						}
					}
				} else {
					// Other panels with 25ms update rate
					if (current_time - last_panel_updates[i] >= 25) {
						char new_value_str[EXAMPLE_MAX_CHAR_SIZE];
						if (values_config[i].decimals == 0) {
							snprintf(new_value_str, sizeof(new_value_str), "%d",
									 (int)final_value);
						} else {
							snprintf(new_value_str, sizeof(new_value_str),
									 "%.*f", values_config[i].decimals,
									 final_value);
						}
						if (strcmp(new_value_str, previous_values[i]) != 0) {
							strcpy(previous_values[i], new_value_str);
							panel_update_t *p_upd =
								malloc(sizeof(panel_update_t));
							if (p_upd) {
								p_upd->panel_index = i;
								strcpy(p_upd->value_str, new_value_str);
								p_upd->final_value = final_value;
								lv_async_call(update_panel_ui, p_upd);
							}
						}
						last_panel_updates[i] = current_time;
					}
				}
				continue;
			}

			// Handle bars (for BAR1 and BAR2)
			if (value_id == BAR1_VALUE_ID || value_id == BAR2_VALUE_ID) {
				int bar_index = value_id - BAR1_VALUE_ID;
				if (values_config[bar_index + BAR1_VALUE_ID - 1].fuel_sender) continue;
				// Update CAN received timestamp for this bar
				last_bar_can_received[bar_index] = current_time;

				// Update both bars immediately without throttling for better
				// responsiveness
				if (fabs(final_value - previous_bar_values[bar_index]) >=
					BAR_UPDATE_THRESHOLD) {
					previous_bar_values[bar_index] = final_value;
					lv_obj_t *bar_obj =
						(value_id == BAR1_VALUE_ID) ? ui_Bar_1 : ui_Bar_2;
					if (bar_obj) {
						int32_t bar_value = (int32_t)final_value;
						// Clamp the value per configuration.
						if (bar_value < values_config[i].bar_min) {
							bar_value = values_config[i].bar_min;
						} else if (bar_value > values_config[i].bar_max) {
							bar_value = values_config[i].bar_max;
						}
						
						// Apply inversion if enabled
						if (values_config[i].invert_bar_value) {
							bar_value = values_config[i].bar_max + values_config[i].bar_min - bar_value;
						}

						// Create and fill our bar update data.
						bar_update_t *b_upd = malloc(sizeof(bar_update_t));
						if (b_upd) {
							b_upd->bar_index = bar_index;
							b_upd->bar_value = bar_value;
							b_upd->final_value = final_value;
							b_upd->config_index = i;
							b_upd->is_timeout = false;
							lv_async_call(update_bar_ui, b_upd);
						}
					}
				}
				continue;
			}

			// Handle RPM
			if (value_id == RPM_VALUE_ID) {
				// Update CAN received timestamp for RPM
				last_rpm_can_received = current_time;

				// Always process RPM updates for better responsiveness
				int rpm_value = (int)final_value;

				// For the gauge bar, limit to rpm_gauge_max
				int gauge_rpm_value =
					rpm_value < 0 ? 0
								  : (rpm_value > rpm_gauge_max ? rpm_gauge_max
															   : rpm_value);

				// For the display value, apply 102.3% scaling to the actual CAN
				// value (no gauge limit)
				int display_rpm_value =
					(int)((float)rpm_value *
						  1.0229f); // 102.3% scaling of actual CAN value

				// Update the display string with the scaled actual CAN value
				char rpm_str[EXAMPLE_MAX_CHAR_SIZE];
				snprintf(rpm_str, sizeof(rpm_str), "%d", display_rpm_value);

				// Update if string changed OR if actual RPM value changed
				// This ensures limiter effects activate immediately
				static int last_rpm_value = -1;
				if (strcmp(rpm_str, previous_values[i]) != 0 ||
					rpm_value != last_rpm_value) {
					strcpy(previous_values[i], rpm_str);
					last_rpm_value = rpm_value;

					rpm_update_t *r_upd = malloc(sizeof(rpm_update_t));
					if (r_upd) {
						strcpy(r_upd->rpm_str, rpm_str);
						r_upd->rpm_value =
							gauge_rpm_value; // Use gauge-limited value for bar
						lv_async_call(update_rpm_ui, r_upd);
					}
				}
			}
			// Handle Speed
			else if (value_id == SPEED_VALUE_ID) {
				// Update CAN received timestamp for speed
				last_speed_can_received = current_time;

				// Use raw CAN bus value without conversion - let CAN provide
				// the value in desired units
				double speed_value = final_value;

				char speed_str[EXAMPLE_MAX_CHAR_SIZE];
				snprintf(speed_str, sizeof(speed_str), "%.0f", speed_value);

				if (strcmp(speed_str, previous_values[i]) != 0) {
					strcpy(previous_values[i], speed_str);
					speed_update_t *s_upd = malloc(sizeof(speed_update_t));
					if (s_upd) {
						strcpy(s_upd->speed_str, speed_str);
						lv_async_call(update_speed_ui, s_upd);
					}
				}
			}
			// Handle Gear
			else if (value_id == GEAR_VALUE_ID) {
				// Skip CAN gear processing if in Speed/RPM Ratio mode
				if (values_config[GEAR_VALUE_ID - 1].gear_detection_mode == 4) {
					continue;
				}
				
				// Update CAN received timestamp for gear
				last_gear_can_received = current_time;

				// Get raw value BEFORE scaling/offset for icon matching
				// This is the actual CAN bus value that should match custom_icon_values
				int64_t gear_raw_value = extract_bits(
					message->data, values_config[GEAR_VALUE_ID - 1].bit_start,
					values_config[GEAR_VALUE_ID - 1].bit_length, 
					values_config[GEAR_VALUE_ID - 1].endianess,
					values_config[GEAR_VALUE_ID - 1].is_signed);

				char gear_str[EXAMPLE_MAX_CHAR_SIZE];

				// Check if label is "GEAR" (case insensitive)
				if (strcasecmp(label_texts[GEAR_VALUE_ID - 1], "GEAR") == 0) {
					// Format gear value based on detection mode
					uint8_t gear_mode =
						values_config[GEAR_VALUE_ID - 1].gear_detection_mode;

					if (gear_mode == 1) {
						// MaxxECU gear detection: -3=Park, -1=Reverse, 0=Neutral, >0=Forward gears
						if (final_value == 0) {
							snprintf(gear_str, sizeof(gear_str),
									 "N"); // Neutral
						} else if (final_value == -1) {
							snprintf(gear_str, sizeof(gear_str),
									 "R"); // Reverse
						} else if (final_value >= 1 && final_value <= 10) {
							snprintf(gear_str, sizeof(gear_str), "%d",
									 (int)final_value);
						} else {
							snprintf(gear_str, sizeof(gear_str),
									 "-"); // Invalid gear
						}
					} else if (gear_mode == 2) {
						// Haltech gear detection (different encoding)
						if (final_value == 0) {
							snprintf(gear_str, sizeof(gear_str),
									 "N"); // Neutral
						} else if (final_value == 255 || final_value == 0xFE) {
							snprintf(gear_str, sizeof(gear_str),
									 "R"); // Reverse
						} else if (final_value >= 1 && final_value <= 8) {
							snprintf(gear_str, sizeof(gear_str), "%d",
									 (int)final_value);
						} else {
							snprintf(gear_str, sizeof(gear_str),
									 "-"); // Invalid gear
						}
					} else {
						// Custom gear detection (mode 0)
						// Check if received value matches any configured gear
						// value
						// Use raw value (before scaling/offset) for matching
						bool gear_found = false;

						// Check each custom gear value
						// UINT32_MAX means "not configured", so skip those
						// 0 is now a valid configured value
						for (int gear_idx = 0; gear_idx < 14;
							 gear_idx++) { // 0-13: P, R, N, D, 1-10
							uint32_t configured_value = values_config[GEAR_VALUE_ID - 1].gear_custom_values[gear_idx];
							// Skip if not configured (UINT32_MAX)
							if (configured_value == UINT32_MAX) {
								continue;
							}
							if (configured_value == (uint32_t)gear_raw_value) {
								if (gear_idx == 0) {
									snprintf(gear_str, sizeof(gear_str),
											 "P"); // Park
								} else if (gear_idx == 1) {
									snprintf(gear_str, sizeof(gear_str),
											 "R"); // Reverse
								} else if (gear_idx == 2) {
									snprintf(gear_str, sizeof(gear_str),
											 "N"); // Neutral
								} else if (gear_idx == 3) {
									snprintf(gear_str, sizeof(gear_str),
											 "D"); // Drive
								} else {
									snprintf(gear_str, sizeof(gear_str), "%d",
											 gear_idx - 3); // Gears 1-10
								}
								gear_found = true;
								break;
							}
						}

						if (!gear_found) {
							snprintf(gear_str, sizeof(gear_str),
									 "-"); // Show dash if no gear value matches
						}
					}
				} else {
					// Use normal numeric formatting if label isn't "GEAR"
					snprintf(gear_str, sizeof(gear_str), "%.0f", final_value);
				}

				// Always update if gear string changed OR if raw value changed (for icon matching)
				// This ensures icon appears even if gear_str is "-" but icon value matches
				bool gear_str_changed = (strcmp(gear_str, previous_values[GEAR_VALUE_ID - 1]) != 0);
				static uint32_t last_raw_value_slow = 0;
				bool raw_value_changed = ((uint32_t)gear_raw_value != last_raw_value_slow);
				
				if (gear_str_changed || raw_value_changed) {
					if (gear_str_changed) {
						strcpy(previous_values[GEAR_VALUE_ID - 1], gear_str);
					}
					last_raw_value_slow = (uint32_t)gear_raw_value;
					gear_update_t *g_upd = malloc(sizeof(gear_update_t));
					if (g_upd) {
						strcpy(g_upd->gear_str, gear_str);
						// Use raw value (before scaling/offset) for icon matching
						g_upd->raw_value = (uint32_t)gear_raw_value;
						lv_async_call(update_gear_ui, g_upd);
					}
				}
			}
		}
	}
}

////////////////////////	STYLES
////////////////////////////////////////////////
static lv_style_t box_style;
lv_style_t common_style;

void apply_common_roller_styles(lv_obj_t *roller) {
	// Set text color for dall items
	lv_obj_set_style_text_color(roller, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_text_color(roller, lv_color_black(), LV_PART_SELECTED);
	lv_obj_set_style_bg_color(roller, lv_color_white(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(roller, 0, LV_PART_SELECTED);
	lv_obj_set_style_radius(roller, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_left(roller, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(roller, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_top(roller, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(roller, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
}

// Initialize styles
static void init_styles(void) {
	// Box Style
	lv_style_init(&box_style);
	lv_style_set_radius(&box_style, 7);
	lv_style_set_bg_color(&box_style,
						  THEME_COLOR_BG); // Black background
	lv_style_set_bg_opa(&box_style, 255); // Full opacity for black background
	lv_style_set_clip_corner(&box_style, false);
	lv_style_set_border_color(&box_style, THEME_COLOR_PANEL);
	lv_style_set_border_opa(&box_style, 255);
	lv_style_set_border_width(&box_style, 3);
	lv_style_set_border_post(&box_style, true); // Ensure border is drawn on top
	lv_style_set_outline_width(&box_style, 0);	// Remove black outline
	lv_style_set_outline_pad(&box_style, 0);
}

void init_common_style(void) {
	lv_style_init(&common_style);
	lv_style_set_radius(&common_style, 7);
	lv_style_set_pad_all(&common_style, 8); // 7px padding on all sides
	lv_style_set_bg_color(&common_style,
						  THEME_COLOR_TEXT_PRIMARY); // White background
	lv_style_set_bg_opa(&common_style, LV_OPA_COVER);
	lv_style_set_border_color(&common_style,
							  THEME_COLOR_TEXT_MUTED); // Light gray border
	lv_style_set_border_width(&common_style, 1);
	lv_style_set_text_color(&common_style, lv_color_black()); // Black text
	lv_style_set_text_font(&common_style,
						   THEME_FONT_SMALL); // Common font
}

// Getter function for common_style to allow access from other files
lv_style_t *get_common_style(void) { return &common_style; }

lv_style_t *get_box_style(void) { return &box_style; }

// RPM border style
static lv_obj_t *create_panel(lv_obj_t *parent, int width, int height, int x,
							  int y, int radius, lv_color_t bg_color,
							  int transform_angle) {
	lv_obj_t *panel = lv_obj_create(parent);
	lv_obj_set_size(panel, width, height);
	lv_obj_set_pos(panel, x, y);
	lv_obj_set_align(panel, LV_ALIGN_CENTER);
	lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(panel, radius, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(panel, bg_color, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(panel, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	if (transform_angle != 0) {
		lv_obj_set_style_transform_angle(panel, transform_angle,
										 LV_PART_MAIN | LV_STATE_DEFAULT);
	}
	return panel;
}
/////////////////////////////////////////////	ITEM CREATION
////////////////////////////////////////////////

void create_rpm_bar_gauge(lv_obj_t *parent_screen) {
	ui_RPM_Base_1 = create_panel(parent_screen, 800, 6, 0, -182, 0,
								 THEME_COLOR_PANEL, 0); // Moved up 2px
	ui_RPM_Base_2 = create_panel(parent_screen, 49, 22, -41, -193, 7,
								 THEME_COLOR_PANEL, 550);
	ui_RPM_Base_3 = create_panel(parent_screen, 49, 22, 105, -181, 7,
								 THEME_COLOR_PANEL, 1250);
	ui_RPM_Base_4 =
		create_panel(parent_screen, 111, 44, 0, -176, 7, THEME_COLOR_PANEL,
					 0); // Back to original position
	lv_color_t saved_color = values_config[RPM_VALUE_ID - 1].rpm_bar_color;
	ui_Panel9 = create_panel(parent_screen, 55, 55, -373, -213, 0, saved_color,
							 0); // Moved up 2px, left 1px

	// Calculate extended RPM max for rightward extension to screen edge
	// Original RPM bar: 765px centered (left edge at -382.5px, right edge at
	// +382.5px) New RPM bar: extends from -382.5px to +400px (screen edge) =
	// 782.5px total Keep left edge at -382.5px, extend only rightward
	const float bar_extension_ratio = 782.5f / 765.0f;
	int32_t extended_rpm_max = (int32_t)(rpm_gauge_max * bar_extension_ratio);

	// Create the RPM bar gauge with extended range and rightward extension
	rpm_bar_gauge = lv_bar_create(parent_screen);
	lv_bar_set_range(rpm_bar_gauge, 0, extended_rpm_max);
	lv_bar_set_value(rpm_bar_gauge, 0, LV_ANIM_OFF);
	lv_obj_set_size(rpm_bar_gauge, 783, 55); // 782.5px rounded up to 783px

	// Position bar so left edge stays at -382.5px and extends to +400px (screen
	// edge) Left edge needs to be at -382.5px, so center should be at: -382.5 +
	// (783/2) = 8.75px
	lv_obj_align(rpm_bar_gauge, LV_ALIGN_TOP_MID, 8,
				 0); // Adjusted to fill remaining space (1px left, 2px up)

	// Set styles for the RPM bar gauge (no gradient, solid color)
	lv_obj_set_style_radius(rpm_bar_gauge, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(rpm_bar_gauge, THEME_COLOR_RPM_BAR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT); // Light gray
	lv_obj_set_style_bg_opa(rpm_bar_gauge, 255,
							LV_PART_MAIN | LV_STATE_DEFAULT);

	// Use the saved color for the RPM bar indicator
	lv_obj_set_style_radius(rpm_bar_gauge, 0,
							LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(rpm_bar_gauge, saved_color,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(rpm_bar_gauge, 255,
							LV_PART_INDICATOR | LV_STATE_DEFAULT);

	// Set gradient color to same as main color for solid appearance
	lv_obj_set_style_bg_grad_color(rpm_bar_gauge, saved_color,
								   LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_grad_dir(rpm_bar_gauge, LV_GRAD_DIR_NONE,
								 LV_PART_INDICATOR | LV_STATE_DEFAULT);

	// Create redline zone rectangle (above RPM bar, below numbers/lines)
	rpm_redline_zone = lv_obj_create(parent_screen);
	lv_obj_set_height(rpm_redline_zone,
					  12);				  // Same height as the taller RPM lines
	lv_obj_set_y(rpm_redline_zone, -191); // Moved up 2px
	lv_obj_set_align(rpm_redline_zone, LV_ALIGN_CENTER);
	lv_obj_clear_flag(rpm_redline_zone, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(rpm_redline_zone, 0,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(rpm_redline_zone, THEME_COLOR_RED,
							  LV_PART_MAIN | LV_STATE_DEFAULT); // Bright red
	lv_obj_set_style_bg_opa(rpm_redline_zone, 180,
							LV_PART_MAIN |
								LV_STATE_DEFAULT); // Semi-transparent
	lv_obj_set_style_border_width(rpm_redline_zone, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	// Initial position and width will be set by update_redline_position()

	// Create large transparent click zone covering the entire extended RPM bar
	// gauge
	lv_obj_t *rpm_click_zone = lv_obj_create(parent_screen);
	lv_obj_set_size(rpm_click_zone, 783, 55); // Match extended RPM bar size
	lv_obj_align(rpm_click_zone, LV_ALIGN_TOP_MID, 9,
				 2); // Match extended RPM bar position
	lv_obj_clear_flag(rpm_click_zone, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_opa(rpm_click_zone, 0,
							LV_PART_MAIN |
								LV_STATE_DEFAULT); // Completely transparent
	lv_obj_set_style_border_opa(rpm_click_zone, 0,
								LV_PART_MAIN | LV_STATE_DEFAULT); // No border
	lv_obj_add_flag(rpm_click_zone, LV_OBJ_FLAG_CLICKABLE);

	// Allocate memory to store RPM value_id and pass it to the event callback
	uint8_t *rpm_id_ptr = lv_mem_alloc(sizeof(uint8_t));
	*rpm_id_ptr = RPM_VALUE_ID;
	lv_obj_add_event_cb(rpm_click_zone, value_long_press_event_cb,
						LV_EVENT_LONG_PRESSED, rpm_id_ptr);
	lv_obj_add_event_cb(rpm_click_zone, free_value_id_event_cb, LV_EVENT_DELETE,
						rpm_id_ptr);
}

int num_rpm_lines = 0;
lv_obj_t *rpm_labels[MAX_RPM_LINES];	// Only need labels for the first set
lv_obj_t *rpm_lines[MAX_RPM_LINES * 2]; // Two sets of lines

void update_rpm_lines(lv_obj_t *parent) {
	// Delete existing lines and labels
	for (int i = 0; i < num_rpm_lines; i++) {
		if (rpm_lines[i] != NULL) {
			lv_obj_del(rpm_lines[i]);
			rpm_lines[i] = NULL;
		}
		if (i < MAX_RPM_LINES && rpm_labels[i] != NULL) {
			lv_obj_del(rpm_labels[i]);
			rpm_labels[i] = NULL;
		}
	}
	num_rpm_lines = 0;

	// Step in increments of 500 RPM for medium and main ticks
	int increments = 500;
	int num_lines = (rpm_gauge_max / increments) + 1; // Include 0 RPM

	// Ensure we don't exceed MAX_RPM_LINES per set
	if (num_lines > MAX_RPM_LINES) {
		num_lines = MAX_RPM_LINES;
	}

	// Get the position and size of the RPM bar
	lv_coord_t bar_x = 18;
	lv_coord_t bar_y_set1 = 0;	// First set starts at px (moved up 2px)
	lv_coord_t bar_y_set2 = 42; // Second set starts at px (moved up 2px)

	// For each tick, calculate its position for both sets
	for (int i = 0; i < num_lines; i++) {
		// Current RPM value for the tick
		int rpm_value = i * increments;

		// Calculate the x position based on rpm_value
		lv_coord_t x_pos = bar_x + ((rpm_value * 765) / rpm_gauge_max);

		// Decide which size line/tick to draw
		//    - Every 1000 RPM: main tick (width=3, height=13)
		//    - Every 500 RPM: medium tick (width=2, height=11)
		lv_coord_t line_width;
		lv_coord_t line_height;
		bool add_label = false; // Only label the 1000s in the first set

		if ((rpm_value % 1000) == 0) {
			// Main tick
			line_width = 3;
			line_height = 12;
			add_label = true;
		} else {
			// Medium tick (500 RPM)
			line_width = 2;
			line_height = 8;
		}

		// Create the first set of lines (top row)
		lv_obj_t *line_top = lv_obj_create(parent);
		lv_obj_set_size(line_top, line_width, line_height);
		lv_obj_set_style_radius(line_top, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_color(line_top, THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(line_top, LV_OPA_COVER,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(line_top, 0,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_pad_all(line_top, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_clear_flag(line_top, LV_OBJ_FLAG_SCROLLABLE);

		// Position the line so it's centered horizontally on x_pos
		lv_coord_t adjusted_x = x_pos - (line_width / 2);
		lv_obj_set_pos(line_top, adjusted_x, bar_y_set1);

		rpm_lines[num_rpm_lines] = line_top;

		// Add a label for 1000 RPM ticks in the first set
		if (add_label) {
			lv_obj_t *label = lv_label_create(parent);

			// Display the "thousands" place (e.g., "7" for 7000)
			char rpm_str[8];
			snprintf(rpm_str, sizeof(rpm_str), "%d", rpm_value / 1000);
			lv_label_set_text(label, rpm_str);

			// Style the label
			lv_obj_set_style_text_color(label, THEME_COLOR_BG,
										LV_PART_MAIN);
			lv_obj_set_style_text_opa(label, LV_OPA_COVER, LV_PART_MAIN);
			lv_obj_set_style_text_font(label, THEME_FONT_DASH_TICK,
									   LV_PART_MAIN | LV_STATE_DEFAULT);

			// Position the label below the line
			lv_obj_align_to(label, line_top, LV_ALIGN_OUT_BOTTOM_MID, 0, 7);

			rpm_labels[num_rpm_lines] = label;
		}

		num_rpm_lines++;

		// Create the second set of lines (bottom row, flipped height)
		lv_obj_t *line_bottom = lv_obj_create(parent);
		lv_obj_set_size(line_bottom, line_width, line_height);
		lv_obj_set_style_radius(line_bottom, 0,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_color(line_bottom, THEME_COLOR_BG,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(line_bottom, LV_OPA_COVER,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(line_bottom, 0,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_pad_all(line_bottom, 0,
								 LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_clear_flag(line_bottom, LV_OBJ_FLAG_SCROLLABLE);

		// Position the bottom line flat at the bottom with its height flipped
		lv_obj_set_pos(line_bottom, adjusted_x,
					   bar_y_set2 + (13 - line_height));

		rpm_lines[num_rpm_lines] = line_bottom;

		num_rpm_lines++;

		// Stop if we exceed the maximum number of lines
		if (num_rpm_lines >= MAX_RPM_LINES * 2) {
			break;
		}
	}
}

// Create a transparent click zone and associate with value_id
static void create_transparent_click_zone(lv_obj_t *parent,
										  lv_obj_t *target_label,
										  uint8_t value_id) {
	lv_obj_t *click_zone = lv_obj_create(parent);

	// Check if this is a bar (BAR1_VALUE_ID=12 or BAR2_VALUE_ID=13) and adjust
	// size accordingly
	if (value_id == BAR1_VALUE_ID || value_id == BAR2_VALUE_ID) {
		// For bars, create a click zone that covers the full bar width and
		// height
		lv_obj_set_size(click_zone, 300, 30); // Match the bar dimensions
	} else {
		// For other elements, use the standard 60x60 size
		lv_obj_set_size(click_zone, 60, 60);
	}

	lv_obj_align_to(click_zone, target_label, LV_ALIGN_CENTER, 0, 0);
	lv_obj_clear_flag(click_zone, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_opa(click_zone, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_opa(click_zone, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_add_flag(click_zone, LV_OBJ_FLAG_CLICKABLE);

	// Add touch events for quick tap detection (to show menu button)
	lv_obj_add_event_cb(click_zone, screen3_touch_event_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(click_zone, screen3_touch_event_cb, LV_EVENT_RELEASED, NULL);

	// Allocate memory to store value_id and pass it to the event callback
	uint8_t *id_ptr = lv_mem_alloc(sizeof(uint8_t));
	*id_ptr = value_id;
	lv_obj_add_event_cb(click_zone, value_long_press_event_cb,
						LV_EVENT_LONG_PRESSED, id_ptr);
	lv_obj_add_event_cb(click_zone, free_value_id_event_cb, LV_EVENT_DELETE,
						id_ptr);
}

void create_warning_config_menu(uint8_t warning_idx) {
	init_common_style();

	// Allocate memory for input objects array - increase size to 8 to include
	// range inputs
	lv_obj_t **input_objects = lv_mem_alloc(8 * sizeof(lv_obj_t *));
	if (!input_objects) {
		printf("Failed to allocate memory for input objects\n");
		return;
	}
	// Initialize all pointers to NULL
	for (int i = 0; i < 8; i++) {
		input_objects[i] = NULL;
	}

	// Allocate memory for preview objects
	lv_obj_t **preview_objects = lv_mem_alloc(2 * sizeof(lv_obj_t *));
	if (!preview_objects) {
		lv_mem_free(input_objects);
		printf("Failed to allocate memory for preview objects\n");
		return;
	}
	preview_objects[0] = NULL;
	preview_objects[1] = NULL;

	// Create the configuration screen
	lv_obj_t *config_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(config_screen, THEME_COLOR_BG, 0);
	lv_obj_set_style_bg_opa(config_screen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(config_screen, LV_OBJ_FLAG_SCROLLABLE);

	// Create save data structure
	warning_save_data_t *save_data = lv_mem_alloc(sizeof(warning_save_data_t));
	if (!save_data) {
		lv_mem_free(input_objects);
		lv_mem_free(preview_objects);
		printf("Failed to allocate memory for save data\n");
		return;
	}
	save_data->warning_idx = warning_idx;
	save_data->input_objects = input_objects;
	save_data->preview_objects = preview_objects;
	save_data->preconfig_warning_dd = NULL; // Will be set when dropdown is created

	// Create main border/background panel - increased height for range inputs
	lv_obj_t *main_border = lv_obj_create(config_screen);
	lv_obj_set_width(main_border, 780);
	lv_obj_set_height(main_border, 405); // Increased from 325 to 405
	lv_obj_set_align(main_border, LV_ALIGN_CENTER);
	lv_obj_set_y(main_border, 117); // Lowered by 50px (was 67)
	lv_obj_clear_flag(main_border, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(main_border, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(main_border, THEME_COLOR_INACTIVE,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(main_border, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(main_border, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_t *input_border = lv_obj_create(config_screen);
	lv_obj_set_width(input_border, 275);
	lv_obj_set_height(input_border, 390); // Increased from 310 to 390
	lv_obj_set_x(input_border, -244);
	lv_obj_set_y(input_border, 117); // Lowered by 50px (was 67)
	lv_obj_set_align(input_border, LV_ALIGN_CENTER);
	lv_obj_clear_flag(input_border, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(input_border, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(input_border, THEME_COLOR_INPUT_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(input_border, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(input_border, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Create preview warning circle in exact Screen3 position
	lv_obj_t *preview_circle = lv_obj_create(config_screen);
	lv_obj_set_width(preview_circle, 15);
	lv_obj_set_height(preview_circle, 15);
	lv_obj_set_x(preview_circle, warning_positions[warning_idx].x);
	lv_obj_set_y(preview_circle, warning_positions[warning_idx].y);
	lv_obj_set_align(preview_circle, LV_ALIGN_CENTER);
	lv_obj_clear_flag(preview_circle, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(preview_circle, 100,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(preview_circle,
							  warning_configs[warning_idx].active_color,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(preview_circle, 255,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(preview_circle, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	preview_objects[0] =
		preview_circle; // Store preview circle for color updates

	// Create preview warning label in exact Screen3 position
	lv_obj_t *preview_label = lv_label_create(config_screen);
	lv_obj_set_width(preview_label,
					 LV_SIZE_CONTENT); // Auto width based on content
	lv_obj_set_height(preview_label, LV_SIZE_CONTENT); // Auto height
	lv_obj_set_x(preview_label, warning_positions[warning_idx].x);
	lv_obj_set_y(preview_label, -112); // Same y-position as in Screen3
	lv_obj_set_align(preview_label, LV_ALIGN_CENTER);
	lv_label_set_text(preview_label, warning_configs[warning_idx].label);
	lv_obj_set_style_text_color(preview_label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(preview_label, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(preview_label, LV_TEXT_ALIGN_CENTER,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(preview_label, THEME_FONT_TINY,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	preview_objects[1] = preview_label;

	// Create the keyboard
	keyboard = lv_keyboard_create(config_screen);
	lv_obj_set_parent(keyboard, lv_layer_top());
	lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(keyboard, keyboard_ready_event_cb, LV_EVENT_READY,
						NULL);

	// Create title
	lv_obj_t *title = lv_label_create(config_screen);
	lv_label_set_text_fmt(title, "Warning %d Configuration", warning_idx + 1);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
	lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

	// Create a container for inputs
	lv_obj_t *inputs_container = lv_obj_create(config_screen);
	lv_obj_set_size(inputs_container, 800,
					480); // Adjusted size to fit within the border
	lv_obj_align(inputs_container, LV_ALIGN_CENTER, 0, 0);
	lv_obj_set_style_bg_opa(inputs_container, 0, 0);
	lv_obj_set_style_border_opa(inputs_container, 0, 0);
	lv_obj_clear_flag(inputs_container, LV_OBJ_FLAG_SCROLLABLE);

	// Warning label input (moved to top)
	lv_obj_t *label_text_label = lv_label_create(inputs_container);
	lv_label_set_text(label_text_label, "Warning Label:");
	lv_obj_set_width(label_text_label, 110);
	lv_obj_set_style_text_align(label_text_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(label_text_label, LV_ALIGN_CENTER, -312,
				 -47); // Was 73, now -47
	lv_obj_set_style_text_color(label_text_label, THEME_COLOR_TEXT_MUTED, 0);

	input_objects[3] = lv_textarea_create(inputs_container);
	lv_obj_add_style(input_objects[3], &common_style, LV_PART_MAIN);
	lv_textarea_set_one_line(input_objects[3], true);
	lv_obj_set_width(input_objects[3], 120);
	lv_obj_align(input_objects[3], LV_ALIGN_CENTER, -180,
				 -47); // Was 73, now -47
	lv_obj_add_event_cb(input_objects[3], keyboard_event_cb, LV_EVENT_ALL,
						NULL);
	lv_textarea_set_text(input_objects[3], warning_configs[warning_idx].label);

	// CAN ID input (moved down)
	lv_obj_t *can_id_label = lv_label_create(inputs_container);
	lv_label_set_text(can_id_label, "CAN ID:");
	lv_obj_set_width(can_id_label, 110);
	lv_obj_set_style_text_align(can_id_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(can_id_label, LV_ALIGN_CENTER, -312, -7); // Was -47, now -7
	lv_obj_set_style_text_color(can_id_label, THEME_COLOR_TEXT_MUTED, 0);

	input_objects[0] = lv_textarea_create(inputs_container);
	lv_obj_add_style(input_objects[0], &common_style, LV_PART_MAIN);
	lv_textarea_set_one_line(input_objects[0], true);
	lv_obj_set_width(input_objects[0], 120);
	lv_obj_align(input_objects[0], LV_ALIGN_CENTER, -180,
				 -7); // Was -47, now -7
	lv_obj_add_event_cb(input_objects[0], keyboard_event_cb, LV_EVENT_ALL,
						NULL);
	char can_id_text[32];
	snprintf(can_id_text, sizeof(can_id_text), "%X",
			 warning_configs[warning_idx].can_id);
	lv_textarea_set_text(input_objects[0], can_id_text);

	// Bit position dropdown
	lv_obj_t *bit_pos_label = lv_label_create(inputs_container);
	lv_label_set_text(bit_pos_label, "Bit Position:");
	lv_obj_set_width(bit_pos_label, 110);
	lv_obj_set_style_text_align(bit_pos_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(bit_pos_label, LV_ALIGN_CENTER, -312, 33); // Was -7, now 33
	lv_obj_set_style_text_color(bit_pos_label, THEME_COLOR_TEXT_MUTED, 0);

	input_objects[1] = lv_dropdown_create(inputs_container);
	lv_obj_add_style(input_objects[1], &common_style, LV_PART_MAIN);
	lv_dropdown_set_options(
		input_objects[1],
		"0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n"
		"16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n"
		"32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n"
		"48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60\n61\n62\n63");
	lv_obj_set_width(input_objects[1], 120);
	lv_obj_align(input_objects[1], LV_ALIGN_CENTER, -180, 33);
	lv_dropdown_set_selected(input_objects[1],
							 warning_configs[warning_idx].bit_position);

	// Highlighted color dropdown
	lv_obj_t *color_label = lv_label_create(inputs_container);
	lv_label_set_text(color_label, "Active Colour:");
	lv_obj_set_width(color_label, 110);
	lv_obj_set_style_text_align(color_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(color_label, LV_ALIGN_CENTER, -312, 73);
	lv_obj_set_style_text_color(color_label, THEME_COLOR_TEXT_MUTED, 0);

	input_objects[4] = lv_dropdown_create(inputs_container);
	lv_obj_add_style(input_objects[4], &common_style, LV_PART_MAIN);
	lv_dropdown_set_options(input_objects[4],
							"Green\nBlue\nOrange\nRed\nYellow");
	lv_obj_set_width(input_objects[4], 120);
	lv_obj_align(input_objects[4], LV_ALIGN_CENTER, -180, 73);

	// Set the current color selection based on the saved configuration
	lv_color_t current_color = warning_configs[warning_idx].active_color;
	uint8_t selected_color = 0; // Default to Green
	if (current_color.full == THEME_COLOR_BLUE_PURE.full)
		selected_color = 1; // Blue
	else if (current_color.full == THEME_COLOR_ORANGE_WEB.full)
		selected_color = 2; // Orange
	else if (current_color.full == THEME_COLOR_RED.full)
		selected_color = 3; // Red
	else if (current_color.full == THEME_COLOR_YELLOW.full)
		selected_color = 4; // Yellow
	lv_dropdown_set_selected(input_objects[4], selected_color);

	// Add event callback to color dropdown to update preview instantly
	lv_obj_add_event_cb(input_objects[4], color_dropdown_cb,
						LV_EVENT_VALUE_CHANGED, save_data);

	// Add Toggle Mode dropdown
	lv_obj_t *toggle_mode_label = lv_label_create(inputs_container);
	lv_label_set_text(toggle_mode_label, "Toggle Mode:");
	lv_obj_set_width(toggle_mode_label, 110);
	lv_obj_set_style_text_align(toggle_mode_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_style_text_color(toggle_mode_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_align(toggle_mode_label, LV_ALIGN_CENTER, -312, 113);

	input_objects[5] = lv_dropdown_create(inputs_container);
	lv_obj_add_style(input_objects[5], &common_style, LV_PART_MAIN);
	lv_dropdown_set_options(input_objects[5], "On/Off\nMomentary");
	lv_obj_set_width(input_objects[5], 120);
	lv_obj_align(input_objects[5], LV_ALIGN_CENTER, -180, 113);
	lv_dropdown_set_selected(input_objects[5],
							 warning_configs[warning_idx].is_momentary ? 1 : 0);

	// Invert Toggle (below Toggle Mode on the left)
	lv_obj_t *invert_toggle_label = lv_label_create(inputs_container);
	lv_label_set_text(invert_toggle_label, "Invert Toggle:");
	lv_obj_set_width(invert_toggle_label, 110);
	lv_obj_set_style_text_align(invert_toggle_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_set_style_text_color(invert_toggle_label, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_align(invert_toggle_label, LV_ALIGN_CENTER, -312, 153);
	
	lv_obj_t *invert_toggle_switch = lv_switch_create(inputs_container);
	lv_obj_align(invert_toggle_switch, LV_ALIGN_CENTER, -180, 153);
	lv_obj_set_size(invert_toggle_switch, 50, 25);
	
	// Set switch state based on configuration
	if (warning_configs[warning_idx].invert_toggle) {
		lv_obj_add_state(invert_toggle_switch, LV_STATE_CHECKED);
	} else {
		lv_obj_clear_state(invert_toggle_switch, LV_STATE_CHECKED);
	}
	
	// Add event callback for invert toggle
	uint8_t *invert_toggle_id_ptr = lv_mem_alloc(sizeof(uint8_t));
	*invert_toggle_id_ptr = warning_idx;
	lv_obj_add_event_cb(invert_toggle_switch, invert_warning_toggle_event_cb, LV_EVENT_VALUE_CHANGED, invert_toggle_id_ptr);
	lv_obj_add_event_cb(invert_toggle_switch, free_warning_idx_event_cb, LV_EVENT_DELETE, invert_toggle_id_ptr);

	// Right column - Preconfigured Warnings (in grey container)
	// Create grey container for preconfig section
	lv_obj_t *preconfig_container = lv_obj_create(config_screen);
	lv_obj_set_width(preconfig_container, 240);
	lv_obj_set_height(preconfig_container, 220);
	lv_obj_align(preconfig_container, LV_ALIGN_CENTER, 250, 40);
	lv_obj_clear_flag(preconfig_container, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_radius(preconfig_container, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(preconfig_container, THEME_COLOR_INPUT_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(preconfig_container, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(preconfig_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_all(preconfig_container, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
	
	// Heading for preconfig container
	lv_obj_t *preconfig_heading = lv_label_create(preconfig_container);
	lv_label_set_text(preconfig_heading, "Pre-configurations");
	lv_obj_set_style_text_color(preconfig_heading, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(preconfig_heading, THEME_FONT_BODY, 0);
	lv_obj_align(preconfig_heading, LV_ALIGN_TOP_MID, 0, 0);
	
	// ECU dropdown
	lv_obj_t *preconfig_ecu_label = lv_label_create(preconfig_container);
	lv_label_set_text(preconfig_ecu_label, "ECU:");
	lv_obj_set_width(preconfig_ecu_label, 70);
	lv_obj_set_style_text_align(preconfig_ecu_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(preconfig_ecu_label, LV_ALIGN_TOP_LEFT, 0, 30);
	lv_obj_set_style_text_color(preconfig_ecu_label, THEME_COLOR_TEXT_MUTED, 0);

	lv_obj_t *preconfig_ecu_dd = lv_dropdown_create(preconfig_container);
	lv_obj_add_style(preconfig_ecu_dd, &common_style, LV_PART_MAIN);
	lv_dropdown_set_options(preconfig_ecu_dd, "Ford");
	lv_obj_set_width(preconfig_ecu_dd, 140);
	lv_obj_align(preconfig_ecu_dd, LV_ALIGN_TOP_LEFT, 80, 25);

	// Version dropdown
	lv_obj_t *preconfig_version_label = lv_label_create(preconfig_container);
	lv_label_set_text(preconfig_version_label, "Version:");
	lv_obj_set_width(preconfig_version_label, 70);
	lv_obj_set_style_text_align(preconfig_version_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(preconfig_version_label, LV_ALIGN_TOP_LEFT, 0, 70);
	lv_obj_set_style_text_color(preconfig_version_label, THEME_COLOR_TEXT_MUTED,
								0);

	lv_obj_t *preconfig_version_dd = lv_dropdown_create(preconfig_container);
	lv_obj_add_style(preconfig_version_dd, &common_style, LV_PART_MAIN);
	lv_dropdown_set_options(preconfig_version_dd, "BA/BF/FG");
	lv_obj_set_width(preconfig_version_dd, 140);
	lv_obj_align(preconfig_version_dd, LV_ALIGN_TOP_LEFT, 80, 65);

	// Warnings dropdown
	lv_obj_t *preconfig_warning_label = lv_label_create(preconfig_container);
	lv_label_set_text(preconfig_warning_label, "Warning:");
	lv_obj_set_width(preconfig_warning_label, 70);
	lv_obj_set_style_text_align(preconfig_warning_label, LV_TEXT_ALIGN_LEFT, 0);
	lv_obj_align(preconfig_warning_label, LV_ALIGN_TOP_LEFT, 0, 110);
	lv_obj_set_style_text_color(preconfig_warning_label, THEME_COLOR_TEXT_MUTED,
								0);

	lv_obj_t *preconfig_warning_dd = lv_dropdown_create(preconfig_container);
	lv_obj_add_style(preconfig_warning_dd, &common_style, LV_PART_MAIN);
	lv_dropdown_set_options(preconfig_warning_dd, "Foglights On\n"
												  "High Beams On\n"
												  "Driver Door\n"
												  "Passenger Door\n"
												  "Rear Doors\n"
												  "Cruise On\n"
												  "Cruise Set\n"
												  "Low Oil Press\n"
												  "Alternator Fail\n"
												  "Engine Light\n"
												  "Handbrake On");
	lv_obj_set_width(preconfig_warning_dd, 140);
	lv_obj_align(preconfig_warning_dd, LV_ALIGN_TOP_LEFT, 80, 105);

	// Store reference to warning dropdown in save_data
	save_data->preconfig_warning_dd = preconfig_warning_dd;

	// Apply button for preconfig
	lv_obj_t *apply_preconfig_btn = lv_btn_create(preconfig_container);
	lv_obj_align(apply_preconfig_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
	lv_obj_set_width(apply_preconfig_btn, 140);
	lv_obj_set_height(apply_preconfig_btn, 30);

	lv_obj_t *apply_preconfig_label = lv_label_create(apply_preconfig_btn);
	lv_label_set_text(apply_preconfig_label, "Apply");
	lv_obj_center(apply_preconfig_label);

	// Add event callback to apply button
	lv_obj_add_event_cb(apply_preconfig_btn, apply_preconfig_warning_cb,
						LV_EVENT_CLICKED, save_data);

	// Save button
	lv_obj_t *save_btn = lv_btn_create(config_screen);
	lv_obj_t *save_label = lv_label_create(save_btn);
	lv_label_set_text(save_label, "Save");
	lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -20, -20);

	lv_obj_add_event_cb(save_btn, save_warning_config_cb, LV_EVENT_CLICKED,
						save_data);

	lv_scr_load(config_screen);
}

// Free warning index event callback
static void free_warning_idx_event_cb(lv_event_t * e) {
	if (lv_event_get_code(e) == LV_EVENT_DELETE) {
		uint8_t * p_idx = (uint8_t *)lv_event_get_user_data(e);
		if (p_idx) {
			lv_mem_free(p_idx);
		}
	}
}

// Invert warning toggle event callback
static void invert_warning_toggle_event_cb(lv_event_t * e) {
	lv_obj_t * switch_obj = lv_event_get_target(e);
	uint8_t *warning_idx_ptr = (uint8_t *)lv_event_get_user_data(e);
	if (!warning_idx_ptr) return;
	
	uint8_t warning_idx = *warning_idx_ptr;
	bool new_invert_toggle = lv_obj_has_state(switch_obj, LV_STATE_CHECKED);
	bool old_invert_toggle = warning_configs[warning_idx].invert_toggle;
	
	// If the invert state changed, we need to flip the current warning state
	// to reflect the inversion immediately (works both ways: on->off and off->on)
	if (new_invert_toggle != old_invert_toggle) {
		// Flip the current state to reflect the inversion change
		// This works both ways: enabling invert flips once, disabling flips back
		warning_configs[warning_idx].current_state = !warning_configs[warning_idx].current_state;
		// Also flip the previous bit state so toggle mode works correctly
		previous_bit_states[warning_idx] = !previous_bit_states[warning_idx];
		// Update the UI immediately
		update_warning_ui_immediate(warning_idx);
		
		ESP_LOGI("WARNING", "Invert toggle changed for warning %d: %s -> %s, state flipped to %s", 
			warning_idx, 
			old_invert_toggle ? "enabled" : "disabled",
			new_invert_toggle ? "enabled" : "disabled",
			warning_configs[warning_idx].current_state ? "ON" : "OFF");
	}
	
	warning_configs[warning_idx].invert_toggle = new_invert_toggle;
	
	// Save configuration to NVS
	save_warning_configs_to_nvs();
	
	ESP_LOGI("WARNING", "Invert toggle %s for warning %d", new_invert_toggle ? "enabled" : "disabled", warning_idx);
}

// Creation and initializing Boxes and Arcs
static void init_boxes_and_arcs(void) {
	for (uint8_t i = 0; i < 8; i++) {
		// Create Box
		ui_Box[i] = lv_obj_create(ui_Screen3);
		lv_obj_set_size(ui_Box[i], 155, 92);
		lv_obj_set_pos(ui_Box[i], box_positions[i][0], box_positions[i][1]);
		lv_obj_set_align(ui_Box[i], LV_ALIGN_CENTER);
		lv_obj_clear_flag(ui_Box[i], LV_OBJ_FLAG_SCROLLABLE);
		// Enable content clipping so children don't overflow
		lv_obj_set_style_clip_corner(ui_Box[i], true, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_add_style(ui_Box[i], &box_style,
						 LV_PART_MAIN | LV_STATE_DEFAULT);
		
		// Add touch events to boxes for quick tap detection (to show menu button)
		lv_obj_add_event_cb(ui_Box[i], screen3_touch_event_cb, LV_EVENT_PRESSED, NULL);
		lv_obj_add_event_cb(ui_Box[i], screen3_touch_event_cb, LV_EVENT_RELEASED, NULL);
	}
}

// Menu button hide timer callback
static void menu_button_hide_timer_cb(lv_timer_t *timer) {
	if (ui_Menu_Button && lv_obj_is_valid(ui_Menu_Button)) {
		lv_obj_add_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN);
	}
	if (menu_button_hide_timer) {
		lv_timer_del(menu_button_hide_timer);
		menu_button_hide_timer = NULL;
	}
}

// Screen3 touch event callback - tracks press/release for quick tap detection
static void screen3_touch_event_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	
	if (code == LV_EVENT_PRESSED) {
		// Record the time when finger touches screen
		touch_press_time = lv_tick_get();
	}
	else if (code == LV_EVENT_RELEASED) {
		// Check how long the touch was held
		uint32_t touch_duration = lv_tick_get() - touch_press_time;
		
		// If touch was quick (less than 300ms), show menu button
		// This avoids showing menu during long presses for panel config
		if (touch_duration < 300) {
			// Show the menu button
			if (ui_Menu_Button && lv_obj_is_valid(ui_Menu_Button)) {
				lv_obj_clear_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN);
				
				// Delete existing timer if any
				if (menu_button_hide_timer) {
					lv_timer_del(menu_button_hide_timer);
				}
				
				// Create new timer to hide button after 6 seconds
				menu_button_hide_timer = lv_timer_create(menu_button_hide_timer_cb, 6000, NULL);
				lv_timer_set_repeat_count(menu_button_hide_timer, 1);
			}
		}
		
		touch_press_time = 0; // Reset
	}
}

// Setup menu close button callback
static void setup_menu_close_btn_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	
	if (code == LV_EVENT_CLICKED) {
		ESP_LOGI("MENU", "Close button clicked - returning to Screen3");
		
		// Store reference to setup menu screen
		lv_obj_t *screen_to_delete = ui_Setup_Menu_Screen;
		
		// First, load Screen3 (this must happen before deleting the current screen)
		if (ui_Screen3 && lv_obj_is_valid(ui_Screen3)) {
			lv_scr_load(ui_Screen3);
			
			// Then delete the setup menu screen after a small delay to allow screen transition
			if (screen_to_delete && lv_obj_is_valid(screen_to_delete)) {
				lv_obj_del_async(screen_to_delete);
				ui_Setup_Menu_Screen = NULL;
			}
		}
	}
}

// Menu button clicked callback - opens setup menu
static void menu_button_clicked_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	
	if (code == LV_EVENT_CLICKED) {
		ESP_LOGI("MENU", "Setup menu button clicked - opening setup menu");
		
		// Hide the menu button immediately
		if (ui_Menu_Button && lv_obj_is_valid(ui_Menu_Button)) {
			lv_obj_add_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN);
		}
		
		// Cancel hide timer if it exists
		if (menu_button_hide_timer) {
			lv_timer_del(menu_button_hide_timer);
			menu_button_hide_timer = NULL;
		}
		
		// Create setup menu screen
		ui_Setup_Menu_Screen = lv_obj_create(NULL);
		lv_obj_clear_flag(ui_Setup_Menu_Screen, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_bg_color(ui_Setup_Menu_Screen, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(ui_Setup_Menu_Screen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
		
		// Create close button at bottom center
		lv_obj_t *close_btn = lv_btn_create(ui_Setup_Menu_Screen);
		lv_obj_set_width(close_btn, 100);
		lv_obj_set_height(close_btn, 50);
		lv_obj_set_x(close_btn, 0);
		lv_obj_set_y(close_btn, -20); // 20px from bottom
		lv_obj_set_align(close_btn, LV_ALIGN_BOTTOM_MID);
		lv_obj_set_style_bg_color(close_btn, THEME_COLOR_BTN_CANCEL, LV_PART_MAIN | LV_STATE_DEFAULT); // Red
		lv_obj_set_style_bg_opa(close_btn, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_radius(close_btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
		
		// Add label to close button
		lv_obj_t *close_label = lv_label_create(close_btn);
		lv_label_set_text(close_label, "CLOSE");
		lv_obj_set_style_text_color(close_label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_center(close_label);
		
		// Add click event to close button
		lv_obj_add_event_cb(close_btn, setup_menu_close_btn_cb, LV_EVENT_CLICKED, NULL);
		
		// Load the setup menu screen
		lv_scr_load(ui_Setup_Menu_Screen);
		
		ESP_LOGI("MENU", "Setup menu screen created and loaded");
	}
}

/////////////////////////////////////////////	UI_SCREEN3__SCREEN_INIT
////////////////////////////////////////////////

void ui_Screen3_screen_init(void) {
	// Initialize styles
	init_styles();
	init_common_style();

	// Initialize configurations
	init_values_config_defaults();
	init_warning_configs();
	// Note: indicator configs already initialized and loaded in main.c
	load_values_config_from_nvs();
	load_warning_configs_from_nvs();
	
	// Update warning UI for warnings with invert enabled (they should show as active on boot)
	// This needs to be done after loading from NVS but before creating UI elements
	for (int i = 0; i < 8; i++) {
		if (warning_configs[i].invert_toggle && warning_configs[i].current_state) {
			// Warning with invert enabled is active - UI will be updated after circles are created
		}
	}

	// Debug: Verify indicator configs are initialized
	printf("Indicator configs initialized - Left: CAN=0x%X, Bit=%d, "
		   "Momentary=%d\n",
		   indicator_configs[0].can_id, indicator_configs[0].bit_position,
		   indicator_configs[0].is_momentary);
	printf("Indicator configs initialized - Right: CAN=0x%X, Bit=%d, "
		   "Momentary=%d\n",
		   indicator_configs[1].can_id, indicator_configs[1].bit_position,
		   indicator_configs[1].is_momentary);

	ui_Screen3 = lv_obj_create(NULL);
	lv_obj_clear_flag(ui_Screen3, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(ui_Screen3, THEME_COLOR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Screen3, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	
	// Add touch event to Screen3 to detect quick taps (press and release tracking)
	lv_obj_add_event_cb(ui_Screen3, screen3_touch_event_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(ui_Screen3, screen3_touch_event_cb, LV_EVENT_RELEASED, NULL);

	// Create timers only if they don't already exist to prevent duplicates
	static bool timers_created = false;
	if (!timers_created) {
		// Create timer for RPM color updates
		lv_timer_create(check_rpm_color_update, 500, NULL);

		// Create timer for warning timeouts
		lv_timer_create(check_warning_timeouts, 50, NULL);

		// Create timer for CAN data timeouts (check every 1 second)
		lv_timer_create(check_can_timeouts, 1000, NULL);

		// Create timer for Speed/RPM Ratio gear calculation (200ms for smooth gear detection)
		lv_timer_create(speed_rpm_gear_update_timer_cb, 200, NULL);

		// Create timer for indicator animation (350ms = realistic car indicator
		// timing)
		indicator_animation_timer =
			lv_timer_create(indicator_animation_timer_cb, 350, NULL);

		timers_created = true;
	}

	// Clear all object pointers before recreation to prevent race conditions
	for (int i = 0; i < 13; i++) {
		ui_Label[i] = NULL;
		ui_Value[i] = NULL;
		if (i < 8) {
			ui_Box[i] = NULL;
			ui_CustomText[i] = NULL;
		}
		// CRITICAL: Reset previous_values to force UI updates after screen
		// recreation This ensures the first CAN message updates the display
		// even if the value hasn't changed
		memset(previous_values[i], 0, sizeof(previous_values[i]));
	}
	ui_RPM_Value = NULL;
	ui_Speed_Value = NULL;
	ui_GEAR_Value = NULL;
	ui_GEAR_Icon = NULL;
	ui_Bar_1 = NULL;
	ui_Bar_2 = NULL;
	ui_Bar_1_Label = NULL;
	ui_Bar_1_Value = NULL;
	ui_Bar_2_Label = NULL;
	ui_Bar_2_Value = NULL;
	rpm_bar_gauge = NULL;

	// Set flag to reset CAN tracking variables on next CAN message
	reset_can_tracking = true;

	init_boxes_and_arcs();
	create_rpm_bar_gauge(ui_Screen3);
	update_rpm_lines(ui_Screen3);
	update_redline_position(); // Set initial redline position

	// Initialize labels, units, bars, and values
	for (uint8_t i = 0; i < 8; i++) {
		// Create label as child of box (so it gets clipped if it overflows)
		ui_Label[i] = lv_label_create(ui_Box[i]);
		lv_label_set_text(ui_Label[i], label_texts[i]);
		lv_obj_set_style_text_color(ui_Label[i], THEME_COLOR_TEXT_PRIMARY,
									LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_opa(ui_Label[i], 255,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_font(ui_Label[i], THEME_FONT_DASH_LABEL,
								   LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_align(ui_Label[i], LV_TEXT_ALIGN_CENTER, 0);
		// Set max width and clip mode to prevent overflow
		lv_obj_set_width(ui_Label[i], 145);
		lv_label_set_long_mode(ui_Label[i], LV_LABEL_LONG_CLIP);
		// Center horizontally, keep original vertical position
		lv_coord_t relative_y = label_positions[i][1] - box_positions[i][1];
		lv_obj_set_x(ui_Label[i], 0);
		lv_obj_set_y(ui_Label[i], relative_y);
		lv_obj_set_align(ui_Label[i], LV_ALIGN_CENTER);

		// Create value label
		ui_Value[i] = lv_label_create(ui_Screen3);
		// Set initial value to "---" to indicate no signal yet or not assigned
		lv_label_set_text(ui_Value[i], "---");
		strcpy(previous_values[i], "---");
		
		lv_obj_set_style_text_color(ui_Value[i], THEME_COLOR_TEXT_PRIMARY,
									LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_opa(ui_Value[i], 255,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_font(ui_Value[i], THEME_FONT_DASH_VALUE,
								   LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_align(ui_Value[i], LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(ui_Value[i], 140);
		lv_label_set_long_mode(ui_Value[i], LV_LABEL_LONG_CLIP);
		lv_obj_set_x(ui_Value[i], value_positions[i][0]);
		lv_obj_set_y(ui_Value[i], value_positions[i][1]);
		lv_obj_set_align(ui_Value[i], LV_ALIGN_CENTER);

		// Create transparent click zone
		create_transparent_click_zone(ui_Screen3, ui_Value[i], i + 1);

		// Create custom text label in bottom right corner of panel
		ui_CustomText[i] = lv_label_create(ui_Screen3);
		lv_label_set_text(ui_CustomText[i], values_config[i].custom_text);
		lv_obj_set_style_text_color(ui_CustomText[i], THEME_COLOR_TEXT_MUTED,
									LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_opa(ui_CustomText[i], 255,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_font(ui_CustomText[i], THEME_FONT_BODY,
								   LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_align(ui_CustomText[i], LV_TEXT_ALIGN_RIGHT,
									LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_width(ui_CustomText[i], 60); // Limit width to fit in panel
		lv_label_set_long_mode(ui_CustomText[i], LV_LABEL_LONG_CLIP);
		// Position in bottom right corner of panel (panel is 155x92, positioned
		// at box_positions)
		lv_obj_set_x(ui_CustomText[i],
					 box_positions[i][0] +
						 41); // Moved right by 5px (36 + 5 = 41)
		lv_obj_set_y(ui_CustomText[i], box_positions[i][1] + 32);
		lv_obj_set_align(ui_CustomText[i], LV_ALIGN_CENTER);

		// Hide custom text if it's empty
		if (strlen(values_config[i].custom_text) == 0) {
			lv_obj_add_flag(ui_CustomText[i], LV_OBJ_FLAG_HIDDEN);
		}
	}

	ui_RPM_Value = lv_label_create(ui_Screen3);
	lv_obj_set_width(ui_RPM_Value, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height(ui_RPM_Value, LV_SIZE_CONTENT); /// 1
	lv_obj_set_x(ui_RPM_Value, 0);
	lv_obj_set_y(ui_RPM_Value, -127);
	lv_obj_set_align(ui_RPM_Value, LV_ALIGN_CENTER);
	lv_label_set_text(ui_RPM_Value, "---");
	strcpy(previous_values[RPM_VALUE_ID - 1], "---");
	lv_obj_set_style_text_color(ui_RPM_Value, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_RPM_Value, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_RPM_Value, THEME_FONT_DASH_RPM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_RPM_Label = lv_label_create(ui_Screen3);
	lv_obj_set_width(ui_RPM_Label, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height(ui_RPM_Label, LV_SIZE_CONTENT); /// 1
	lv_obj_set_x(ui_RPM_Label, 0);
	lv_obj_set_y(ui_RPM_Label, -164);
	lv_obj_set_align(ui_RPM_Label, LV_ALIGN_CENTER);
	lv_label_set_text(ui_RPM_Label, "RPM");
	lv_obj_set_style_text_color(ui_RPM_Label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_RPM_Label, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_RPM_Label, THEME_FONT_DASH_LABEL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_Speed_Value = lv_label_create(ui_Screen3);
	lv_obj_set_width(ui_Speed_Value, LV_SIZE_CONTENT);	/// 1
	lv_obj_set_height(ui_Speed_Value, LV_SIZE_CONTENT); /// 1
	lv_obj_set_align(ui_Speed_Value, LV_ALIGN_CENTER);
	lv_obj_set_x(ui_Speed_Value, 0);
	lv_obj_set_y(ui_Speed_Value, 30);
	// Set initial speed value to "---" to indicate no signal yet
	lv_label_set_text(ui_Speed_Value, "---");
	strcpy(previous_values[SPEED_VALUE_ID - 1], "---");
	lv_obj_set_style_text_color(ui_Speed_Value, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_Speed_Value, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Speed_Value, THEME_FONT_DASH_SPEED,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_Kmh = lv_label_create(ui_Screen3);
	lv_obj_set_width(ui_Kmh, LV_SIZE_CONTENT);	/// 1
	lv_obj_set_height(ui_Kmh, LV_SIZE_CONTENT); /// 1
	lv_obj_set_x(ui_Kmh, 37);
	lv_obj_set_y(ui_Kmh, 64);
	lv_obj_set_align(ui_Kmh, LV_ALIGN_CENTER);
	// Set units text based on saved configuration
	lv_label_set_text(
		ui_Kmh, values_config[SPEED_VALUE_ID - 1].use_mph ? "mph" : "k/mh");
	lv_obj_set_style_text_color(ui_Kmh, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_Kmh, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Kmh, THEME_FONT_SMALL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_Indicator_Left = lv_img_create(ui_Screen3);
	lv_img_set_src(ui_Indicator_Left, &ui_img_indicator_left_png);
	lv_obj_set_width(ui_Indicator_Left, LV_SIZE_CONTENT);  /// 32
	lv_obj_set_height(ui_Indicator_Left, LV_SIZE_CONTENT); /// 30
	lv_obj_set_x(ui_Indicator_Left, -95);
	lv_obj_set_y(ui_Indicator_Left, -133);
	lv_obj_set_align(ui_Indicator_Left, LV_ALIGN_CENTER);
	lv_obj_add_flag(ui_Indicator_Left, LV_OBJ_FLAG_ADV_HITTEST);  /// Flags
	lv_obj_clear_flag(ui_Indicator_Left, LV_OBJ_FLAG_SCROLLABLE); /// Flags
	lv_obj_set_style_opa(ui_Indicator_Left, 50,
						 LV_PART_MAIN | LV_STATE_DEFAULT);
	// No outline for left indicator
	lv_obj_set_style_outline_width(ui_Indicator_Left, 0,
								   LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_Indicator_Right = lv_img_create(ui_Screen3);
	lv_img_set_src(ui_Indicator_Right, &ui_img_indicator_right_png);
	lv_obj_set_width(ui_Indicator_Right, LV_SIZE_CONTENT);	/// 32
	lv_obj_set_height(ui_Indicator_Right, LV_SIZE_CONTENT); /// 30
	lv_obj_set_x(ui_Indicator_Right, 95);
	lv_obj_set_y(ui_Indicator_Right, -133);
	lv_obj_set_align(ui_Indicator_Right, LV_ALIGN_CENTER);
	lv_obj_add_flag(ui_Indicator_Right, LV_OBJ_FLAG_ADV_HITTEST);  /// Flags
	lv_obj_clear_flag(ui_Indicator_Right, LV_OBJ_FLAG_SCROLLABLE); /// Flags
	lv_obj_set_style_opa(ui_Indicator_Right, 50,
						 LV_PART_MAIN | LV_STATE_DEFAULT);
	// No outline for right indicator
	lv_obj_set_style_outline_width(ui_Indicator_Right, 0,
								   LV_PART_MAIN | LV_STATE_DEFAULT);

	// Create transparent touch areas for indicators
	// Left indicator touch area
	lv_obj_t *left_indicator_touch_area = lv_obj_create(ui_Screen3);
	lv_obj_set_size(left_indicator_touch_area, 50, 50);
	lv_obj_set_x(left_indicator_touch_area, -95);
	lv_obj_set_y(left_indicator_touch_area, -133);
	lv_obj_set_align(left_indicator_touch_area, LV_ALIGN_CENTER);
	lv_obj_clear_flag(left_indicator_touch_area, LV_OBJ_FLAG_SCROLLABLE);

	// Make it transparent
	lv_obj_set_style_bg_opa(left_indicator_touch_area, 0,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(left_indicator_touch_area, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Add long press handler for left indicator
	uint8_t *left_indicator_id = lv_mem_alloc(sizeof(uint8_t));
	if (left_indicator_id) {
		*left_indicator_id = 0;
		lv_obj_add_event_cb(left_indicator_touch_area, indicator_longpress_cb,
							LV_EVENT_LONG_PRESSED, left_indicator_id);
		printf("Left indicator touch area created with ID %d\n",
			   *left_indicator_id);
	} else {
		printf("Error: Failed to allocate memory for left indicator ID\n");
	}

	// Right indicator touch area
	lv_obj_t *right_indicator_touch_area = lv_obj_create(ui_Screen3);
	lv_obj_set_size(right_indicator_touch_area, 50, 50);
	lv_obj_set_x(right_indicator_touch_area, 95);
	lv_obj_set_y(right_indicator_touch_area, -133);
	lv_obj_set_align(right_indicator_touch_area, LV_ALIGN_CENTER);
	lv_obj_clear_flag(right_indicator_touch_area, LV_OBJ_FLAG_SCROLLABLE);

	// Make it transparent
	lv_obj_set_style_bg_opa(right_indicator_touch_area, 0,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(right_indicator_touch_area, 0,
								  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Add long press handler for right indicator
	uint8_t *right_indicator_id = lv_mem_alloc(sizeof(uint8_t));
	if (right_indicator_id) {
		*right_indicator_id = 1;
		lv_obj_add_event_cb(right_indicator_touch_area, indicator_longpress_cb,
							LV_EVENT_LONG_PRESSED, right_indicator_id);
		printf("Right indicator touch area created with ID %d\n",
			   *right_indicator_id);
	} else {
		printf("Error: Failed to allocate memory for right indicator ID\n");
	}

	ui_Bar_1 = lv_bar_create(ui_Screen3);
	// Set default range if not configured
	if (values_config[BAR1_VALUE_ID - 1].bar_max <=
		values_config[BAR1_VALUE_ID - 1].bar_min) {
		values_config[BAR1_VALUE_ID - 1].bar_min = 0;
		values_config[BAR1_VALUE_ID - 1].bar_max = 100;
	}
	lv_bar_set_range(ui_Bar_1, values_config[BAR1_VALUE_ID - 1].bar_min,
					 values_config[BAR1_VALUE_ID - 1].bar_max);
	lv_bar_set_value(ui_Bar_1, values_config[BAR1_VALUE_ID - 1].bar_min,
					 LV_ANIM_OFF);
	lv_obj_set_width(ui_Bar_1, 300);
	lv_obj_set_height(ui_Bar_1, 30);
	lv_obj_set_x(ui_Bar_1, -240);
	lv_obj_set_y(ui_Bar_1, 209);
	lv_obj_set_align(ui_Bar_1, LV_ALIGN_CENTER);
	lv_obj_set_style_radius(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_Bar_1, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Bar_1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(ui_Bar_1, THEME_COLOR_PANEL,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_opa(ui_Bar_1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(ui_Bar_1, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_outline_width(
		ui_Bar_1, 0, LV_PART_MAIN | LV_STATE_DEFAULT); // Remove black outline
	lv_obj_set_style_outline_pad(ui_Bar_1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_left(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_top(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(ui_Bar_1, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_set_style_radius(ui_Bar_1, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_Bar_1, THEME_COLOR_GREEN_BRIGHT,
							  LV_PART_INDICATOR |
								  LV_STATE_DEFAULT); //(0x19439a) = cold blue
	lv_obj_set_style_bg_opa(ui_Bar_1, 255,
							LV_PART_INDICATOR | LV_STATE_DEFAULT);

	ui_Bar_1_Label = lv_label_create(ui_Screen3);
	lv_obj_set_width(ui_Bar_1_Label, LV_SIZE_CONTENT);	/// 1
	lv_obj_set_height(ui_Bar_1_Label, LV_SIZE_CONTENT); /// 1
	lv_obj_set_x(ui_Bar_1_Label, -240);
	lv_obj_set_y(ui_Bar_1_Label, 181);
	lv_obj_set_align(ui_Bar_1_Label, LV_ALIGN_CENTER);
	lv_label_set_text(ui_Bar_1_Label, label_texts[BAR1_VALUE_ID - 1]);
	lv_obj_set_style_text_color(ui_Bar_1_Label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_Bar_1_Label, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(ui_Bar_1_Label, LV_TEXT_ALIGN_CENTER,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Bar_1_Label, THEME_FONT_DASH_LABEL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ui_Bar_1_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_Bar_1_Label, THEME_COLOR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Bar_1_Label, 0,
							LV_PART_MAIN |
								LV_STATE_DEFAULT); // Remove black background
	lv_obj_set_style_pad_left(ui_Bar_1_Label, 10,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(ui_Bar_1_Label, 10,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_top(ui_Bar_1_Label, 0,
							 LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(ui_Bar_1_Label, 0,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Bar 1 Numeric Value Display
	ui_Bar_1_Value = lv_label_create(ui_Screen3);
	lv_obj_set_width(ui_Bar_1_Value,
					 80); // Fixed width for right-alignment behavior
	lv_obj_set_height(ui_Bar_1_Value, LV_SIZE_CONTENT);
	lv_obj_align(
		ui_Bar_1_Value, LV_ALIGN_CENTER, -140,
		181); // Position so right edge is at -100 (width 80, so center at -140)
	lv_label_set_text(ui_Bar_1_Value, "---"); // Initial value
	strcpy(previous_values[BAR1_VALUE_ID - 1], "---");
	lv_obj_set_style_text_color(ui_Bar_1_Value, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_Bar_1_Value, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(ui_Bar_1_Value, LV_TEXT_ALIGN_RIGHT,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Bar_1_Value, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ui_Bar_1_Value, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Bar_1_Value, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

	// Set initial visibility based on configuration
	if (!values_config[BAR1_VALUE_ID - 1].show_bar_value) {
		lv_obj_add_flag(ui_Bar_1_Value, LV_OBJ_FLAG_HIDDEN);
	}

	ui_Bar_2 = lv_bar_create(ui_Screen3);
	// Set default range if not configured
	if (values_config[BAR2_VALUE_ID - 1].bar_max <=
		values_config[BAR2_VALUE_ID - 1].bar_min) {
		values_config[BAR2_VALUE_ID - 1].bar_min = 0;
		values_config[BAR2_VALUE_ID - 1].bar_max = 100;
	}
	lv_bar_set_range(ui_Bar_2, values_config[BAR2_VALUE_ID - 1].bar_min,
					 values_config[BAR2_VALUE_ID - 1].bar_max);
	lv_bar_set_value(ui_Bar_2, values_config[BAR2_VALUE_ID - 1].bar_min,
					 LV_ANIM_OFF);
	lv_obj_set_width(ui_Bar_2, 300);
	lv_obj_set_height(ui_Bar_2, 30);
	lv_obj_set_x(ui_Bar_2, 240);
	lv_obj_set_y(ui_Bar_2, 209);
	lv_obj_set_align(ui_Bar_2, LV_ALIGN_CENTER);
	lv_obj_set_style_radius(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_Bar_2, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Bar_2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(ui_Bar_2, THEME_COLOR_PANEL,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_opa(ui_Bar_2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(ui_Bar_2, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_outline_width(
		ui_Bar_2, 0, LV_PART_MAIN | LV_STATE_DEFAULT); // Remove black outline
	lv_obj_set_style_outline_pad(ui_Bar_2, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_left(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_top(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(ui_Bar_2, 5, LV_PART_MAIN | LV_STATE_DEFAULT);

	lv_obj_set_style_radius(ui_Bar_2, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_Bar_2, THEME_COLOR_GREEN_BRIGHT,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Bar_2, 255,
							LV_PART_INDICATOR | LV_STATE_DEFAULT);

	ui_Bar_2_Label = lv_label_create(ui_Screen3);
	lv_obj_set_width(ui_Bar_2_Label, LV_SIZE_CONTENT);	/// 1
	lv_obj_set_height(ui_Bar_2_Label, LV_SIZE_CONTENT); /// 1
	lv_obj_set_x(ui_Bar_2_Label, 240);
	lv_obj_set_y(ui_Bar_2_Label, 181);
	lv_obj_set_align(ui_Bar_2_Label, LV_ALIGN_CENTER);
	lv_label_set_text(ui_Bar_2_Label, label_texts[BAR2_VALUE_ID - 1]);
	lv_obj_set_style_text_color(ui_Bar_2_Label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_Bar_2_Label, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Bar_2_Label, THEME_FONT_DASH_LABEL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ui_Bar_2_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_Bar_2_Label, THEME_COLOR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Bar_2_Label, 0,
							LV_PART_MAIN |
								LV_STATE_DEFAULT); // Remove black background
	lv_obj_set_style_pad_left(ui_Bar_2_Label, 10,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(ui_Bar_2_Label, 10,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_top(ui_Bar_2_Label, 0,
							 LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(ui_Bar_2_Label, 0,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	// Bar 2 Numeric Value Display
	ui_Bar_2_Value = lv_label_create(ui_Screen3);
	lv_obj_set_width(ui_Bar_2_Value,
					 80); // Fixed width for right-alignment behavior
	lv_obj_set_height(ui_Bar_2_Value, LV_SIZE_CONTENT);
	lv_obj_align(
		ui_Bar_2_Value, LV_ALIGN_CENTER, 340,
		181); // Position so right edge is at 380 (width 80, so center at 340)
	lv_label_set_text(ui_Bar_2_Value, "---"); // Initial value
	strcpy(previous_values[BAR2_VALUE_ID - 1], "---");
	lv_obj_set_style_text_color(ui_Bar_2_Value, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_Bar_2_Value, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(ui_Bar_2_Value, LV_TEXT_ALIGN_RIGHT,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Bar_2_Value, THEME_FONT_MEDIUM,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ui_Bar_2_Value, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Bar_2_Value, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

	// Set initial visibility based on configuration
	if (!values_config[BAR2_VALUE_ID - 1].show_bar_value) {
		lv_obj_add_flag(ui_Bar_2_Value, LV_OBJ_FLAG_HIDDEN);
	}

	ui_Gear_Panel = lv_obj_create(ui_Screen3);
	lv_obj_set_width(ui_Gear_Panel, 90);
	lv_obj_set_height(ui_Gear_Panel, 90);
	lv_obj_set_x(ui_Gear_Panel, 0);
	lv_obj_set_y(ui_Gear_Panel, 180);
	lv_obj_set_align(ui_Gear_Panel, LV_ALIGN_CENTER);
	lv_obj_clear_flag(ui_Gear_Panel, LV_OBJ_FLAG_SCROLLABLE); /// Flags
	lv_obj_set_style_radius(ui_Gear_Panel, 7, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(ui_Gear_Panel, THEME_COLOR_PANEL,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Gear_Panel, 255,
							LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_color(ui_Gear_Panel, THEME_COLOR_PANEL,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_opa(ui_Gear_Panel, 255,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(ui_Gear_Panel, 3,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_outline_color(ui_Gear_Panel, THEME_COLOR_TEXT_PRIMARY,
								   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_outline_opa(ui_Gear_Panel, 255,
								 LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_outline_width(ui_Gear_Panel, 0,
								   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_outline_pad(ui_Gear_Panel, 1,
								 LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_Gear_Label = lv_label_create(ui_Screen3);
	lv_obj_set_width(ui_Gear_Label, LV_SIZE_CONTENT);  /// 1
	lv_obj_set_height(ui_Gear_Label, LV_SIZE_CONTENT); /// 1
	lv_obj_set_x(ui_Gear_Label, 0);
	lv_obj_set_y(ui_Gear_Label, 152);
	lv_obj_set_align(ui_Gear_Label, LV_ALIGN_CENTER);
	lv_label_set_text(ui_Gear_Label, label_texts[GEAR_VALUE_ID - 1]);
	lv_obj_set_style_text_color(ui_Gear_Label, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_Gear_Label, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_Gear_Label, THEME_FONT_DASH_LABEL,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_radius(ui_Gear_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_left(ui_Gear_Label, 10,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_right(ui_Gear_Label, 10,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_top(ui_Gear_Label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_bottom(ui_Gear_Label, 0,
								LV_PART_MAIN | LV_STATE_DEFAULT);

	ui_GEAR_Value = lv_label_create(ui_Screen3);
	lv_obj_set_width(ui_GEAR_Value, 115);
	lv_obj_set_height(ui_GEAR_Value, LV_SIZE_CONTENT); /// 1
	lv_obj_set_x(ui_GEAR_Value, 10);
	lv_obj_set_y(ui_GEAR_Value, 198);
	lv_obj_set_align(ui_GEAR_Value, LV_ALIGN_CENTER);
	// Set initial gear value to "---" to indicate no signal yet or not assigned
	lv_label_set_text(ui_GEAR_Value, "---");
	strcpy(previous_values[GEAR_VALUE_ID - 1], "---");
	
	lv_obj_set_style_text_color(ui_GEAR_Value, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_opa(ui_GEAR_Value, 255,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_align(ui_GEAR_Value, LV_TEXT_ALIGN_CENTER,
								LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(ui_GEAR_Value, THEME_FONT_DASH_GEAR,
							   LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_transform_zoom(ui_GEAR_Value, 210,
									LV_PART_MAIN | LV_STATE_DEFAULT);

	// Create gear icon (for custom icon display)
	ui_GEAR_Icon = lv_img_create(ui_Screen3);
	lv_img_set_src(ui_GEAR_Icon, &Smart_Car_Key);
	// Set zoom to 88% (12% smaller): 256 * 0.88 = 225.28, use 225
	lv_img_set_zoom(ui_GEAR_Icon, 225);
	lv_obj_set_size(ui_GEAR_Icon, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_img_set_size_mode(ui_GEAR_Icon, LV_IMG_SIZE_MODE_REAL);
	// Set pivot point to center of image (30x58 original size, so center is 15, 29)
	// This ensures the icon stays centered when zoomed
	lv_img_set_pivot(ui_GEAR_Icon, 15, 29);
	// Position exactly like the text label - same coordinates and alignment
	lv_obj_set_x(ui_GEAR_Icon, 0);
	lv_obj_set_y(ui_GEAR_Icon, 194);
	lv_obj_set_align(ui_GEAR_Icon, LV_ALIGN_CENTER);
	// Initially hidden - will be shown when custom icon value matches
	lv_obj_add_flag(ui_GEAR_Icon, LV_OBJ_FLAG_HIDDEN);

	create_transparent_click_zone(ui_Screen3, ui_RPM_Value, RPM_VALUE_ID);
	create_transparent_click_zone(ui_Screen3, ui_Speed_Value, SPEED_VALUE_ID);
	create_transparent_click_zone(ui_Screen3, ui_GEAR_Value, GEAR_VALUE_ID);
	create_transparent_click_zone(ui_Screen3, ui_Bar_1, BAR1_VALUE_ID);
	create_transparent_click_zone(ui_Screen3, ui_Bar_2, BAR2_VALUE_ID);

	// Create Menu button (initially hidden, shown on tap) - Glass/Glassmorphism style
	ui_Menu_Button = lv_btn_create(ui_Screen3);
	lv_obj_set_width(ui_Menu_Button, 90);
	lv_obj_set_height(ui_Menu_Button, 40);
	lv_obj_set_x(ui_Menu_Button, 0);
	lv_obj_set_y(ui_Menu_Button, 105); // Centered between Speed (Y=30) and Gear (Y=180)
	lv_obj_set_align(ui_Menu_Button, LV_ALIGN_CENTER);
	lv_obj_add_flag(ui_Menu_Button, LV_OBJ_FLAG_HIDDEN); // Initially hidden
	
	// Glassmorphism effect: semi-transparent white background
	lv_obj_set_style_bg_color(ui_Menu_Button, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_opa(ui_Menu_Button, 60, LV_PART_MAIN | LV_STATE_DEFAULT); // Very transparent
	
	// Subtle white border for glass effect
	lv_obj_set_style_border_color(ui_Menu_Button, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(ui_Menu_Button, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_opa(ui_Menu_Button, 120, LV_PART_MAIN | LV_STATE_DEFAULT);
	
	// Rounded corners for modern glass look
	lv_obj_set_style_radius(ui_Menu_Button, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
	
	// Soft shadow for depth
	lv_obj_set_style_shadow_width(ui_Menu_Button, 15, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_color(ui_Menu_Button, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_shadow_opa(ui_Menu_Button, 100, LV_PART_MAIN | LV_STATE_DEFAULT);
	
	// Add label to button
	lv_obj_t *menu_button_label = lv_label_create(ui_Menu_Button);
	lv_label_set_text(menu_button_label, "MENU");
	lv_obj_set_style_text_color(menu_button_label, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(menu_button_label, THEME_FONT_MEDIUM, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(menu_button_label);
	
	// Add click event to menu button
	lv_obj_add_event_cb(ui_Menu_Button, menu_button_clicked_cb, LV_EVENT_CLICKED, NULL);

	ui_RDM_Logo_Text = lv_img_create(ui_Screen3);
	lv_img_set_src(ui_RDM_Logo_Text, &ui_img_RDM_Light);
	lv_obj_set_width(ui_RDM_Logo_Text, LV_SIZE_CONTENT);  /// 107
	lv_obj_set_height(ui_RDM_Logo_Text, LV_SIZE_CONTENT); /// 40
	lv_obj_set_x(ui_RDM_Logo_Text, 0);
	lv_obj_set_y(ui_RDM_Logo_Text, -65);
	lv_obj_set_align(ui_RDM_Logo_Text, LV_ALIGN_CENTER);
	lv_obj_add_flag(ui_RDM_Logo_Text, LV_OBJ_FLAG_ADV_HITTEST);	 /// Flags
	lv_obj_clear_flag(ui_RDM_Logo_Text, LV_OBJ_FLAG_SCROLLABLE); /// Flags
	lv_obj_add_event_cb(ui_RDM_Logo_Text, device_settings_longpress_cb,
						LV_EVENT_LONG_PRESSED, NULL);
	lv_obj_add_flag(ui_RDM_Logo_Text,
					LV_OBJ_FLAG_CLICKABLE); // Make sure it's clickable

	// In ui_Screen3_screen_init(), replace all individual warning creation
	// with:
	for (int i = 0; i < 8; i++) {
		// Create warning circle
		warning_circles[i] = lv_obj_create(ui_Screen3);
		lv_obj_set_width(warning_circles[i], 15);
		lv_obj_set_height(warning_circles[i], 15);
		lv_obj_set_x(warning_circles[i], warning_positions[i].x);
		lv_obj_set_y(warning_circles[i], warning_positions[i].y);
		lv_obj_set_align(warning_circles[i], LV_ALIGN_CENTER);
		lv_obj_clear_flag(warning_circles[i], LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_radius(warning_circles[i], 100,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_color(warning_circles[i], THEME_COLOR_INACTIVE,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_opa(warning_circles[i], 255,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(warning_circles[i], 0,
									  LV_PART_MAIN | LV_STATE_DEFAULT);

		// Create warning label
		warning_labels[i] = lv_label_create(ui_Screen3);
		lv_obj_set_width(
			warning_labels[i],
			LV_SIZE_CONTENT); // Auto width to prevent word breaking
		lv_obj_set_height(warning_labels[i], LV_SIZE_CONTENT); // Auto height
		lv_obj_set_x(warning_labels[i], warning_positions[i].x);
		lv_obj_set_y(warning_labels[i], -112);
		lv_obj_set_align(warning_labels[i], LV_ALIGN_CENTER);
		lv_obj_add_flag(warning_labels[i], LV_OBJ_FLAG_HIDDEN);

		// Use the saved configuration label if it exists, otherwise use default
		const char *saved_label = warning_configs[i].label;
		if (saved_label && saved_label[0] != '\0') {
			// If there's a saved label, use it
			lv_label_set_text(warning_labels[i], saved_label);
		} else {
			// Use default label if no saved configuration
			char label_text[20];
			snprintf(label_text, sizeof(label_text), "Warning\n%d", i + 1);
			lv_label_set_text(warning_labels[i], label_text);
		}

		lv_obj_set_style_text_color(warning_labels[i], THEME_COLOR_TEXT_PRIMARY,
									LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_opa(warning_labels[i], 255,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_align(warning_labels[i], LV_TEXT_ALIGN_CENTER,
									LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_text_font(warning_labels[i], THEME_FONT_TINY,
								   LV_PART_MAIN | LV_STATE_DEFAULT);

		// Don't reset current_state here - it was already loaded from NVS
		// If invert_toggle is enabled, it should have been set to true in load_warning_configs_from_nvs()
		// If not loaded from NVS, it defaults to false from init_warning_configs()

		// Create transparent touch area
		lv_obj_t *touch_area = lv_obj_create(ui_Screen3);
		lv_obj_set_size(touch_area, 50, 80);
		lv_obj_set_x(touch_area, warning_positions[i].x);
		lv_obj_set_y(touch_area, warning_positions[i].y);
		lv_obj_set_align(touch_area, LV_ALIGN_CENTER);
		lv_obj_clear_flag(touch_area, LV_OBJ_FLAG_SCROLLABLE);

		// Make it transparent
		lv_obj_set_style_bg_opa(touch_area, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_border_width(touch_area, 0,
									  LV_PART_MAIN | LV_STATE_DEFAULT);

		// Add long press handler
		uint8_t *warning_id = lv_mem_alloc(sizeof(uint8_t));
		*warning_id = i;
		lv_obj_add_event_cb(touch_area, warning_longpress_cb,
							LV_EVENT_LONG_PRESSED, warning_id);
		
		// Update UI to reflect the current state (especially for inverted warnings)
		// This ensures warnings with invert enabled show as active on boot
		update_warning_ui_immediate(i);
	}

	// Store references if needed
	ui_Warning_1 = warning_circles[0];
	ui_Warning_2 = warning_circles[1];
	ui_Warning_3 = warning_circles[2];
	ui_Warning_4 = warning_circles[3];
	ui_Warning_5 = warning_circles[4];
	ui_Warning_6 = warning_circles[5];
	ui_Warning_7 = warning_circles[6];
	ui_Warning_8 = warning_circles[7];

	// Create RPM lights circles if enabled (background layer)
	if (values_config[RPM_VALUE_ID - 1].rpm_lights_enabled) {
		create_rpm_lights_circles(ui_Screen3);
	}

	// CAN-driven indicators must start OFF until a CAN message sets them; avoid
	// stale state from previous run or NVS.
	for (int i = 0; i < 2; i++) {
		if (indicator_configs[i].input_source == 1) {
			indicator_configs[i].current_state = false;
			previous_indicator_states[i] = false;
		}
	}

	// Ensure indicators start with correct visual state (OFF unless bit is
	// active)
	printf("Setting initial indicator states - Left: %s, Right: %s\n",
		   indicator_configs[0].current_state ? "ACTIVE" : "INACTIVE",
		   indicator_configs[1].current_state ? "ACTIVE" : "INACTIVE");

	// Force update indicator UI to match current state
	for (int i = 0; i < 2; i++) {
		lv_obj_t *indicator_obj =
			(i == 0) ? ui_Indicator_Left : ui_Indicator_Right;
		if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
			if (indicator_configs[i].current_state) {
				// Active - set to 100% opacity
				lv_obj_set_style_opa(indicator_obj, 255,
									 LV_PART_MAIN | LV_STATE_DEFAULT);
				printf("Indicator %d: Set to ACTIVE (100%% opacity)\n", i);
			} else {
				// Inactive - set to 50% opacity (default/dimmed state)
				lv_obj_set_style_opa(indicator_obj, 50,
									 LV_PART_MAIN | LV_STATE_DEFAULT);
				printf("Indicator %d: Set to INACTIVE (50%% opacity)\n", i);
			}
		}
	}
}

// Interface function for other modules to refresh Screen3 labels
void refresh_screen3_labels(void) {
	printf("DEBUG: Refreshing Screen3 labels\n");

	// Update panel labels (1-8)
	for (int i = 0; i < 8; i++) {
		if (ui_Label[i] && lv_obj_is_valid(ui_Label[i])) {
			lv_label_set_text(ui_Label[i], label_texts[i]);
		}
	}

	// Update gear label
	if (ui_Gear_Label && lv_obj_is_valid(ui_Gear_Label)) {
		lv_label_set_text(ui_Gear_Label, label_texts[GEAR_VALUE_ID - 1]);
	}

	// Update bar labels
	if (ui_Bar_1_Label && lv_obj_is_valid(ui_Bar_1_Label)) {
		lv_label_set_text(ui_Bar_1_Label, label_texts[BAR1_VALUE_ID - 1]);
	}
	if (ui_Bar_2_Label && lv_obj_is_valid(ui_Bar_2_Label)) {
		lv_label_set_text(ui_Bar_2_Label, label_texts[BAR2_VALUE_ID - 1]);
	}

	printf("DEBUG: Screen3 labels refreshed\n");
}

// Speed/RPM Ratio gear calculation timer callback
void speed_rpm_gear_update_timer_cb(lv_timer_t *timer) {
	// Only calculate gear if Speed/RPM Ratio mode is selected
	if (values_config[GEAR_VALUE_ID - 1].gear_detection_mode != 4) {
		return;
	}

	// Get current RPM and Speed values (reset each time to avoid stale data)
	int current_rpm = 0;
	float current_speed = 0.0f;
	bool rpm_valid = false;
	bool speed_valid = false;
	
	// Parse RPM from previous_values array (it's stored as a string)
	if (strlen(previous_values[RPM_VALUE_ID - 1]) > 0 && 
		strcmp(previous_values[RPM_VALUE_ID - 1], "---") != 0 &&
		strcmp(previous_values[RPM_VALUE_ID - 1], "-") != 0) {
		current_rpm = atoi(previous_values[RPM_VALUE_ID - 1]);
		if (current_rpm > 0) {
			rpm_valid = true;
		}
	}
	
	// Parse Speed from previous_values array (it's stored as a string)
	if (strlen(previous_values[SPEED_VALUE_ID - 1]) > 0 && 
		strcmp(previous_values[SPEED_VALUE_ID - 1], "---") != 0 &&
		strcmp(previous_values[SPEED_VALUE_ID - 1], "-") != 0) {
		current_speed = atof(previous_values[SPEED_VALUE_ID - 1]);
		if (current_speed >= 0) {
			speed_valid = true;
		}
	}
	
	// Safety checks: need valid RPM and speed to calculate gear
	if (!rpm_valid || !speed_valid || current_rpm <= 500 || current_speed < 5.0f) {
		// Display "N" if not enough data or vehicle is stationary/idle
		char gear_str[EXAMPLE_MAX_CHAR_SIZE];
		snprintf(gear_str, sizeof(gear_str), "N");
		if (strcmp(gear_str, previous_values[GEAR_VALUE_ID - 1]) != 0) {
			strcpy(previous_values[GEAR_VALUE_ID - 1], gear_str);
			gear_update_t *g_upd = malloc(sizeof(gear_update_t));
			if (g_upd) {
				strcpy(g_upd->gear_str, gear_str);
				g_upd->raw_value = 0; // Speed/RPM ratio mode - no raw CAN value
				lv_async_call(update_gear_ui, g_upd);
			}
		}
		return;
	}
	
	// Convert speed to km/h if it's in MPH
	float speed_kmh = current_speed;
	if (values_config[SPEED_VALUE_ID - 1].use_mph) {
		speed_kmh = current_speed / 0.621371f; // Convert MPH to KMH
	}
	
	// Get configuration
	float tire_circ_mm = values_config[GEAR_VALUE_ID - 1].tire_circumference_mm;
	float final_drive = values_config[GEAR_VALUE_ID - 1].final_drive_ratio;
	
	// Safety check for configuration
	if (tire_circ_mm <= 0 || final_drive <= 0) {
		ESP_LOGW("GEAR", "Speed/RPM Ratio mode enabled but tire/final drive not configured!");
		return;
	}
	
	// Calculate current overall gear ratio
	// Formula: gear_ratio = (RPM * tire_circumference_mm * 60) / (speed_kph * final_drive_ratio * 1000000)
	// This simplifies to: gear_ratio = (RPM * tire_circ_mm * 0.00006) / (speed_kph * final_drive)
	float current_ratio = (current_rpm * tire_circ_mm * 0.00006f) / (speed_kmh * final_drive);
	
	// Find the closest matching gear by comparing ratios
	// Check reverse first (negative speed indicates reverse, but we'll check ratio anyway)
	int best_gear = -1;
	float smallest_diff = 999999.0f;
	static int last_gear = -1; // Remember last gear for hysteresis (-1 = reverse, 0 = neutral, 1-10 = forward gears)
	
	// Check reverse gear first
	float reverse_ratio = values_config[GEAR_VALUE_ID - 1].reverse_gear_ratio;
	if (reverse_ratio > 0.001f) {
		float diff = fabsf(current_ratio - reverse_ratio);
		float tolerance = reverse_ratio * 0.25f;
		
		// Add hysteresis: if this was the previous gear, give it preference
		if (last_gear == -1) {
			diff *= 0.8f;
		}
		
		if (diff < smallest_diff && diff < tolerance) {
			smallest_diff = diff;
			best_gear = -1; // -1 represents reverse
		}
	}
	
	// Check forward gears (1-10)
	for (int i = 0; i < 10; i++) {
		float gear_ratio = values_config[GEAR_VALUE_ID - 1].gear_ratios[i];
		
		// Skip gears with 0 ratio (not configured)
		if (gear_ratio <= 0.001f) {
			continue;
		}
		
		float diff = fabsf(current_ratio - gear_ratio);
		
		// Allow 25% tolerance for ratio matching (increased from 15% for better stability)
		float tolerance = gear_ratio * 0.25f;
		
		// Add hysteresis: if this was the previous gear, give it preference (reduce its diff by 20%)
		if (i + 1 == last_gear) {
			diff *= 0.8f;
		}
		
		if (diff < smallest_diff && diff < tolerance) {
			smallest_diff = diff;
			best_gear = i + 1; // Gear numbers are 1-10
		}
	}
	
	// Debug logging (can be removed later)
	static int log_counter = 0;
	if (log_counter++ % 10 == 0) { // Log every 10th calculation (every 2 seconds)
		ESP_LOGI("GEAR", "Speed/RPM Ratio: RPM=%d, Speed=%.1f km/h, Ratio=%.3f, Gear=%d", 
				 current_rpm, speed_kmh, current_ratio, best_gear);
	}
	
	// Update gear display
	char gear_str[EXAMPLE_MAX_CHAR_SIZE];
	if (best_gear == -1) {
		snprintf(gear_str, sizeof(gear_str), "R"); // Reverse
		last_gear = -1;
	} else if (best_gear > 0) {
		snprintf(gear_str, sizeof(gear_str), "%d", best_gear);
		last_gear = best_gear; // Remember this gear for next time
	} else {
		snprintf(gear_str, sizeof(gear_str), "-"); // No matching gear found
		last_gear = -1;
	}
	
	// Only update if gear changed
	if (strcmp(gear_str, previous_values[GEAR_VALUE_ID - 1]) != 0) {
		strcpy(previous_values[GEAR_VALUE_ID - 1], gear_str);
		gear_update_t *g_upd = malloc(sizeof(gear_update_t));
		if (g_upd) {
			strcpy(g_upd->gear_str, gear_str);
			g_upd->raw_value = 0; // Speed/RPM ratio mode - no raw CAN value
			lv_async_call(update_gear_ui, g_upd);
		}
	}
}

// Speed/RPM Ratio gear update timer callback
void gear_ecu_dropdown_event_cb(lv_event_t *e) {
	lv_obj_t *dropdown = lv_event_get_target(e);
	uint16_t selected = lv_dropdown_get_selected(dropdown);
	// Indices: 0 = Custom, 1 = MaxxECU, 2 = Haltech, 3 = Ford, 4 = Speed/RPM Ratio

	// External functions from menu_screen.c
	extern void create_custom_gear_values_section(lv_obj_t * parent,
												  uint8_t gear_mode);
	extern void hide_custom_gear_values_section(void);
	extern void create_speed_rpm_ratio_config_menu(void);

	if (selected == 0) {
		// ========== CUSTOM ==========
		printf("Gear ECU Presets: Custom (overriding device settings)\n");
		values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 0;
		// Keep existing CAN ID settings for custom mode
		// Custom mode overrides device settings - save this choice

		// Show custom gear values section - use ui_MenuScreen if available, otherwise current screen
		lv_obj_t *parent_screen = (ui_MenuScreen && lv_obj_is_valid(ui_MenuScreen)) ? ui_MenuScreen : lv_scr_act();
		create_custom_gear_values_section(parent_screen, 0);
		save_values_config_to_nvs();
		ESP_LOGI("MENU",
				 "Gear ECU set to Custom mode - device settings overridden");
	} else if (selected == 1) {
		// ========== MAXXECU ==========
		printf("Gear ECU Presets: MaxxECU\n");
		values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 1;

		// Hide custom gear values section
		hide_custom_gear_values_section();
		values_config[GEAR_VALUE_ID - 1].can_id = 536;	// 0x218
		values_config[GEAR_VALUE_ID - 1].endianess = 1; // Little Endian
		values_config[GEAR_VALUE_ID - 1].bit_start = 0;
		values_config[GEAR_VALUE_ID - 1].bit_length = 16;
		values_config[GEAR_VALUE_ID - 1].scale = 1.0f;
		values_config[GEAR_VALUE_ID - 1].value_offset = 0.0f;
		values_config[GEAR_VALUE_ID - 1].decimals = 0;
		values_config[GEAR_VALUE_ID - 1].is_signed = true; // MaxxECU uses signed values (-1 for reverse)

		// Update UI fields for GEAR (ID=11 => index=10)
		if (g_can_id_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%u",
					 values_config[GEAR_VALUE_ID - 1].can_id);
			lv_textarea_set_text(g_can_id_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_endian_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_endian_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].endianess);
		}

		if (g_bit_start_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_start_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].bit_start);
		}

		if (g_bit_length_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_length_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].bit_length - 1);
		}

		if (g_scale_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[GEAR_VALUE_ID - 1].scale);
			lv_textarea_set_text(g_scale_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_offset_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[GEAR_VALUE_ID - 1].value_offset);
			lv_textarea_set_text(g_offset_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_decimals_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_decimals_dropdown[GEAR_VALUE_ID - 1],
									 values_config[GEAR_VALUE_ID - 1].decimals);
		}
	} else if (selected == 2) {
		// ========== HALTECH ==========
		printf("Gear ECU Presets: Haltech\n");
		values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 2;

		// Hide custom gear values section
		hide_custom_gear_values_section();
		// Note: Haltech doesn't have a standard gear CAN ID in the preconfig
		// Users will need to configure manually or we can add a common one
		values_config[GEAR_VALUE_ID - 1].can_id =
			0x370; // Common Haltech gear CAN ID
		values_config[GEAR_VALUE_ID - 1].endianess = 0; // Big Endian
		values_config[GEAR_VALUE_ID - 1].bit_start = 16;
		values_config[GEAR_VALUE_ID - 1].bit_length = 16;
		values_config[GEAR_VALUE_ID - 1].scale = 1.0f;
		values_config[GEAR_VALUE_ID - 1].value_offset = 0.0f;
		values_config[GEAR_VALUE_ID - 1].decimals = 0;

		// Update UI fields similarly
		if (g_can_id_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%u",
					 values_config[GEAR_VALUE_ID - 1].can_id);
			lv_textarea_set_text(g_can_id_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_endian_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_endian_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].endianess);
		}

		if (g_bit_start_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_start_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].bit_start);
		}

		if (g_bit_length_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_length_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].bit_length - 1);
		}

		if (g_scale_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[GEAR_VALUE_ID - 1].scale);
			lv_textarea_set_text(g_scale_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_offset_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[GEAR_VALUE_ID - 1].value_offset);
			lv_textarea_set_text(g_offset_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_decimals_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_decimals_dropdown[GEAR_VALUE_ID - 1],
									 values_config[GEAR_VALUE_ID - 1].decimals);
		}
	} else if (selected == 3) {
		// ========== FORD ==========
		printf("Gear ECU Presets: Ford BA/BF/FG\n");
		values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 3;

		// Hide custom gear values section
		hide_custom_gear_values_section();
		values_config[GEAR_VALUE_ID - 1].can_id = 0x3E9; // Ford gear CAN ID
		values_config[GEAR_VALUE_ID - 1].endianess = 1;	 // Little Endian
		values_config[GEAR_VALUE_ID - 1].bit_start = 4;
		values_config[GEAR_VALUE_ID - 1].bit_length = 4;
		values_config[GEAR_VALUE_ID - 1].scale = 1.0f;
		values_config[GEAR_VALUE_ID - 1].value_offset = 0.0f;
		values_config[GEAR_VALUE_ID - 1].decimals = 0;

		// Update UI fields for GEAR (ID=11 => index=10)
		if (g_can_id_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%u",
					 values_config[GEAR_VALUE_ID - 1].can_id);
			lv_textarea_set_text(g_can_id_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_endian_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_endian_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].endianess);
		}

		if (g_bit_start_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_start_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].bit_start);
		}

		if (g_bit_length_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(
				g_bit_length_dropdown[GEAR_VALUE_ID - 1],
				values_config[GEAR_VALUE_ID - 1].bit_length - 1);
		}

		if (g_scale_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[GEAR_VALUE_ID - 1].scale);
			lv_textarea_set_text(g_scale_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_offset_input[GEAR_VALUE_ID - 1]) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%g",
					 values_config[GEAR_VALUE_ID - 1].value_offset);
			lv_textarea_set_text(g_offset_input[GEAR_VALUE_ID - 1], buf);
		}

		if (g_decimals_dropdown[GEAR_VALUE_ID - 1]) {
			lv_dropdown_set_selected(g_decimals_dropdown[GEAR_VALUE_ID - 1],
									 values_config[GEAR_VALUE_ID - 1].decimals);
		}
	} else if (selected == 4) {
		// ========== SPEED/RPM RATIO ==========
		printf("Gear ECU Presets: Speed/RPM Ratio (calculated gear estimation)\n");
		values_config[GEAR_VALUE_ID - 1].gear_detection_mode = 4;

		// Hide custom gear values section
		hide_custom_gear_values_section();
		
		// This mode doesn't use CAN for gear - it calculates from speed and RPM
		// The CAN processing sections will skip gear when in this mode
		
		// Open the Speed/RPM Ratio configuration menu
		create_speed_rpm_ratio_config_menu();
		
		save_values_config_to_nvs();
		ESP_LOGI("MENU", "Gear ECU set to Speed/RPM Ratio mode - opens configuration");
		return; // Exit early since we're opening a new menu
	}

	// Save configuration to NVS immediately (moved outside custom check)
	save_values_config_to_nvs();
	ESP_LOGI("MENU", "Gear ECU preset changed to: %s (saved to NVS)",
			 selected == 0
				 ? "Custom"
				 : (selected == 1 ? "MaxxECU"
								  : (selected == 2 ? "Haltech" 
								  : (selected == 3 ? "Ford" : "Speed/RPM Ratio"))));

	// Show/hide custom gear config button based on selected mode
	if (custom_gear_config_button != NULL &&
		lv_obj_is_valid(custom_gear_config_button)) {
		if (selected == 0) {
			// Custom mode - show the button
			lv_obj_clear_flag(custom_gear_config_button, LV_OBJ_FLAG_HIDDEN);
		} else {
			// MaxxECU, Haltech, Ford, or Speed/RPM Ratio mode - hide the button
			lv_obj_add_flag(custom_gear_config_button, LV_OBJ_FLAG_HIDDEN);
		}
	} else {
		// If button is invalid, reset the pointer
		custom_gear_config_button = NULL;
		ESP_LOGW("GEAR",
				 "Custom gear button reference was invalid, reset to NULL");
	}
}

// Custom gear CAN ID input objects
static lv_obj_t *custom_gear_can_inputs[10] = {NULL}; // N, R, 1-8

void custom_gear_can_id_event_cb(lv_event_t *e) {
	lv_obj_t *textarea = lv_event_get_target(e);

	// Find which gear this input corresponds to
	int gear_index = -1;
	for (int i = 0; i < 10; i++) {
		if (custom_gear_can_inputs[i] == textarea) {
			gear_index = i;
			break;
		}
	}

	if (gear_index == -1)
		return;

	// Get the CAN ID value from the input
	const char *can_id_str = lv_textarea_get_text(textarea);
	uint32_t can_id = 0;

	// Parse CAN ID (support both decimal and hex)
	if (strncmp(can_id_str, "0x", 2) == 0 ||
		strncmp(can_id_str, "0X", 2) == 0) {
		can_id = strtoul(can_id_str, NULL, 16);
	} else {
		can_id = strtoul(can_id_str, NULL, 10);
	}

	// Validate CAN ID range (0x000 to 0x7FF for standard CAN)
	if (can_id > 0x7FF) {
		can_id = 0x7FF;
		char corrected_str[16];
		snprintf(corrected_str, sizeof(corrected_str), "0x%03X", can_id);
		lv_textarea_set_text(textarea, corrected_str);
	}

	// Store the CAN ID
	// This function is deprecated - custom gear values are now handled in
	// menu_screen.c

	// Save to NVS
	save_values_config_to_nvs();

	// Log the change
	const char *gear_names[] = {"P", "R", "N", "D", "1", "2", "3",
								"4", "5", "6", "7", "8", "9", "10"};
	ESP_LOGI("GEAR", "Custom gear %s CAN ID set to: 0x%03X",
			 gear_names[gear_index], can_id);
}

void create_custom_gear_config_menu(void) {
	// Create a new screen for custom gear configuration
	lv_obj_t *custom_gear_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(custom_gear_screen, THEME_COLOR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title
	lv_obj_t *title_label = lv_label_create(custom_gear_screen);
	lv_label_set_text(title_label, "Custom Gear CAN ID Configuration");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(title_label, THEME_FONT_DASH_LABEL, 0);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 20);

	// Instructions
	lv_obj_t *instructions = lv_label_create(custom_gear_screen);
	lv_label_set_text(instructions,
					  "Set individual CAN IDs for each gear position.\nLeave "
					  "blank (0x000) to disable a gear.");
	lv_obj_set_style_text_color(instructions, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(instructions, THEME_FONT_TINY, 0);
	lv_obj_align(instructions, LV_ALIGN_TOP_MID, 0, 50);
	lv_obj_set_style_text_align(instructions, LV_TEXT_ALIGN_CENTER, 0);

	// Create gear configuration grid
	const char *gear_labels[] = {
		"Neutral (N)", "Reverse (R)", "Gear 1", "Gear 2", "Gear 3",
		"Gear 4",	   "Gear 5",	  "Gear 6", "Gear 7", "Gear 8"};

	for (int i = 0; i < 10; i++) {
		int row = i / 2;
		int col = i % 2;

		// Gear label
		lv_obj_t *gear_label = lv_label_create(custom_gear_screen);
		lv_label_set_text(gear_label, gear_labels[i]);
		lv_obj_set_style_text_color(gear_label, THEME_COLOR_TEXT_PRIMARY, 0);
		lv_obj_align(gear_label, LV_ALIGN_TOP_LEFT, 50 + col * 350,
					 100 + row * 60);

		// CAN ID input
		custom_gear_can_inputs[i] = lv_textarea_create(custom_gear_screen);
		lv_textarea_set_one_line(custom_gear_can_inputs[i], true);
		lv_textarea_set_max_length(custom_gear_can_inputs[i], 8);
		lv_obj_set_width(custom_gear_can_inputs[i], 100);
		lv_obj_set_height(custom_gear_can_inputs[i], 35);
		lv_obj_align(custom_gear_can_inputs[i], LV_ALIGN_TOP_LEFT,
					 200 + col * 350, 95 + row * 60);
		lv_obj_add_style(custom_gear_can_inputs[i], get_common_style(),
						 LV_PART_MAIN);

		// Set current value
		char current_value[16];
		uint32_t can_id = 0; // Deprecated function - no longer used
		if (can_id == 0) {
			strcpy(current_value, "0x000");
		} else {
			snprintf(current_value, sizeof(current_value), "0x%03X", can_id);
		}
		lv_textarea_set_text(custom_gear_can_inputs[i], current_value);

		// Add event callback
		lv_obj_add_event_cb(custom_gear_can_inputs[i],
							custom_gear_can_id_event_cb, LV_EVENT_VALUE_CHANGED,
							NULL);
		lv_obj_add_event_cb(custom_gear_can_inputs[i],
							custom_gear_can_id_event_cb, LV_EVENT_DEFOCUSED,
							NULL);
	}

	// Back button
	lv_obj_t *back_btn = lv_btn_create(custom_gear_screen);
	lv_obj_set_size(back_btn, 100, 40);
	lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_t *back_label = lv_label_create(back_btn);
	lv_label_set_text(back_label, "Back");
	lv_obj_center(back_label);
	lv_obj_add_event_cb(back_btn, custom_gear_back_btn_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Save button
	lv_obj_t *save_btn = lv_btn_create(custom_gear_screen);
	lv_obj_set_size(save_btn, 100, 40);
	lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_t *save_label = lv_label_create(save_btn);
	lv_label_set_text(save_label, "Save");
	lv_obj_center(save_label);
	lv_obj_add_event_cb(save_btn, custom_gear_save_btn_event_cb,
						LV_EVENT_CLICKED, NULL);

	// Load the custom gear screen
	lv_scr_load(custom_gear_screen);
}

// Custom gear back button event callback
void custom_gear_back_btn_event_cb(lv_event_t *e) {
	ESP_LOGI("GEAR", "Returning to gear configuration menu");
	// Go back to the gear configuration menu
	load_menu_screen_for_value(GEAR_VALUE_ID);
}

// Custom gear save button event callback
void custom_gear_save_btn_event_cb(lv_event_t *e) {
	ESP_LOGI("GEAR", "Saving custom gear configuration");
	// Save configuration to NVS
	save_values_config_to_nvs();
	// Go back to the gear configuration menu
	load_menu_screen_for_value(GEAR_VALUE_ID);
}

// Speed/RPM Ratio configuration inputs
static lv_obj_t *speed_rpm_tire_circumference_input = NULL;
static lv_obj_t *speed_rpm_final_drive_input = NULL;
static lv_obj_t *speed_rpm_reverse_ratio_input = NULL; // Reverse gear ratio
static lv_obj_t *speed_rpm_gear_ratio_inputs[10] = {NULL}; // Gear ratios for 1-10

// Speed/RPM Ratio input event callbacks
void speed_rpm_tire_circumference_event_cb(lv_event_t *e) {
	lv_obj_t *textarea = lv_event_get_target(e);
	const char *text = lv_textarea_get_text(textarea);
	values_config[GEAR_VALUE_ID - 1].tire_circumference_mm = atof(text);
	ESP_LOGI("GEAR", "Tire circumference set to: %.2f mm", values_config[GEAR_VALUE_ID - 1].tire_circumference_mm);
}

void speed_rpm_final_drive_event_cb(lv_event_t *e) {
	lv_obj_t *textarea = lv_event_get_target(e);
	const char *text = lv_textarea_get_text(textarea);
	values_config[GEAR_VALUE_ID - 1].final_drive_ratio = atof(text);
	ESP_LOGI("GEAR", "Final drive ratio set to: %.3f", values_config[GEAR_VALUE_ID - 1].final_drive_ratio);
}

void speed_rpm_reverse_ratio_event_cb(lv_event_t *e) {
	lv_obj_t *textarea = lv_event_get_target(e);
	const char *text = lv_textarea_get_text(textarea);
	values_config[GEAR_VALUE_ID - 1].reverse_gear_ratio = atof(text);
	ESP_LOGI("GEAR", "Reverse gear ratio set to: %.3f", values_config[GEAR_VALUE_ID - 1].reverse_gear_ratio);
}

void speed_rpm_gear_ratio_event_cb(lv_event_t *e) {
	lv_obj_t *textarea = lv_event_get_target(e);
	
	// Check if it's the reverse gear input
	if (speed_rpm_reverse_ratio_input == textarea) {
		speed_rpm_reverse_ratio_event_cb(e);
		return;
	}
	
	// Find which gear this input corresponds to
	int gear_index = -1;
	for (int i = 0; i < 10; i++) {
		if (speed_rpm_gear_ratio_inputs[i] == textarea) {
			gear_index = i;
			break;
		}
	}
	
	if (gear_index >= 0) {
		const char *text = lv_textarea_get_text(textarea);
		values_config[GEAR_VALUE_ID - 1].gear_ratios[gear_index] = atof(text);
		ESP_LOGI("GEAR", "Gear %d ratio set to: %.3f", gear_index + 1, values_config[GEAR_VALUE_ID - 1].gear_ratios[gear_index]);
	}
}

void speed_rpm_ratio_back_btn_event_cb(lv_event_t *e) {
	ESP_LOGI("GEAR", "Returning to gear configuration menu");
	load_menu_screen_for_value(GEAR_VALUE_ID);
}

void speed_rpm_ratio_save_btn_event_cb(lv_event_t *e) {
	ESP_LOGI("GEAR", "Saving Speed/RPM Ratio configuration");
	save_values_config_to_nvs();
	load_menu_screen_for_value(GEAR_VALUE_ID);
}

void create_speed_rpm_ratio_config_menu(void) {
	// Create a new screen for Speed/RPM Ratio configuration
	lv_obj_t *speed_rpm_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(speed_rpm_screen, THEME_COLOR_BG,
							  LV_PART_MAIN | LV_STATE_DEFAULT);

	// Title
	lv_obj_t *title_label = lv_label_create(speed_rpm_screen);
	lv_label_set_text(title_label, "Speed/RPM Ratio Gear Configuration");
	lv_obj_set_style_text_color(title_label, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(title_label, THEME_FONT_DASH_LABEL, 0);
	lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 10);

	// Instructions
	lv_obj_t *instructions = lv_label_create(speed_rpm_screen);
	lv_label_set_text(instructions,
					  "Configure tire size, final drive, and gear ratios.\nGear is estimated from speed and RPM.");
	lv_obj_set_style_text_color(instructions, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(instructions, THEME_FONT_TINY, 0);
	lv_obj_align(instructions, LV_ALIGN_TOP_MID, 0, 35);
	lv_obj_set_style_text_align(instructions, LV_TEXT_ALIGN_CENTER, 0);

	// Tire Circumference
	lv_obj_t *tire_label = lv_label_create(speed_rpm_screen);
	lv_label_set_text(tire_label, "Tire Circumference (mm):");
	lv_obj_set_style_text_color(tire_label, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_align(tire_label, LV_ALIGN_TOP_LEFT, 50, 70);
	
	speed_rpm_tire_circumference_input = lv_textarea_create(speed_rpm_screen);
	lv_textarea_set_one_line(speed_rpm_tire_circumference_input, true);
	lv_obj_set_width(speed_rpm_tire_circumference_input, 100);
	lv_obj_align(speed_rpm_tire_circumference_input, LV_ALIGN_TOP_LEFT, 250, 65);
	lv_obj_add_style(speed_rpm_tire_circumference_input, get_common_style(), LV_PART_MAIN);
	lv_obj_clear_flag(speed_rpm_tire_circumference_input, LV_OBJ_FLAG_SCROLLABLE); // Remove scrolling
	char tire_buf[16];
	snprintf(tire_buf, sizeof(tire_buf), "%.1f", values_config[GEAR_VALUE_ID - 1].tire_circumference_mm);
	lv_textarea_set_text(speed_rpm_tire_circumference_input, tire_buf);
	lv_obj_add_event_cb(speed_rpm_tire_circumference_input, speed_rpm_tire_circumference_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(speed_rpm_tire_circumference_input, keyboard_event_cb, LV_EVENT_ALL, NULL);

	// Final Drive Ratio
	lv_obj_t *final_drive_label = lv_label_create(speed_rpm_screen);
	lv_label_set_text(final_drive_label, "Final Drive Ratio:");
	lv_obj_set_style_text_color(final_drive_label, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_align(final_drive_label, LV_ALIGN_TOP_LEFT, 400, 70);
	
	speed_rpm_final_drive_input = lv_textarea_create(speed_rpm_screen);
	lv_textarea_set_one_line(speed_rpm_final_drive_input, true);
	lv_obj_set_width(speed_rpm_final_drive_input, 100);
	lv_obj_align(speed_rpm_final_drive_input, LV_ALIGN_TOP_LEFT, 580, 65);
	lv_obj_add_style(speed_rpm_final_drive_input, get_common_style(), LV_PART_MAIN);
	lv_obj_clear_flag(speed_rpm_final_drive_input, LV_OBJ_FLAG_SCROLLABLE); // Remove scrolling
	char final_drive_buf[16];
	snprintf(final_drive_buf, sizeof(final_drive_buf), "%.3f", values_config[GEAR_VALUE_ID - 1].final_drive_ratio);
	lv_textarea_set_text(speed_rpm_final_drive_input, final_drive_buf);
	lv_obj_add_event_cb(speed_rpm_final_drive_input, speed_rpm_final_drive_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(speed_rpm_final_drive_input, keyboard_event_cb, LV_EVENT_ALL, NULL);

	// Gear Ratios Grid (Reverse + 1-10, total 11 inputs)
	const char *gear_labels[] = {"Reverse", "1st", "2nd", "3rd", "4th", "5th", "6th", "7th", "8th", "9th", "10th"};
	
	for (int i = 0; i < 11; i++) {
		int row = i / 5;
		int col = i % 5;

		// Gear label
		lv_obj_t *gear_label = lv_label_create(speed_rpm_screen);
		lv_label_set_text(gear_label, gear_labels[i]);
		lv_obj_set_style_text_color(gear_label, THEME_COLOR_TEXT_PRIMARY, 0);
		lv_obj_align(gear_label, LV_ALIGN_TOP_LEFT, 50 + col * 150, 120 + row * 50);

		// Gear ratio input
		if (i == 0) {
			// Reverse gear input
			speed_rpm_reverse_ratio_input = lv_textarea_create(speed_rpm_screen);
			lv_textarea_set_one_line(speed_rpm_reverse_ratio_input, true);
			lv_obj_set_width(speed_rpm_reverse_ratio_input, 80);
			lv_obj_set_height(speed_rpm_reverse_ratio_input, 30);
			lv_obj_align(speed_rpm_reverse_ratio_input, LV_ALIGN_TOP_LEFT, 90 + col * 150, 115 + row * 50);
			lv_obj_add_style(speed_rpm_reverse_ratio_input, get_common_style(), LV_PART_MAIN);
			lv_obj_clear_flag(speed_rpm_reverse_ratio_input, LV_OBJ_FLAG_SCROLLABLE); // Remove scrolling
			
			// Set current value
			char reverse_buf[16];
			snprintf(reverse_buf, sizeof(reverse_buf), "%.3f", values_config[GEAR_VALUE_ID - 1].reverse_gear_ratio);
			lv_textarea_set_text(speed_rpm_reverse_ratio_input, reverse_buf);
			
			// Add event callbacks
			lv_obj_add_event_cb(speed_rpm_reverse_ratio_input, speed_rpm_reverse_ratio_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
			lv_obj_add_event_cb(speed_rpm_reverse_ratio_input, keyboard_event_cb, LV_EVENT_ALL, NULL);
		} else {
			// Forward gear inputs (1-10)
			int gear_index = i - 1; // Map to gear_ratios array (0-9 for gears 1-10)
			speed_rpm_gear_ratio_inputs[gear_index] = lv_textarea_create(speed_rpm_screen);
			lv_textarea_set_one_line(speed_rpm_gear_ratio_inputs[gear_index], true);
			lv_obj_set_width(speed_rpm_gear_ratio_inputs[gear_index], 80);
			lv_obj_set_height(speed_rpm_gear_ratio_inputs[gear_index], 30);
			lv_obj_align(speed_rpm_gear_ratio_inputs[gear_index], LV_ALIGN_TOP_LEFT, 90 + col * 150, 115 + row * 50);
			lv_obj_add_style(speed_rpm_gear_ratio_inputs[gear_index], get_common_style(), LV_PART_MAIN);
			lv_obj_clear_flag(speed_rpm_gear_ratio_inputs[gear_index], LV_OBJ_FLAG_SCROLLABLE); // Remove scrolling

			// Set current value
			char ratio_buf[16];
			snprintf(ratio_buf, sizeof(ratio_buf), "%.3f", values_config[GEAR_VALUE_ID - 1].gear_ratios[gear_index]);
			lv_textarea_set_text(speed_rpm_gear_ratio_inputs[gear_index], ratio_buf);

			// Add event callbacks
			lv_obj_add_event_cb(speed_rpm_gear_ratio_inputs[gear_index], speed_rpm_gear_ratio_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
			lv_obj_add_event_cb(speed_rpm_gear_ratio_inputs[gear_index], keyboard_event_cb, LV_EVENT_ALL, NULL);
		}
	}

	// Back button
	lv_obj_t *back_btn = lv_btn_create(speed_rpm_screen);
	lv_obj_set_size(back_btn, 100, 40);
	lv_obj_align(back_btn, LV_ALIGN_BOTTOM_LEFT, 50, -20);
	lv_obj_t *back_label = lv_label_create(back_btn);
	lv_label_set_text(back_label, "Back");
	lv_obj_center(back_label);
	lv_obj_add_event_cb(back_btn, speed_rpm_ratio_back_btn_event_cb, LV_EVENT_CLICKED, NULL);

	// Save button
	lv_obj_t *save_btn = lv_btn_create(speed_rpm_screen);
	lv_obj_set_size(save_btn, 100, 40);
	lv_obj_align(save_btn, LV_ALIGN_BOTTOM_RIGHT, -50, -20);
	lv_obj_t *save_label = lv_label_create(save_btn);
	lv_label_set_text(save_label, "Save");
	lv_obj_center(save_label);
	lv_obj_add_event_cb(save_btn, speed_rpm_ratio_save_btn_event_cb, LV_EVENT_CLICKED, NULL);

	// Load the Speed/RPM Ratio screen
	lv_scr_load(speed_rpm_screen);
}

// Timer callback for indicator animation
void indicator_animation_timer_cb(lv_timer_t *timer) {
	// Toggle animation state (realistic car indicator timing)
	indicator_animation_state = !indicator_animation_state;

	bool any_indicator_animating = false;

	// Update both indicators if they're active and have animation enabled
	for (int i = 0; i < 2; i++) {
		if (indicator_configs[i].current_state) {
			lv_obj_t *indicator_obj =
				(i == 0) ? ui_Indicator_Left : ui_Indicator_Right;
			if (indicator_obj && lv_obj_is_valid(indicator_obj)) {
				if (indicator_configs[i].animation_enabled) {
					// Flash between 100% and 50% opacity like a real indicator
					uint8_t opacity =
						indicator_animation_state
							? 255
							: 128; // 100% or 50% (128 = 50% of 255)
					lv_obj_set_style_opa(indicator_obj, opacity,
										 LV_PART_MAIN | LV_STATE_DEFAULT);
					any_indicator_animating = true;
				} else {
					// Solid mode - always 100% opacity when active
					lv_obj_set_style_opa(indicator_obj, 255,
										 LV_PART_MAIN | LV_STATE_DEFAULT);
				}
			}
		}
	}

	// If no indicators are animating, pause the timer to save CPU
	if (!any_indicator_animating && indicator_animation_timer) {
		lv_timer_pause(indicator_animation_timer);
	}
}
