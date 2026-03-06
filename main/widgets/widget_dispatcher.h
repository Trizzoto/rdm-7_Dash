#pragma once
#include "lvgl.h"
#include "driver/twai.h"
#include "ui/screens/ui_Screen3.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Update struct types shared across widget dispatcher and widget files ------*/

#ifndef PANEL_UPDATE_T_DEFINED
#define PANEL_UPDATE_T_DEFINED
#ifndef EXAMPLE_MAX_CHAR_SIZE
#define EXAMPLE_MAX_CHAR_SIZE 64
#endif
typedef struct {
    uint8_t panel_index;
    char    value_str[EXAMPLE_MAX_CHAR_SIZE];
    double  final_value;
} panel_update_t;
#endif

#ifndef BAR_UPDATE_T_DEFINED
#define BAR_UPDATE_T_DEFINED
typedef struct {
    uint8_t bar_index;
    int32_t bar_value;
    double  final_value;
    int     config_index;
    bool    is_timeout;
} bar_update_t;
#endif

#ifndef RPM_UPDATE_T_DEFINED
#define RPM_UPDATE_T_DEFINED
typedef struct {
    char rpm_str[EXAMPLE_MAX_CHAR_SIZE];
    int  rpm_value;
} rpm_update_t;
#endif

#ifndef SPEED_UPDATE_T_DEFINED
#define SPEED_UPDATE_T_DEFINED
typedef struct {
    char speed_str[EXAMPLE_MAX_CHAR_SIZE];
} speed_update_t;
#endif

#ifndef GEAR_UPDATE_T_DEFINED
#define GEAR_UPDATE_T_DEFINED
typedef struct {
    char     gear_str[EXAMPLE_MAX_CHAR_SIZE];
    uint32_t raw_value;
} gear_update_t;
#endif

#ifndef TEXT_UPDATE_T_DEFINED
#define TEXT_UPDATE_T_DEFINED
typedef struct {
    uint8_t value_idx;
    char    value_str[EXAMPLE_MAX_CHAR_SIZE];
} text_update_t;
#endif

/* --- API ------------------------------------------------------------------*/

/** Initialise all value_config_t defaults. Called at start of screen init. */
void init_values_config_defaults(void);

/** Refresh all Screen3 text labels from saved config (called after NVS load). */
void refresh_screen3_labels(void);

/** Main CAN frame dispatcher — routes message to correct widget update. */
void process_can_message(const twai_message_t *message);

/** 1-second timer that sends "---" to widgets whose CAN feed has timed out. */
void check_can_timeouts(lv_timer_t *timer);

#ifdef __cplusplus
}
#endif
