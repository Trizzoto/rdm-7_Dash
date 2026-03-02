#pragma once
#include "lvgl.h"
#include "ui/screens/ui_Screen3.h"
#ifdef __cplusplus
extern "C" {
#endif

/** Create gear panel, value label, and custom icon on parent. */
void widget_gear_create(lv_obj_t *parent);

/** Immediate gear UI update. */
void update_gear_ui_immediate(const char *gear_str, uint32_t raw_value);

/** Async gear UI update (lv_async_call compatible). */
void update_gear_ui(void *param);

/** 200 ms timer callback for Speed/RPM ratio gear calculation. */
void speed_rpm_gear_update_timer_cb(lv_timer_t *timer);

/** Gear ECU preset dropdown callback (registered by config_modal). */
void gear_ecu_dropdown_event_cb(lv_event_t *e);

/** Custom gear CAN-ID input callback. */
void custom_gear_can_id_event_cb(lv_event_t *e);

/** Create the custom gear values config overlay. */
void create_custom_gear_config_menu(void);

/** Speed/RPM ratio config save button callback. */
void speed_rpm_ratio_save_btn_event_cb(lv_event_t *e);

/** Speed/RPM ratio back button callback. */
void speed_rpm_ratio_back_btn_event_cb(lv_event_t *e);

/** Create the speed/RPM ratio config overlay. */
void create_speed_rpm_ratio_config_menu(void);

/** Custom gear save/back button callbacks. */
void custom_gear_save_btn_event_cb(lv_event_t *e);
void custom_gear_back_btn_event_cb(lv_event_t *e);

/** Returns pointer to last_gear_can_received for dispatcher timeout tracking. */
uint64_t *widget_gear_get_last_can_time(void);

/* ui_Gear_Label is defined in widget_gear.c; declare extern for extern access */
extern lv_obj_t *ui_Gear_Label;

#ifdef __cplusplus
}
#endif
