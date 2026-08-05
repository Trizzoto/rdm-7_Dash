/**
 * serial_commands_capture.c — screenshot serial JSON-RPC handler.
 *
 * Methods: screenshot.
 */
#include "serial_commands_internal.h"
#include "uart_protocol.h"
#include "serial_protocol.h"

#include "cJSON.h"
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
