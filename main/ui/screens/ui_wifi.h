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

/* Preset modes for wifi_ui_show_with_preset. Mirrors the internal Off/STA/AP
 * enum but exposed as a stable public ABI for the first-run wizard. */
typedef enum {
    WIFI_UI_PRESET_STA = 1,   /* WiFi client mode + persist wifi-on-boot */
    WIFI_UI_PRESET_AP  = 2,   /* Hotspot mode + persist hotspot-on-boot */
} wifi_ui_preset_t;

/* Switch runtime to the requested mode AND persist matching Start-on-Boot,
 * then open the WiFi settings screen. Used by the first-run wizard so the
 * user lands on a screen that already reflects their choice. */
void wifi_ui_show_with_preset(wifi_ui_preset_t preset);

/* Backward compat */
const char *wifi_get_ap_ssid(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_WIFI_H */
