#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WIFI_MGR_STATE_OFF,
    WIFI_MGR_STATE_IDLE,
    WIFI_MGR_STATE_SCANNING,
    WIFI_MGR_STATE_CONNECTING,
    WIFI_MGR_STATE_CONNECTED,
    WIFI_MGR_STATE_AP_ONLY,
    WIFI_MGR_STATE_FAILED
} wifi_mgr_state_t;

typedef void (*wifi_mgr_event_cb_t)(wifi_mgr_state_t new_state, void *user_data);

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t auth_mode;
} wifi_mgr_ap_record_t;

/* Lifecycle */
void wifi_manager_init(void);          /* Create netif + event loop, NO radio start */
void wifi_manager_start(void);         /* Start radio with AP+STA or STA based on config */
void wifi_manager_stop(void);          /* Stop radio */
bool wifi_manager_is_started(void);

/* STA operations */
void wifi_manager_scan(void);
void wifi_manager_connect(const char *ssid, const char *password);
void wifi_manager_disconnect(void);
void wifi_manager_forget(void);        /* Disconnect + clear saved creds */
void wifi_manager_auto_connect(void);  /* Connect to saved creds if available */

/* AP operations */
void wifi_manager_enable_ap(bool enable);
bool wifi_manager_is_ap_enabled(void);
void wifi_manager_set_ap_password(const char *password);

/* State queries */
wifi_mgr_state_t wifi_manager_get_state(void);
const char *wifi_manager_get_connected_ssid(void);
const char *wifi_manager_get_ap_ssid(void);
const char *wifi_manager_get_sta_ip(void);
const char *wifi_manager_get_ap_ip(void);

/* Scan results */
uint16_t wifi_manager_get_scan_results(wifi_mgr_ap_record_t *out, uint16_t max);

/* Event subscription */
void wifi_manager_set_event_cb(wifi_mgr_event_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif
