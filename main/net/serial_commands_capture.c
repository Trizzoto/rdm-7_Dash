/**
 * serial_commands_capture.c — screenshot serial JSON-RPC handler.
 *
 * Methods: screenshot.
 */
#include "serial_commands_internal.h"
#include "uart_protocol.h"
#include "serial_protocol.h"

#include "cJSON.h"
#include "esp_log.h"
#include "display_capture.h"

#include <string.h>

static const char *TAG = "serial_cmd";

/* ── screenshot ──────────────────────────────────────────────────────────── */

void _handle_screenshot(int id, cJSON *params)
{
    (void)params;
    uint8_t *buf = NULL;
    size_t size = 0;

    esp_err_t err = display_capture_screenshot(&buf, &size);
    if (err != ESP_OK || !buf) {
        _send_error(id, "Screenshot failed");
        return;
    }

    /* Send size in JSON response — desktop will fetch binary via chunked read */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "size", size);
    cJSON_AddStringToObject(r, "format", "bmp");
    _send_response(id, r, NULL);

    /* Send the binary data as a binary frame */
    /* Prefix: type tag (0x01) + screenshot data */
    size_t frame_len = 1 + size;
    uint8_t *frame = malloc(frame_len);
    if (frame) {
        frame[0] = UART_PAYLOAD_BINARY;
        memcpy(frame + 1, buf, size);
        serial_protocol_send_frame(frame, frame_len);
        free(frame);
    } else {
        ESP_LOGE(TAG, "OOM for screenshot frame (%u bytes)", (unsigned)frame_len);
    }
    display_capture_free_buffer(buf);
}
