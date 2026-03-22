/**
 * widget_shift_light.h -- Shift light widget for racing dashboards.
 *
 * Displays a row of colored LEDs that progressively illuminate as RPM
 * approaches redline, then flash at the shift point.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "widget_types.h"

typedef struct {
    char     signal_name[32];
    int16_t  signal_index;
    uint8_t  led_count;        /* Number of LEDs (4-16, default 8) */
    float    range_min;        /* RPM value where first LED lights (e.g., 4000) */
    float    range_max;        /* RPM value where all LEDs lit (e.g., 7000) */
    float    flash_threshold;  /* RPM value where LEDs start flashing (e.g., 7200) */
    uint16_t flash_speed;      /* Flash period in ms (default 200) */
    lv_color_t color_low;      /* Color for low RPM LEDs (green) */
    lv_color_t color_mid;      /* Color for mid RPM LEDs (yellow) */
    lv_color_t color_high;     /* Color for high RPM LEDs (red) */
    lv_color_t color_off;      /* Color for inactive LEDs (dark gray) */
    uint8_t  led_spacing;      /* Gap between LEDs in pixels (default 2) */
    uint8_t  border_radius;    /* LED corner radius (default 2) */
    uint8_t  led_width;        /* Per-LED width override, 0 = auto-fit (default 0) */
    uint8_t  led_height;       /* Per-LED height override, 0 = auto-fit (default 0) */
    uint8_t  fill_mode;        /* 0 = left-to-right, 1 = outside-to-inside (default 0) */
    float    threshold_mid;    /* Position (0-1) where color switches from low→mid (default 0.5) */
    float    threshold_high;   /* Position (0-1) where color switches from mid→high (default 0.8) */
    bool     flash_state;      /* Runtime: current flash on/off */
    uint8_t  active_count;     /* Runtime: how many LEDs are active */
    lv_obj_t *leds[16];        /* Runtime: LVGL rectangle objects */
    lv_timer_t *flash_timer;   /* Runtime: flash animation timer */
} shift_light_data_t;

widget_t *widget_shift_light_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
