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

// 503 + Retry-After for transient conditions (LVGL lock busy, a heavy build
// already in flight, momentary OOM under load). Semantically "retry shortly",
// which client poll/edit loops honour — unlike a 500, which surfaces an error.
esp_err_t web_server_send_busy(httpd_req_t *req);

// Drain up to max_drain bytes of an unread request body so a subsequent error
// response is delivered cleanly (a FIN, not a TCP RST). Call before rejecting
// an oversize upload by content_len, or the client sees a connection reset
// instead of the 4xx. Bounded so a huge body can't tie the worker up.
void web_server_drain_body(httpd_req_t *req, size_t max_drain);

// Serialize + send a cJSON object as application/json (with CORS), then free
// it. ALWAYS consumes root. Sends a 500 instead of crashing if the serialize
// OOMs. Forward-declared with a cJSON struct tag so callers needn't include
// cJSON.h just for the prototype.
struct cJSON;
esp_err_t web_server_send_json(httpd_req_t *req, struct cJSON *root);

// Path-traversal guards for user-supplied file/layout names.
// web_server_name_is_safe: no dots, no slashes, no control chars.
// web_server_filename_is_safe: allows one dot (extension); rejects "..".
bool web_server_name_is_safe(const char *name);
bool web_server_filename_is_safe(const char *name);

// In-place percent-decode of a query-string value: "%XX" -> byte, "+" -> space.
// httpd_query_key_value() returns the raw encoded value; call this before the
// safety check + path build so names with spaces (e.g. "Fugaz One") resolve.
void web_server_url_decode(char *name);

// Receive a (small, JSON) POST body into buf, looping until content_len is
// consumed — a single httpd_req_recv() only returns the first TCP segment,
// so split bodies used to cause intermittent 400s. Null-terminates. On any
// failure (no body, socket error, body >= cap) sends the 4xx itself and
// returns ESP_FAIL — callers just `return ESP_FAIL;`. Not for file uploads;
// those stream chunks instead of buffering.
esp_err_t web_server_recv_body(httpd_req_t *req, char *buf, size_t cap);

// Same receive loop but never touches the response: returns body length, or
// -1 on empty/oversize/socket error. For handlers with an OPTIONAL body or
// their own error envelope.
int web_server_recv_body_raw(httpd_req_t *req, char *buf, size_t cap);

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
void web_server_channels_register(httpd_handle_t server);
void web_server_lap_register(httpd_handle_t server);
void web_server_test_register(httpd_handle_t server);

/* Rebuild + load the active dashboard screen immediately. LVGL-task only
 * (all lv_* calls). Defined in web_server_layout.c. */
void web_server_rebuild_active_screen(void);

#ifdef __cplusplus
}
#endif
