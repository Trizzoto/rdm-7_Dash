/**
 * serial_commands_capture.c — screenshot serial JSON-RPC handler.
 *
 * Methods: screenshot.
 */
#include "serial_commands_internal.h"
#include "uart_protocol.h"
#include "serial_protocol.h"

#include "cJSON.h"
#include "system/remote_touch.h"
#include "can/obd2.h"
#include "can/can_sim.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "display_capture.h"

#include <string.h>

static const char *TAG = "serial_cmd";

/* ── screenshot ──────────────────────────────────────────────────────────── */

/* JPEG, not the raw framebuffer.
 *
 * The old handler answered {"size":768000,"format":"bmp"} and then sent
 * nothing, every time, on every build — three separate reasons at once:
 *   - it was never a BMP, just raw RGB565 with no header;
 *   - 768001 bytes cannot come from malloc() on a chip with 512 KB of
 *     internal RAM; and
 *   - even allocated, uart_protocol_send_frame rejects anything over
 *     UART_PROTO_MAX_PAYLOAD (64 KB), and its return value was discarded.
 * The caller was left waiting on a size it had just been promised.
 *
 * A full-res JPEG is 25-40 KB, so it fits the protocol as it stands and needs
 * no chunked-download RPC. The size is still checked before sending, because
 * a busy screen encodes larger and "silently sends nothing" is the exact
 * failure this is replacing. */
/* LVGL mutex (defined in main.c) */
extern bool rdm_lvgl_lock(int timeout_ms);
extern void rdm_lvgl_unlock(void);

/* ── touch ───────────────────────────────────────────────────────────────
 * The USB twin of POST /api/touch: {"enabled"?:bool, "x","y", "state":
 * "down"|"move"|"up"}. Over WiFi this is how Studio's CONTROL mode drives the
 * glass; a dash with no network had no way to be tapped at all, so the
 * screens only the touchscreen reaches (the setup wizard) could be looked at
 * over a cable — the screenshot RPC — but not used. Replies {enabled}. */
void _handle_touch(int id, cJSON *params)
{
    cJSON *en = params ? cJSON_GetObjectItemCaseSensitive(params, "enabled") : NULL;
    if (cJSON_IsBool(en)) remote_touch_set_enabled(cJSON_IsTrue(en));

    cJSON *x_js = params ? cJSON_GetObjectItemCaseSensitive(params, "x") : NULL;
    cJSON *y_js = params ? cJSON_GetObjectItemCaseSensitive(params, "y") : NULL;
    cJSON *s_js = params ? cJSON_GetObjectItemCaseSensitive(params, "state") : NULL;
    if (cJSON_IsNumber(x_js) && cJSON_IsNumber(y_js) && cJSON_IsString(s_js)) {
        const char *s = s_js->valuestring;
        bool pressed = (strcmp(s, "down") == 0) || (strcmp(s, "move") == 0);
        remote_touch_set((int16_t)x_js->valueint, (int16_t)y_js->valueint, pressed);
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", remote_touch_is_enabled());
    _send_response(id, r, NULL);
}

/* ── obd2.sim ────────────────────────────────────────────────────────────
 * The USB twin of /api/obd2/sim: {"on"?:bool}. Without "on" it only reports.
 * The bench virtual ECU answers discovery and polling, so OBD2 setup can be
 * proven with no car — and, now, with no network either. */
void _handle_obd2_sim(int id, cJSON *params)
{
    cJSON *on = params ? cJSON_GetObjectItemCaseSensitive(params, "on") : NULL;
    if (!rdm_lvgl_lock(1200)) {
        _send_error(id, "LVGL busy");
        return;
    }
    if (cJSON_IsBool(on)) obd2_sim_set_enabled(cJSON_IsTrue(on));
    bool sim_on = obd2_sim_enabled();
    rdm_lvgl_unlock();
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "sim", sim_on);
    _send_response(id, r, NULL);
}

/* can.sim {"on":true,"ecu":"Haltech","version":"Nexus"} — a bench ECU that
 * broadcasts that preset's CAN stream into the receive path (can_sim.h). */
void _handle_can_sim(int id, cJSON *params)
{
    cJSON *on = params ? cJSON_GetObjectItemCaseSensitive(params, "on") : NULL;
    if (cJSON_IsBool(on)) {
        if (cJSON_IsTrue(on)) {
            cJSON *ecu = cJSON_GetObjectItemCaseSensitive(params, "ecu");
            cJSON *ver = cJSON_GetObjectItemCaseSensitive(params, "version");
            if (!can_sim_start(cJSON_IsString(ecu) ? ecu->valuestring : "Haltech",
                               cJSON_IsString(ver) ? ver->valuestring : "Nexus")) {
                _send_error(id, "No CAN preset by that ECU and version");
                return;
            }
        } else {
            can_sim_stop();
        }
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "sim", can_sim_active());
    cJSON_AddStringToObject(r, "ecu", can_sim_ecu());
    cJSON_AddStringToObject(r, "version", can_sim_version());
    _send_response(id, r, NULL);
}

void _handle_screenshot(int id, cJSON *params)
{
    (void)params;
    uint8_t *buf = NULL;
    size_t size = 0;

    esp_err_t err = display_capture_screenshot_jpeg(80, true, false, &buf, &size);
    if (err != ESP_OK || !buf) {
        _send_error(id, "Screenshot failed");
        return;
    }

    if (size + 1 > UART_PROTO_MAX_PAYLOAD) {
        ESP_LOGE(TAG, "screenshot is %u bytes, over the %d-byte frame limit",
                 (unsigned)size, UART_PROTO_MAX_PAYLOAD);
        display_capture_free_buffer(buf);
        _send_error(id, "Screenshot too large for one frame");
        return;
    }

    /* Size and shape first; the image follows in one binary frame. Width and
     * height are stated because the caller has no other way to know the panel. */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "size", size);
    cJSON_AddStringToObject(r, "format", "jpeg");
    cJSON_AddNumberToObject(r, "width", CAPTURE_WIDTH);
    cJSON_AddNumberToObject(r, "height", CAPTURE_HEIGHT);
    _send_response(id, r, NULL);

    /* Prefix: type tag (0x01) + JPEG bytes. */
    size_t frame_len = 1 + size;
    uint8_t *frame = heap_caps_malloc(frame_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) frame = malloc(frame_len);
    if (frame) {
        frame[0] = UART_PAYLOAD_BINARY;
        memcpy(frame + 1, buf, size);
        esp_err_t serr = serial_protocol_send_frame(frame, frame_len);
        if (serr != ESP_OK)
            ESP_LOGE(TAG, "screenshot frame not sent: %s", esp_err_to_name(serr));
        free(frame);
    } else {
        ESP_LOGE(TAG, "OOM for screenshot frame (%u bytes)", (unsigned)frame_len);
    }
    display_capture_free_buffer(buf);
}
