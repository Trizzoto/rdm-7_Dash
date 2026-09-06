/**
 * frame_parser.c — STX/ETX/CRC16 frame parser, shared by every framed
 * serial transport (UART, USB CDC).
 *
 * Extracted from uart_protocol.c / usb_cdc_protocol.c, which used to
 * carry byte-for-byte identical copies of this state machine (only the
 * transport-level read/write calls differ between them).
 */
#include "frame_parser.h"
#include "uart_protocol.h"     /* UART_PROTO_STX/ETX/MAX_PAYLOAD, UART_PAYLOAD_*, uart_protocol_crc16() */
#include "serial_commands.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdlib.h>
#include <string.h>

void frame_parser_reset(frame_parser_t *p)
{
    if (p->payload) {
        free(p->payload);
        p->payload = NULL;
    }
    p->state = FRAME_STATE_IDLE;
    p->len_pos = 0;
    p->payload_len = 0;
    p->payload_pos = 0;
    p->crc_pos = 0;
}

static void _process_complete_frame(frame_parser_t *p, transport_id_t transport,
                                    const char *tag)
{
    uint16_t received_crc = (uint16_t)p->crc_buf[0] |
                            ((uint16_t)p->crc_buf[1] << 8);
    uint16_t computed_crc = uart_protocol_crc16(p->payload, p->payload_len);

    if (received_crc != computed_crc) {
        ESP_LOGW(tag, "CRC mismatch: got 0x%04X, expected 0x%04X",
                 received_crc, computed_crc);
        frame_parser_reset(p);
        return;
    }

    if (p->payload_len < 1) {
        ESP_LOGW(tag, "Empty payload");
        frame_parser_reset(p);
        return;
    }

    /* Set active transport before dispatching, so the response routes back
     * on the same transport that received the request. */
    serial_protocol_set_active(transport);

    uint8_t payload_type = p->payload[0];
    uint8_t *payload_data = p->payload + 1;
    size_t payload_data_len = p->payload_len - 1;

    if (payload_type == UART_PAYLOAD_JSON) {
        /* Null-terminate the JSON string */
        char *json_str = malloc(payload_data_len + 1);
        if (json_str) {
            memcpy(json_str, payload_data, payload_data_len);
            json_str[payload_data_len] = '\0';
            serial_commands_dispatch(json_str, payload_data_len);
            free(json_str);
        } else {
            ESP_LOGE(tag, "OOM for JSON dispatch (%u bytes)",
                     (unsigned)payload_data_len);
        }
    } else if (payload_type == UART_PAYLOAD_BINARY) {
        serial_commands_handle_binary(payload_data, payload_data_len);
    } else {
        ESP_LOGW(tag, "Unknown payload type: 0x%02X", payload_type);
    }

    /* Don't free payload here — frame_parser_reset handles it */
    frame_parser_reset(p);
}

void frame_parser_feed(frame_parser_t *p, const uint8_t *data, size_t len,
                       transport_id_t transport, const char *tag)
{
    /* Give up on a frame that went quiet.
     *
     * The length field is a promise, and this parser used to hold the sender
     * to it without limit: mid-payload it consumes every byte it is given
     * until the count is met. Over Bluetooth a frame can stop short — the
     * phone's stack refuses a chunk partway through, and the app re-sends the
     * whole frame from the start. Those fresh bytes, STX and all, were then
     * eaten as the tail of the half-frame, the CRC failed, and the re-send was
     * gone too. The requester saw "the dash did not answer".
     *
     * A frame in flight is a continuous stream: at the default 23-byte MTU a
     * chunk lands every connection interval, tens of milliseconds apart at
     * worst. Silence of FRAME_PARSER_STALL_MS mid-frame therefore means the
     * rest is not coming, and the right move is back to IDLE, hunting for the
     * next STX. The sender side waits longer than this before re-sending, so
     * the retry always meets a parser that is ready for it. */
    if (p->state != FRAME_STATE_IDLE && len > 0) {
        int64_t now = esp_timer_get_time();
        if (now - p->last_byte_us > (int64_t)FRAME_PARSER_STALL_MS * 1000) {
            ESP_LOGW(tag, "frame abandoned: %u of %u payload bytes, then %lld ms of silence",
                     (unsigned)p->payload_pos, (unsigned)p->payload_len,
                     (long long)((now - p->last_byte_us) / 1000));
            frame_parser_reset(p);
        }
    }
    if (len > 0) p->last_byte_us = esp_timer_get_time();

    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        switch (p->state) {
        case FRAME_STATE_IDLE:
            if (byte == UART_PROTO_STX) {
                p->state = FRAME_STATE_LENGTH;
                p->len_pos = 0;
            }
            /* Non-STX bytes (ESP_LOG output) are silently discarded */
            break;

        case FRAME_STATE_LENGTH:
            p->len_buf[p->len_pos++] = byte;
            if (p->len_pos == 4) {
                p->payload_len = (uint32_t)p->len_buf[0] |
                                 ((uint32_t)p->len_buf[1] << 8) |
                                 ((uint32_t)p->len_buf[2] << 16) |
                                 ((uint32_t)p->len_buf[3] << 24);
                if (p->payload_len == 0 ||
                    p->payload_len > UART_PROTO_MAX_PAYLOAD) {
                    ESP_LOGW(tag, "Invalid frame length: %u",
                             (unsigned)p->payload_len);
                    frame_parser_reset(p);
                    break;
                }
                p->payload = malloc(p->payload_len);
                if (!p->payload) {
                    ESP_LOGE(tag, "OOM for frame (%u bytes)",
                             (unsigned)p->payload_len);
                    frame_parser_reset(p);
                    break;
                }
                p->payload_pos = 0;
                p->state = FRAME_STATE_PAYLOAD;
            }
            break;

        case FRAME_STATE_PAYLOAD:
            p->payload[p->payload_pos++] = byte;
            if (p->payload_pos == p->payload_len) {
                p->state = FRAME_STATE_CRC;
                p->crc_pos = 0;
            }
            break;

        case FRAME_STATE_CRC:
            p->crc_buf[p->crc_pos++] = byte;
            if (p->crc_pos == 2) {
                p->state = FRAME_STATE_ETX;
            }
            break;

        case FRAME_STATE_ETX:
            if (byte == UART_PROTO_ETX) {
                _process_complete_frame(p, transport, tag);
            } else {
                ESP_LOGW(tag, "Expected ETX, got 0x%02X", byte);
                frame_parser_reset(p);
            }
            break;
        }
    }
}
