#include "wifi_manager.h"
#include "ota_handler.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "storage/config_store.h"
#include "lvgl.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "wifi_mgr";

#define DEFAULT_SCAN_LIST_SIZE 20
#define WIFI_RECONNECT_MAX_ATTEMPTS  5
#define WIFI_RECONNECT_BASE_DELAY_MS 2000   /* doubles each attempt */
#define WIFI_RECONNECT_MAX_DELAY_MS  30000

/* ── Static state ─────────────────────────────────────────────────────── */

static bool             s_initialized   = false;
static bool             s_started       = false;
static bool             s_ap_enabled    = false;
static _Atomic wifi_mgr_state_t s_state  = WIFI_MGR_STATE_OFF;

static int              s_reconnect_attempts = 0;
static bool             s_should_reconnect   = false;
static bool             s_user_disconnect    = false;
static TimerHandle_t    s_reconnect_timer    = NULL;

static esp_netif_t     *s_sta_netif     = NULL;
static esp_netif_t     *s_ap_netif      = NULL;

static char             s_connected_ssid[33]  = {0};
static char             s_ap_ssid[16]         = {0};
static char             s_sta_ip[20]          = {0};
static char             s_pending_ssid[33]    = {0};
static char             s_pending_pass[65]    = {0};

/* Auto-connect: scan first, then connect if SSID found */
static bool             s_auto_connect_pending = false;

/* Forward-decl — definition lives below with the other helpers. Used by
 * _process_scan_results which is emitted before the definition. */
static bool _has_saved_sta_creds(void);
static char             s_auto_ssid[33]        = {0};
static char             s_auto_pass[65]        = {0};

static wifi_mgr_event_cb_t s_event_cb    = NULL;
static void                *s_event_ud   = NULL;

/* Scan result cache */
static wifi_mgr_ap_record_t s_scan_results[DEFAULT_SCAN_LIST_SIZE];
static uint16_t             s_scan_count = 0;

/* ── Forward declarations ─────────────────────────────────────────────── */

static void _stop_reconnect(void);

/* ── Helpers ──────────────────────────────────────────────────────────── */

typedef struct {
    wifi_mgr_event_cb_t cb;
    wifi_mgr_state_t    state;
    void               *user_data;
} _deferred_cb_t;

static void _deferred_event_cb(void *arg)
{
    _deferred_cb_t *ctx = (_deferred_cb_t *)arg;
    if (ctx->cb) {
        ctx->cb(ctx->state, ctx->user_data);
    }
    free(ctx);
}

static void _set_state(wifi_mgr_state_t new_state)
{
    s_state = new_state;
    if (s_event_cb) {
        _deferred_cb_t *ctx = malloc(sizeof(_deferred_cb_t));
        if (ctx) {
            ctx->cb        = s_event_cb;
            ctx->state     = new_state;
            ctx->user_data = s_event_ud;
            lv_async_call(_deferred_event_cb, ctx);
        }
    }
}

/* ── Reconnect logic ─────────────────────────────────────────────────── */

/* Deferred to LVGL task — timer service task stack is too small for
 * esp_wifi_connect() + _set_state() (malloc + lv_async_call). */
/* Multi-SSID rotation (#19): after each failed attempt on the current slot,
   advance to the next known network before the exponential backoff runs again.
   On an empty list, falls back to the legacy single-cred path. */
static uint8_t s_current_wifi_slot = 0;

static void _deferred_reconnect(void *arg)
{
    (void)arg;
    if (!s_started || !s_should_reconnect) return;

    s_reconnect_attempts++;
    ESP_LOGI(TAG, "Reconnect attempt %d/%d (scan first)",
             s_reconnect_attempts, WIFI_RECONNECT_MAX_ATTEMPTS);

    /* Load the known-networks list and rotate through it. */
    wifi_credentials_t list[CONFIG_STORE_WIFI_SLOT_COUNT];
    uint8_t list_count = 0;
    (void) config_store_load_wifi_list(list, &list_count);

    wifi_credentials_t chosen = {0};
    if (list_count > 0) {
        if (s_current_wifi_slot >= list_count) s_current_wifi_slot = 0;
        chosen = list[s_current_wifi_slot];
        ESP_LOGI(TAG, "Trying known network %u/%u: '%s'",
                 (unsigned)(s_current_wifi_slot + 1), (unsigned)list_count, chosen.ssid);
        /* Advance for next attempt — wrap around */
        s_current_wifi_slot = (uint8_t)((s_current_wifi_slot + 1) % list_count);
    } else {
        /* Legacy single-cred fallback (empty list or pre-migration NVS) */
        (void) config_store_load_wifi(&chosen);
    }

    if (chosen.ssid[0] != '\0') {
        s_auto_connect_pending = true;
        strncpy(s_auto_ssid, chosen.ssid, sizeof(s_auto_ssid) - 1);
        s_auto_ssid[sizeof(s_auto_ssid) - 1] = '\0';
        strncpy(s_auto_pass, chosen.password, sizeof(s_auto_pass) - 1);
        s_auto_pass[sizeof(s_auto_pass) - 1] = '\0';
        wifi_manager_scan();
    } else {
        s_should_reconnect = false;
        _set_state(WIFI_MGR_STATE_FAILED);
    }
}

static void _reconnect_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    lv_async_call(_deferred_reconnect, NULL);
}

static void _schedule_reconnect(void)
{
    if (s_reconnect_attempts >= WIFI_RECONNECT_MAX_ATTEMPTS) {
        ESP_LOGW(TAG, "Max reconnect attempts reached (%d) — giving up",
                 WIFI_RECONNECT_MAX_ATTEMPTS);
        s_should_reconnect = false;
        _set_state(WIFI_MGR_STATE_FAILED);
        return;
    }

    int delay = WIFI_RECONNECT_BASE_DELAY_MS << s_reconnect_attempts;
    if (delay > WIFI_RECONNECT_MAX_DELAY_MS) {
        delay = WIFI_RECONNECT_MAX_DELAY_MS;
    }

    ESP_LOGI(TAG, "Scheduling reconnect in %d ms (attempt %d/%d)",
             delay, s_reconnect_attempts + 1, WIFI_RECONNECT_MAX_ATTEMPTS);

    if (!s_reconnect_timer) {
        s_reconnect_timer = xTimerCreate("wifi_reconn", pdMS_TO_TICKS(delay),
                                          pdFALSE, NULL, _reconnect_timer_cb);
        if (!s_reconnect_timer) {
            ESP_LOGE(TAG, "Failed to create reconnect timer");
            s_should_reconnect = false;
            _set_state(WIFI_MGR_STATE_FAILED);
            return;
        }
    } else {
        xTimerChangePeriod(s_reconnect_timer, pdMS_TO_TICKS(delay), 0);
    }

    if (xTimerStart(s_reconnect_timer, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start reconnect timer");
    }
}

static void _stop_reconnect(void)
{
    s_should_reconnect = false;
    s_reconnect_attempts = 0;
    if (s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
        xTimerDelete(s_reconnect_timer, 0);
        s_reconnect_timer = NULL;
    }
}

static void _build_ap_ssid(void)
{
    if (s_ap_ssid[0]) return;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "RDM7-%02X%02X", mac[4], mac[5]);
}

/* Sort scan results by RSSI descending (bubble sort, small N) */
static void _sort_scan_results(void)
{
    for (uint16_t i = 0; i < s_scan_count; i++) {
        for (uint16_t j = i + 1; j < s_scan_count; j++) {
            if (s_scan_results[j].rssi > s_scan_results[i].rssi) {
                wifi_mgr_ap_record_t tmp = s_scan_results[i];
                s_scan_results[i] = s_scan_results[j];
                s_scan_results[j] = tmp;
            }
        }
    }
}

/* Process scan results from the WiFi driver */
static void _process_scan_results(void)
{
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);

    if (ap_num > DEFAULT_SCAN_LIST_SIZE) {
        ap_num = DEFAULT_SCAN_LIST_SIZE;
    }

    s_scan_count = 0;

    if (ap_num == 0) {
        ESP_LOGI(TAG, "Scan complete: no networks found");
        if (s_auto_connect_pending) {
            s_auto_connect_pending = false;
            memset(s_auto_pass, 0, sizeof(s_auto_pass));
            ESP_LOGW(TAG, "Auto-connect: no networks found");
            _set_state(WIFI_MGR_STATE_FAILED);
        } else {
            _set_state(WIFI_MGR_STATE_IDLE);
        }
        return;
    }

    wifi_ap_record_t *ap_info = malloc(sizeof(wifi_ap_record_t) * ap_num);
    if (!ap_info) {
        ESP_LOGE(TAG, "Failed to allocate scan result buffer");
        _set_state(WIFI_MGR_STATE_IDLE);
        return;
    }

    esp_err_t err = esp_wifi_scan_get_ap_records(&ap_num, ap_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get scan records: %s", esp_err_to_name(err));
        free(ap_info);
        _set_state(WIFI_MGR_STATE_IDLE);
        return;
    }

    for (uint16_t i = 0; i < ap_num; i++) {
        strncpy(s_scan_results[i].ssid, (const char *)ap_info[i].ssid, 32);
        s_scan_results[i].ssid[32] = '\0';
        s_scan_results[i].rssi = ap_info[i].rssi;
        s_scan_results[i].auth_mode = (uint8_t)ap_info[i].authmode;
    }
    s_scan_count = ap_num;

    free(ap_info);

    _sort_scan_results();

    ESP_LOGI(TAG, "Scan complete: %u networks found", s_scan_count);

    /* Auto-connect: if a scan was triggered for auto-connect, try to find the SSID */
    if (s_auto_connect_pending) {
        s_auto_connect_pending = false;
        bool found = false;
        for (uint16_t i = 0; i < s_scan_count; i++) {
            if (strcmp(s_scan_results[i].ssid, s_auto_ssid) == 0) {
                found = true;
                break;
            }
        }
        if (found) {
            ESP_LOGI(TAG, "Auto-connect: '%s' found in scan, connecting", s_auto_ssid);
            wifi_manager_connect(s_auto_ssid, s_auto_pass);
        } else {
            ESP_LOGW(TAG, "Auto-connect: '%s' not found in scan results", s_auto_ssid);
            _set_state(WIFI_MGR_STATE_FAILED);
        }
        memset(s_auto_pass, 0, sizeof(s_auto_pass));
        return;
    }

    /* Restore state: if we were connected before scanning, stay CONNECTED */
    if (s_connected_ssid[0] != '\0') {
        _set_state(WIFI_MGR_STATE_CONNECTED);
    } else {
        _set_state(WIFI_MGR_STATE_IDLE);
    }

    /* Scan-only path: if we upgraded to APSTA just to run this scan (AP was
     * on, no saved STA, user opened the WiFi settings to see available
     * networks), the STA half keeps spamming `Haven't to connect to a
     * suitable AP now!` because it has no target. Drop back to AP-only so
     * the radio can serve the hotspot reliably. If the user picks a network
     * next, `wifi_manager_connect` will re-upgrade. */
    if (s_ap_enabled && s_connected_ssid[0] == '\0' && !_has_saved_sta_creds()) {
        wifi_mode_t m = WIFI_MODE_NULL;
        esp_wifi_get_mode(&m);
        if (m == WIFI_MODE_APSTA) {
            ESP_LOGI(TAG, "Scan complete — downgrading APSTA → AP-only (no saved STA)");
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
    }
}

/* ── ESP event handler ────────────────────────────────────────────────── */

static void _wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started");
            break;

        case WIFI_EVENT_STA_CONNECTED: {
            wifi_event_sta_connected_t *ev = (wifi_event_sta_connected_t *)event_data;
            memset(s_connected_ssid, 0, sizeof(s_connected_ssid));
            /* Use the pending SSID we stored before calling esp_wifi_connect(),
               because the event struct SSID may not be null-terminated. */
            if (s_pending_ssid[0] != '\0') {
                strncpy(s_connected_ssid, s_pending_ssid, sizeof(s_connected_ssid) - 1);
            } else {
                /* Fallback: copy from event, length-limited */
                size_t len = ev->ssid_len;
                if (len > 32) len = 32;
                memcpy(s_connected_ssid, ev->ssid, len);
                s_connected_ssid[len] = '\0';
            }
            ESP_LOGI(TAG, "Associated with '%s' — waiting for IP", s_connected_ssid);

            /* L2 association succeeded — reset reconnect state */
            _stop_reconnect();

            /* Save credentials to NVS — legacy single-cred AND to the
               multi-SSID list (#19) so future reconnect rotations see it. */
            wifi_credentials_t creds = {0};
            strncpy(creds.ssid, s_connected_ssid, sizeof(creds.ssid) - 1);
            strncpy(creds.password, s_pending_pass, sizeof(creds.password) - 1);
            creds.auto_connect = true;
            esp_err_t save_err = config_store_save_wifi(&creds);
            if (save_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to save WiFi creds: %s", esp_err_to_name(save_err));
            }
            esp_err_t add_err = config_store_add_wifi(&creds);
            if (add_err != ESP_OK && add_err != ESP_ERR_INVALID_ARG) {
                ESP_LOGW(TAG, "Failed to add WiFi to known list: %s", esp_err_to_name(add_err));
            }

            /* Don't set CONNECTED yet — wait for IP_EVENT_STA_GOT_IP.
             * Keep CONNECTING state so the UI shows "Connecting..." */
            break;
        }

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *ev = (wifi_event_sta_disconnected_t *)event_data;
            ESP_LOGI(TAG, "Disconnected (reason: %d)", ev->reason);
            bool was_connected = (s_connected_ssid[0] != '\0');
            s_connected_ssid[0] = '\0';
            memset(s_sta_ip, 0, sizeof(s_sta_ip));

            if (s_user_disconnect) {
                /* User-initiated disconnect — do NOT auto-reconnect */
                s_user_disconnect = false;
                _set_state(s_ap_enabled ? WIFI_MGR_STATE_AP_ONLY
                                        : WIFI_MGR_STATE_IDLE);
            } else if (s_should_reconnect) {
                /* Already in reconnect flow — schedule next attempt */
                _set_state(s_ap_enabled ? WIFI_MGR_STATE_AP_ONLY
                                        : WIFI_MGR_STATE_IDLE);
                _schedule_reconnect();
            } else if (s_state == WIFI_MGR_STATE_CONNECTING || was_connected) {
                /* Lost connection or initial connect failed — start reconnect
                 * if we have saved credentials to retry with. */
                wifi_credentials_t creds = {0};
                if (config_store_load_wifi(&creds) == ESP_OK &&
                    creds.ssid[0] != '\0') {
                    s_should_reconnect = true;
                    s_reconnect_attempts = 0;
                    _set_state(s_ap_enabled ? WIFI_MGR_STATE_AP_ONLY
                                            : WIFI_MGR_STATE_IDLE);
                    _schedule_reconnect();
                } else {
                    _set_state(WIFI_MGR_STATE_FAILED);
                }
            } else {
                _set_state(s_ap_enabled ? WIFI_MGR_STATE_AP_ONLY
                                        : WIFI_MGR_STATE_IDLE);
            }
            break;
        }

        case WIFI_EVENT_SCAN_DONE:
            _process_scan_results();
            break;

        case WIFI_EVENT_AP_START:
            /* SoftAP is up. Clients reach the editor at 192.168.4.1 (or via
               the QR code from Device Settings); mDNS was removed in 2026-04. */
            break;

        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ev = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "AP: station " MACSTR " joined (aid=%d)",
                     MAC2STR(ev->mac), ev->aid);
            break;
        }

        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ev = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "AP: station " MACSTR " left (aid=%d)",
                     MAC2STR(ev->mac), ev->aid);
            break;
        }

        default:
            break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
            snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
            ESP_LOGI(TAG, "Got IP: %s", s_sta_ip);
            _set_state(WIFI_MGR_STATE_CONNECTED);
            /* Kick off NTP now that we have a route. Idempotent — every
             * subsequent got-IP is a no-op. Required by Share Raw CAN
             * uploads which sign an HMAC over a unix timestamp. */
            initialize_sntp();
        } else if (event_id == IP_EVENT_STA_LOST_IP) {
            ESP_LOGW(TAG, "Lost IP address");
            memset(s_sta_ip, 0, sizeof(s_sta_ip));
        }
    }
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

/* Does the device have at least one saved STA SSID in NVS?
 *
 * When false, we should NOT start the STA interface — ESP-IDF's WiFi stack
 * will otherwise spam `Haven't to connect to a suitable AP now!` every 500ms
 * and steal radio time from the AP, making the hotspot unreliable (single
 * radio, APSTA concurrent mode). Used to pick AP-only vs APSTA at boot. */
static bool _has_saved_sta_creds(void)
{
    wifi_credentials_t list[CONFIG_STORE_WIFI_SLOT_COUNT] = {0};
    uint8_t list_count = 0;
    if (config_store_load_wifi_list(list, &list_count) == ESP_OK && list_count > 0) {
        for (uint8_t i = 0; i < list_count; ++i) {
            if (list[i].ssid[0] != '\0') return true;
        }
    }
    wifi_credentials_t one = {0};
    if (config_store_load_wifi(&one) == ESP_OK && one.ssid[0] != '\0') return true;
    return false;
}

void wifi_manager_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return;
    }

    ESP_LOGI(TAG, "Initializing");

    /* Create TCP/IP stack + default event loop (tolerate repeated calls) */
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(ret);
    }

    /* Create netifs */
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    /* Register event handlers */
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_LOST_IP, &_wifi_event_handler, NULL));

    _build_ap_ssid();

    s_initialized = true;
    s_state       = WIFI_MGR_STATE_OFF;

    ESP_LOGI(TAG, "Initialized (AP SSID will be '%s')", s_ap_ssid);
}

void wifi_manager_start(void)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized — call wifi_manager_init() first");
        return;
    }
    if (s_started) {
        ESP_LOGW(TAG, "Already started");
        return;
    }

    ESP_LOGI(TAG, "Starting WiFi radio");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Load AP enabled state from the boot-config store — single source of
     * truth for the at-boot AP preference.  rdm_ap_config_t (NS_WIFI_AP)
     * only carries the password; its legacy "enabled" field is ignored here
     * so the two stores can't diverge and cause unexpected mode decisions. */
    wifi_boot_config_t boot_cfg = {0};
    config_store_load_wifi_boot(&boot_cfg);
    s_ap_enabled = boot_cfg.ap_enabled;

    /* Pick mode. APSTA costs the AP radio time — the STA half keeps
     * probing/retrying even when it has no target, which spams
     * `Haven't to connect to a suitable AP now!` and drops AP frames.
     * Only enable STA if there's at least one saved SSID to connect to. */
    bool need_sta = _has_saved_sta_creds();
    wifi_mode_t start_mode;
    if (s_ap_enabled && need_sta)       start_mode = WIFI_MODE_APSTA;
    else if (s_ap_enabled)              start_mode = WIFI_MODE_AP;
    else                                start_mode = WIFI_MODE_STA;
    ESP_ERROR_CHECK(esp_wifi_set_mode(start_mode));
    ESP_LOGI(TAG, "Mode: %s (AP=%d saved_sta=%d)",
             start_mode == WIFI_MODE_APSTA ? "APSTA" :
             start_mode == WIFI_MODE_AP    ? "AP-only" : "STA-only",
             s_ap_enabled, need_sta);

    /* Configure AP if enabled */
    if (s_ap_enabled) {
        /* Load password from dedicated AP config namespace */
        rdm_ap_config_t ap_cfg = {0};
        config_store_load_ap_config(&ap_cfg);

        wifi_config_t ap_wifi_cfg = {0};
        strncpy((char *)ap_wifi_cfg.ap.ssid, s_ap_ssid, sizeof(ap_wifi_cfg.ap.ssid) - 1);
        ap_wifi_cfg.ap.ssid_len      = strlen(s_ap_ssid);
        /* Channel 11 instead of 1 — in this user's RF environment channel 1
         * was too congested to complete 802.11 auth reliably with phones
         * (reason=15 timeouts). If other users report issues on 11, a
         * boot-time channel scan would be a cleaner fix. */
        ap_wifi_cfg.ap.channel        = 11;
        ap_wifi_cfg.ap.max_connection = 4;

        if (strlen(ap_cfg.password) >= 8) {
            strncpy((char *)ap_wifi_cfg.ap.password, ap_cfg.password,
                    sizeof(ap_wifi_cfg.ap.password) - 1);
            ap_wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            ap_wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_wifi_cfg));
        ESP_LOGI(TAG, "AP configured: SSID='%s' auth=%s",
                 s_ap_ssid,
                 ap_wifi_cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    /* Disable power save — dash display is always powered, and PS mode
     * causes intermittent disconnects and increased latency. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Force HT20 on both interfaces. Espressif's guidance for concurrent-
     * mode and phone-facing APs is HT20 — HT40 often fails the 4-way WPA2
     * handshake on weak or picky clients (reason=15 timeouts). Costs a
     * little throughput but makes the hotspot reliable. */
    if (s_ap_enabled) esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);

    s_started = true;

    if (s_ap_enabled) {
        ESP_LOGI(TAG, "AP started — connect to '%s', browse http://192.168.4.1",
                 s_ap_ssid);
        _set_state(WIFI_MGR_STATE_AP_ONLY);
    } else {
        _set_state(WIFI_MGR_STATE_IDLE);
    }
}

void wifi_manager_stop(void)
{
    if (!s_started) {
        ESP_LOGW(TAG, "Not started");
        return;
    }

    ESP_LOGI(TAG, "Stopping WiFi radio");

    _stop_reconnect();

    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }
    err = esp_wifi_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_deinit: %s", esp_err_to_name(err));
    }

    s_started           = false;
    s_connected_ssid[0] = '\0';
    memset(s_sta_ip, 0, sizeof(s_sta_ip));
    s_scan_count = 0;
    s_auto_connect_pending = false;
    memset(s_auto_pass, 0, sizeof(s_auto_pass));

    _set_state(WIFI_MGR_STATE_OFF);
}

bool wifi_manager_is_started(void)
{
    return s_started;
}

/* ── STA operations ───────────────────────────────────────────────────── */

void wifi_manager_scan(void)
{
    if (!s_started) {
        ESP_LOGW(TAG, "WiFi not started — cannot scan");
        return;
    }

    if (s_state == WIFI_MGR_STATE_SCANNING) {
        ESP_LOGD(TAG, "Scan already in progress");
        return;
    }

    /* Scan requires the STA interface. If we're AP-only, upgrade. */
    wifi_mode_t cur_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&cur_mode);
    if (cur_mode == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "Upgrading AP-only → APSTA for scan");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    }

    ESP_LOGI(TAG, "Starting scan (non-blocking)");

    /* Active scan, 120 ms min / 1500 ms max per channel. Old 100/500 timing
     * regularly missed phone hotspots whose beacon interval landed outside the
     * window — Android tethering APs in particular advertise less aggressively
     * than home routers. ESP32-S3 is 2.4 GHz only; if a network never appears
     * regardless, check the phone's hotspot band setting (must be 2.4 GHz). */
    wifi_scan_config_t scan_cfg = {
        .ssid        = NULL,
        .bssid       = NULL,
        .channel     = 0,
        .show_hidden = false,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 120,
        .scan_time.active.max = 1500
    };

    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan start failed: %s", esp_err_to_name(err));
        return;
    }

    _set_state(WIFI_MGR_STATE_SCANNING);
}

void wifi_manager_connect(const char *ssid, const char *password)
{
    if (!s_started) {
        ESP_LOGE(TAG, "WiFi not started — cannot connect");
        return;
    }
    if (!ssid || ssid[0] == '\0') {
        ESP_LOGE(TAG, "SSID is empty");
        return;
    }

    /* Cancel pending auto-connect. Preserve reconnect state if we're
     * being called from within the reconnect flow (scan found SSID). */
    if (!s_should_reconnect) {
        _stop_reconnect();
    }
    s_auto_connect_pending = false;

    ESP_LOGI(TAG, "Connecting to '%s'", ssid);

    /* If we booted in AP-only mode (no saved credentials at boot) and the
     * user is now trying to connect to a network, upgrade the mode so the
     * STA interface actually exists. Otherwise esp_wifi_connect() would
     * silently no-op. */
    wifi_mode_t cur_mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&cur_mode);
    if (cur_mode == WIFI_MODE_AP) {
        ESP_LOGI(TAG, "Upgrading AP-only → APSTA for STA connect");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    } else if (cur_mode == WIFI_MODE_NULL) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    }

    /* Store pending credentials for use in event handler */
    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    s_pending_ssid[sizeof(s_pending_ssid) - 1] = '\0';

    if (password && password[0] != '\0') {
        strncpy(s_pending_pass, password, sizeof(s_pending_pass) - 1);
        s_pending_pass[sizeof(s_pending_pass) - 1] = '\0';
    } else {
        s_pending_pass[0] = '\0';
    }

    /* Configure STA */
    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    if (password && password[0] != '\0') {
        strncpy((char *)wifi_cfg.sta.password, password,
                sizeof(wifi_cfg.sta.password) - 1);
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set STA config: %s", esp_err_to_name(err));
        _set_state(WIFI_MGR_STATE_FAILED);
        return;
    }

    /* Disconnect first (safe even if not connected) */
    esp_wifi_disconnect();

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        _set_state(WIFI_MGR_STATE_FAILED);
        return;
    }

    _set_state(WIFI_MGR_STATE_CONNECTING);
}

void wifi_manager_disconnect(void)
{
    if (!s_started) return;

    ESP_LOGI(TAG, "Disconnecting STA");
    _stop_reconnect();
    s_user_disconnect = true;
    memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
    memset(s_pending_pass, 0, sizeof(s_pending_pass));
    esp_wifi_disconnect();
    /* State update happens in the event handler */
}

void wifi_manager_forget(void)
{
    ESP_LOGI(TAG, "Forgetting saved network");

    _stop_reconnect();  /* No creds to reconnect to */
    s_auto_connect_pending = false;
    memset(s_auto_pass, 0, sizeof(s_auto_pass));
    config_store_clear_wifi();
    s_connected_ssid[0] = '\0';
    memset(s_sta_ip, 0, sizeof(s_sta_ip));
    memset(s_pending_ssid, 0, sizeof(s_pending_ssid));
    memset(s_pending_pass, 0, sizeof(s_pending_pass));

    if (s_started) {
        esp_wifi_disconnect();
    }

    _set_state(s_ap_enabled ? WIFI_MGR_STATE_AP_ONLY : WIFI_MGR_STATE_IDLE);
}

void wifi_manager_auto_connect(void)
{
    /* If STA interface isn't part of the current mode (AP-only at boot when
     * no SSIDs were saved), scanning is both useless and noisy. Skip it. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    bool sta_up = (mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA);

    wifi_credentials_t creds = {0};
    if (config_store_load_wifi(&creds) != ESP_OK) {
        ESP_LOGI(TAG, "No saved WiFi credentials — %s", sta_up ? "scanning only" : "AP-only, skipping scan");
        if (sta_up) wifi_manager_scan();
        return;
    }

    if (!creds.auto_connect || creds.ssid[0] == '\0') {
        ESP_LOGI(TAG, "Auto-connect disabled or SSID empty — %s", sta_up ? "scanning only" : "AP-only, skipping scan");
        if (sta_up) wifi_manager_scan();
        return;
    }

    ESP_LOGI(TAG, "Auto-connect: scanning for '%s'", creds.ssid);

    /* Store credentials — _process_scan_results will connect if found */
    s_auto_connect_pending = true;
    strncpy(s_auto_ssid, creds.ssid, sizeof(s_auto_ssid) - 1);
    s_auto_ssid[sizeof(s_auto_ssid) - 1] = '\0';
    strncpy(s_auto_pass, creds.password, sizeof(s_auto_pass) - 1);
    s_auto_pass[sizeof(s_auto_pass) - 1] = '\0';

    /* Don't set state before calling scan — wifi_manager_scan() checks
     * for SCANNING state and would reject the call. */
    wifi_manager_scan();
}

/* ── AP operations ────────────────────────────────────────────────────── */

void wifi_manager_enable_ap(bool enable)
{
    if (!s_started) {
        ESP_LOGW(TAG, "WiFi not started — AP setting cached for next start");
        s_ap_enabled = enable;
        return;
    }

    if (enable == s_ap_enabled) return;

    s_ap_enabled = enable;

    if (enable) {
        /* AP-only when no saved STA — see _has_saved_sta_creds comment */
        wifi_mode_t new_mode = _has_saved_sta_creds() ? WIFI_MODE_APSTA : WIFI_MODE_AP;
        ESP_ERROR_CHECK(esp_wifi_set_mode(new_mode));

        /* Configure AP */
        rdm_ap_config_t ap_cfg;
        config_store_load_ap_config(&ap_cfg);

        wifi_config_t ap_wifi_cfg = {0};
        strncpy((char *)ap_wifi_cfg.ap.ssid, s_ap_ssid, sizeof(ap_wifi_cfg.ap.ssid) - 1);
        ap_wifi_cfg.ap.ssid_len       = strlen(s_ap_ssid);
        /* Same channel 11 as boot path — see wifi_manager_start for rationale. */
        ap_wifi_cfg.ap.channel        = 11;
        ap_wifi_cfg.ap.max_connection  = 4;

        if (strlen(ap_cfg.password) >= 8) {
            strncpy((char *)ap_wifi_cfg.ap.password, ap_cfg.password,
                    sizeof(ap_wifi_cfg.ap.password) - 1);
            ap_wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            ap_wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
        }

        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_wifi_cfg));
        ESP_LOGI(TAG, "AP enabled: SSID='%s'", s_ap_ssid);
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_LOGI(TAG, "AP disabled");
    }
}

bool wifi_manager_is_ap_enabled(void)
{
    return s_ap_enabled;
}

void wifi_manager_set_ap_password(const char *password)
{
    if (!s_started || !s_ap_enabled) return;

    /* Reconfigure the AP with the new password */
    wifi_config_t ap_wifi_cfg = {0};
    strncpy((char *)ap_wifi_cfg.ap.ssid, s_ap_ssid, sizeof(ap_wifi_cfg.ap.ssid) - 1);
    ap_wifi_cfg.ap.ssid_len       = strlen(s_ap_ssid);
    ap_wifi_cfg.ap.channel        = 11;
    ap_wifi_cfg.ap.max_connection  = 4;

    if (password && strlen(password) >= 8) {
        strncpy((char *)ap_wifi_cfg.ap.password, password,
                sizeof(ap_wifi_cfg.ap.password) - 1);
        ap_wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_AP, &ap_wifi_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "AP password updated (auth=%s)",
                 ap_wifi_cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");
    } else {
        ESP_LOGE(TAG, "Failed to update AP config: %s", esp_err_to_name(err));
    }
}

/* ── State queries ────────────────────────────────────────────────────── */

wifi_mgr_state_t wifi_manager_get_state(void)
{
    return s_state;
}

const char *wifi_manager_get_connected_ssid(void)
{
    return s_connected_ssid[0] ? s_connected_ssid : NULL;
}

const char *wifi_manager_get_ap_ssid(void)
{
    _build_ap_ssid();
    return s_ap_ssid;
}

const char *wifi_manager_get_sta_ip(void)
{
    return s_sta_ip[0] ? s_sta_ip : NULL;
}

const char *wifi_manager_get_ap_ip(void)
{
    return "192.168.4.1";
}

/* ── Scan results ─────────────────────────────────────────────────────── */

uint16_t wifi_manager_get_scan_results(wifi_mgr_ap_record_t *out, uint16_t max)
{
    if (!out || max == 0) return 0;

    uint16_t count = (s_scan_count < max) ? s_scan_count : max;
    memcpy(out, s_scan_results, count * sizeof(wifi_mgr_ap_record_t));
    return count;
}

/* ── Event subscription ───────────────────────────────────────────────── */

void wifi_manager_set_event_cb(wifi_mgr_event_cb_t cb, void *user_data)
{
    s_event_cb = cb;
    s_event_ud = user_data;
}
