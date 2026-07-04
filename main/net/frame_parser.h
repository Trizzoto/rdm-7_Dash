/**
 * frame_parser.h — STX/ETX/CRC16 frame parser, shared by every framed
 * serial transport (UART, USB CDC). See uart_protocol.h for the wire
 * format this state machine implements.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "serial_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FRAME_STATE_IDLE,       /* Waiting for STX */
    FRAME_STATE_LENGTH,     /* Reading 4-byte length field */
    FRAME_STATE_PAYLOAD,    /* Reading payload bytes */
    FRAME_STATE_CRC,        /* Reading 2-byte CRC */
    FRAME_STATE_ETX,        /* Expecting ETX byte */
} frame_parse_state_t;

typedef struct {
    frame_parse_state_t state;
    uint8_t  len_buf[4];
    uint8_t  len_pos;
    uint32_t payload_len;
    uint8_t *payload;
    uint32_t payload_pos;
    uint8_t  crc_buf[2];
    uint8_t  crc_pos;
} frame_parser_t;

/**
 * @brief Reset parser state to idle, freeing any in-flight payload buffer.
 *
 * Must be called once before the first frame_parser_feed() (transports do
 * this in their _init()).
 */
void frame_parser_reset(frame_parser_t *p);

/**
 * @brief Feed raw bytes into the parser.
 *
 * Non-STX bytes are silently discarded, so ESP_LOG console output
 * interleaved on the same wire doesn't desync the parser. On a complete,
 * CRC-valid frame, dispatches to serial_commands via
 * serial_protocol_set_active(transport) + serial_commands_dispatch() /
 * serial_commands_handle_binary(), keyed on the payload type tag
 * (UART_PAYLOAD_JSON / UART_PAYLOAD_BINARY).
 *
 * @param p          Parser state, owned by the caller.
 * @param data       Raw bytes just read from the transport.
 * @param len        Number of bytes in data.
 * @param transport  Which transport these bytes came from — passed through
 *                   to serial_protocol_set_active() so responses route back
 *                   on the same transport.
 * @param tag        ESP_LOG TAG used for parse-error / CRC-mismatch
 *                   logging, so log lines still read per-transport
 *                   ("uart_proto" / "usb_cdc").
 */
void frame_parser_feed(frame_parser_t *p, const uint8_t *data, size_t len,
                       transport_id_t transport, const char *tag);

#ifdef __cplusplus
}
#endif
