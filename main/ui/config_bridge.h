/**
 * config_bridge.h — Bridge between config modal (value_id based) and
 * widget type_data + signal registry.
 *
 * Provides lookup functions so the touchscreen config modal can read/write
 * the same data that the web UI and JSON persistence use.
 */
#pragma once

#include "lvgl.h"
#include "widgets/widget_types.h"
#include "widgets/signal.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Value ID → Widget lookup ─────────────────────────────────────────── */

/**
 * Find the widget_t for a given value_id.
 * value_id 1-8  → WIDGET_PANEL slot 0-7
 * value_id 9    → WIDGET_RPM_BAR
 * value_id 10   → WIDGET_SPEED
 * value_id 11   → WIDGET_GEAR
 * value_id 12   → WIDGET_BAR slot 0
 * value_id 13   → WIDGET_BAR slot 1
 */
widget_t *config_bridge_get_widget(uint8_t value_id);

/* ── Signal (CAN) field readers ───────────────────────────────────────── */

signal_t *config_bridge_get_signal(uint8_t value_id);
uint32_t  config_bridge_get_can_id(uint8_t value_id);
uint8_t   config_bridge_get_bit_start(uint8_t value_id);
uint8_t   config_bridge_get_bit_length(uint8_t value_id);
float     config_bridge_get_scale(uint8_t value_id);
float     config_bridge_get_offset(uint8_t value_id);
uint8_t   config_bridge_get_endian(uint8_t value_id);
bool      config_bridge_get_is_signed(uint8_t value_id);

/* ── Signal (CAN) field writers ───────────────────────────────────────── */

void config_bridge_set_can_id(uint8_t value_id, uint32_t can_id);
void config_bridge_set_bit_start(uint8_t value_id, uint8_t bit_start);
void config_bridge_set_bit_length(uint8_t value_id, uint8_t bit_length);
void config_bridge_set_scale(uint8_t value_id, float scale);
void config_bridge_set_offset(uint8_t value_id, float offset);
void config_bridge_set_endian(uint8_t value_id, uint8_t endian);
void config_bridge_set_is_signed(uint8_t value_id, bool is_signed);

/* ── Display field accessors ──────────────────────────────────────────── */

const char *config_bridge_get_label(uint8_t value_id);
void        config_bridge_set_label(uint8_t value_id, const char *label);
uint8_t     config_bridge_get_decimals(uint8_t value_id);
void        config_bridge_set_decimals(uint8_t value_id, uint8_t decimals);
const char *config_bridge_get_custom_text(uint8_t value_id);
void        config_bridge_set_custom_text(uint8_t value_id, const char *text);

/* ── Speed-specific ───────────────────────────────────────────────────── */

bool config_bridge_get_use_mph(void);
void config_bridge_set_use_mph(bool use_mph);

/* ── Bar-specific ─────────────────────────────────────────────────────── */

int32_t    config_bridge_get_bar_min(uint8_t value_id);
int32_t    config_bridge_get_bar_max(uint8_t value_id);
bool       config_bridge_get_show_bar_value(uint8_t value_id);
bool       config_bridge_get_invert_bar_value(uint8_t value_id);
bool       config_bridge_get_fuel_sender(uint8_t value_id);
float      config_bridge_get_fuel_sender_empty_v(uint8_t value_id);
float      config_bridge_get_fuel_sender_full_v(uint8_t value_id);
uint8_t    config_bridge_get_fuel_sender_filter(uint8_t value_id);

void config_bridge_set_bar_min(uint8_t value_id, int32_t val);
void config_bridge_set_bar_max(uint8_t value_id, int32_t val);
void config_bridge_set_show_bar_value(uint8_t value_id, bool val);
void config_bridge_set_invert_bar_value(uint8_t value_id, bool val);
void config_bridge_set_fuel_sender(uint8_t value_id, bool val);
void config_bridge_set_fuel_sender_empty_v(uint8_t value_id, float val);
void config_bridge_set_fuel_sender_full_v(uint8_t value_id, float val);
void config_bridge_set_fuel_sender_filter(uint8_t value_id, uint8_t val);

/* ── Panel alert-specific ─────────────────────────────────────────────── */

float      config_bridge_get_warning_high_threshold(uint8_t value_id);
float      config_bridge_get_warning_low_threshold(uint8_t value_id);
lv_color_t config_bridge_get_warning_high_color(uint8_t value_id);
lv_color_t config_bridge_get_warning_low_color(uint8_t value_id);
bool       config_bridge_get_warning_high_enabled(uint8_t value_id);
bool       config_bridge_get_warning_low_enabled(uint8_t value_id);

void config_bridge_set_warning_high_threshold(uint8_t value_id, float val);
void config_bridge_set_warning_low_threshold(uint8_t value_id, float val);
void config_bridge_set_warning_high_color(uint8_t value_id, lv_color_t color);
void config_bridge_set_warning_low_color(uint8_t value_id, lv_color_t color);

/* ── Bar alert-specific ───────────────────────────────────────────────── */

int32_t    config_bridge_get_bar_low(uint8_t value_id);
int32_t    config_bridge_get_bar_high(uint8_t value_id);
lv_color_t config_bridge_get_bar_low_color(uint8_t value_id);
lv_color_t config_bridge_get_bar_high_color(uint8_t value_id);
lv_color_t config_bridge_get_bar_in_range_color(uint8_t value_id);

void config_bridge_set_bar_low(uint8_t value_id, int32_t val);
void config_bridge_set_bar_high(uint8_t value_id, int32_t val);
void config_bridge_set_bar_low_color(uint8_t value_id, lv_color_t color);
void config_bridge_set_bar_high_color(uint8_t value_id, lv_color_t color);
void config_bridge_set_bar_in_range_color(uint8_t value_id, lv_color_t color);

/* ── RPM-specific ─────────────────────────────────────────────────────── */

lv_color_t config_bridge_get_rpm_bar_color(void);
uint8_t    config_bridge_get_rpm_limiter_effect(void);
int32_t    config_bridge_get_rpm_limiter_value(void);
lv_color_t config_bridge_get_rpm_limiter_color(void);
bool       config_bridge_get_rpm_background_enabled(void);
int32_t    config_bridge_get_rpm_background_value(void);
lv_color_t config_bridge_get_rpm_background_color(void);

/* ── Gear-specific ────────────────────────────────────────────────────── */

uint8_t  config_bridge_get_gear_detection_mode(void);
void     config_bridge_set_gear_detection_mode(uint8_t mode);
float    config_bridge_get_tire_circumference(void);
void     config_bridge_set_tire_circumference(float val);
float    config_bridge_get_final_drive_ratio(void);
void     config_bridge_set_final_drive_ratio(float val);
float    config_bridge_get_reverse_gear_ratio(void);
void     config_bridge_set_reverse_gear_ratio(float val);
float    config_bridge_get_gear_ratio(uint8_t idx);
void     config_bridge_set_gear_ratio(uint8_t idx, float val);
uint32_t config_bridge_get_gear_custom_value(uint8_t idx);
void     config_bridge_set_gear_custom_value(uint8_t idx, uint32_t val);
uint8_t  config_bridge_get_gear_icon_type(uint8_t idx);
void     config_bridge_set_gear_icon_type(uint8_t idx, uint8_t type);
uint32_t config_bridge_get_gear_icon_value(uint8_t idx);
void     config_bridge_set_gear_icon_value(uint8_t idx, uint32_t val);

/* ── Signal auto-create ───────────────────────────────────────────────── */

/**
 * Ensure a signal exists for the given value_id.
 * If the widget has no signal bound, creates one with a generated name
 * and binds it. Returns the signal_t pointer.
 */
signal_t *config_bridge_ensure_signal(uint8_t value_id);

#ifdef __cplusplus
}
#endif
