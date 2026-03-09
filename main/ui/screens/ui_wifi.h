#ifndef UI_WIFI_H
#define UI_WIFI_H

#include "lvgl.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi_types.h"
#include "esp_netif.h"

// Function declarations
void init_wifi_screen(void);
void show_wifi_screen(void);
void hide_wifi_screen(void);
void wifi_screen_delete(void);
bool is_wifi_screen_active(void);

// Auto-connect to saved WiFi credentials (call from app_main at boot)
void wifi_auto_connect(void);

// Add extern declaration for connected_ssid
extern char *connected_ssid;

#endif // UI_WIFI_H