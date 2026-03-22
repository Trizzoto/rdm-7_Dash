#ifndef UI_WIFI_H
#define UI_WIFI_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle -- called from device_settings and main.c */
void wifi_ui_init(void);          /* One-time init (register wifi_manager event cb) */
void wifi_ui_show(void);          /* Create & show settings screen */
void wifi_ui_hide(void);          /* Hide and destroy screen */
bool wifi_ui_is_active(void);

/* Backward compat */
const char *wifi_get_ap_ssid(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_WIFI_H */
