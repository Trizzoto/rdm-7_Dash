/**
 * uart_protocol.h — Serial command protocol over UART.
 *
 * Frame format (STX/ETX with CRC16):
 *   [STX 0x02] [Length: 4B LE] [Payload: N bytes] [CRC16: 2B LE] [ETX 0x03]
 *
 * Length field = size of Payload only (not including STX/Length/CRC/ETX).
 * CRC16-CCITT over the Payload bytes.
 *
 * JSON-RPC style request/response:
 *   Request:  {"id": 1, "method": "layout.list", "params": {}}
 *   Response: {"id": 1, "result": [...], "error": null}
 *
 * Binary chunk frames (for image/font/OTA uploads):
 *   [STX] [Length: 4B LE] [session: 8B] [chunk_idx: 2B LE] [data: NB] [CRC16: 2B LE] [ETX]
 *   The first byte of payload distinguishes: 0x00 = JSON, 0x01 = binary chunk.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Frame markers */
#define UART_PROTO_STX  0x02
#define UART_PROTO_ETX  0x03

/* Payload type tags (first byte of payload) */
#define UART_PAYLOAD_JSON   0x00
#define UART_PAYLOAD_BINARY 0x01

/* Protocol limits */
#define UART_PROTO_MAX_PAYLOAD  (64 * 1024)  /* 64 KB max JSON payload */
#define UART_PROTO_CHUNK_SIZE   4096         /* Binary chunk size */
#define UART_PROTO_BAUD_RATE    921600

/* UART port — UART1 for serial protocol (UART0 stays as ESP_LOG console).
 * User has a hardware switch to route USB-UART bridge between UART0/UART1. */
#define UART_PROTO_PORT_NUM     1

/* GPIO pins for UART1 — same physical pins as UART0 console,
 * hardware switch selects which UART peripheral drives them. */
#define UART_PROTO_TX_PIN       43
#define UART_PROTO_RX_PIN       44

/**
 * @brief Initialise UART protocol handler.
 *
 * Configures UART0 at 921600 baud and starts the RX processing task
 * on core 0 (priority 5).
 *
 * @return ESP_OK on success.
 */
esp_err_t uart_protocol_init(void);

/**
 * @brief Send a framed response over UART.
 *
 * Wraps the payload in STX/Length/CRC/ETX and transmits.
 *
 * @param data    Payload bytes.
 * @param len     Length of payload.
 * @return ESP_OK on success.
 */
esp_err_t uart_protocol_send_frame(const uint8_t *data, size_t len);

/**
 * @brief Send a JSON response string as a framed message.
 *
 * Prepends the JSON payload type tag (0x00) and frames it.
 *
 * @param json_str  Null-terminated JSON string.
 * @return ESP_OK on success.
 */
esp_err_t uart_protocol_send_json(const char *json_str);

/**
 * @brief Compute CRC16-CCITT over a buffer.
 *
 * @param data  Input bytes.
 * @param len   Number of bytes.
 * @return CRC16 value.
 */
uint16_t uart_protocol_crc16(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
