#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// Shared infrastructure included by web_server.c and every web_server_<domain>.c.
// Do NOT include this from headers that are pulled in by non-web-server translation
// units — it drags in esp_http_server.h which is heavy.

#include <stdbool.h>
#include "esp_http_server.h"
#include "esp_log.h"

// LVGL lock helpers (defined in main.c).
extern bool rdm_lvgl_lock(int timeout_ms);
extern void rdm_lvgl_unlock(void);

// Registration tally — defined in web_server.c, incremented by every
// REGISTER_URI call including those in domain register() functions.
// web_server_start() resets both to zero before registering anything.
extern int web_server_uri_register_attempts;
extern int web_server_uri_register_failures;

// Count and log every registration attempt.  A non-zero failure count in the
// boot log means max_uri_handlers is too low; bump it in web_server_start().
#define REGISTER_URI(svr, uri_ptr) do { \
    web_server_uri_register_attempts++; \
    esp_err_t _reg_r = httpd_register_uri_handler((svr), (uri_ptr)); \
    if (_reg_r != ESP_OK) { \
        web_server_uri_register_failures++; \
        ESP_LOGE("web_server", "REGISTER_URI failed for %s (%s): %s", \
                 (uri_ptr)->uri, \
                 (uri_ptr)->method == HTTP_GET    ? "GET"     : \
                 (uri_ptr)->method == HTTP_POST   ? "POST"    : \
                 (uri_ptr)->method == HTTP_OPTIONS ? "OPTIONS" : "?", \
                 esp_err_to_name(_reg_r)); \
    } \
} while (0)

// 413 Payload Too Large JSON response (layout JSON exceeded LAYOUT_MAX_FILE_BYTES).
esp_err_t web_server_send_layout_too_large(httpd_req_t *req, size_t actual);

// Path-traversal guards for user-supplied file/layout names.
// web_server_name_is_safe: no dots, no slashes, no control chars.
// web_server_filename_is_safe: allows one dot (extension); rejects "..".
bool web_server_name_is_safe(const char *name);
bool web_server_filename_is_safe(const char *name);

// Domain register() entry points — called from web_server_start().
void web_server_captive_register(httpd_handle_t server);
void web_server_gear_register(httpd_handle_t server);
void web_server_touch_register(httpd_handle_t server);
void web_server_capture_register(httpd_handle_t server);
void web_server_signals_register(httpd_handle_t server);
void web_server_assets_register(httpd_handle_t server);
void web_server_wifi_register(httpd_handle_t server);
void web_server_ota_register(httpd_handle_t server);
void web_server_system_register(httpd_handle_t server);
void web_server_logger_register(httpd_handle_t server);
void web_server_layout_register(httpd_handle_t server);
void web_server_obd2_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
