/**
 * serial_protocol.h — Transport abstraction for serial communication.
 *
 * Supports multiple transports (UART, USB CDC) with automatic routing:
 * responses are sent back on whichever transport received the request.
 *
 * Each transport's RX task sets the current transport ID before dispatching
 * to serial_commands; the send functions check this ID to route correctly.
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transport identifiers */
typedef enum {
    TRANSPORT_UART    = 0,
    TRANSPORT_USB_CDC = 1,
    TRANSPORT_MAX
} transport_id_t;

/* Per-transport send function signatures */
typedef esp_err_t (*transport_send_frame_fn)(const uint8_t *data, size_t len);
typedef esp_err_t (*transport_send_json_fn)(const char *json_str);

/**
 * @brief Register a transport's send functions.
 *
 * Called during init by each transport (UART, USB CDC).
 */
void serial_protocol_register(transport_id_t id,
                              transport_send_frame_fn send_frame,
                              transport_send_json_fn  send_json);

/**
 * @brief Set the active transport for the current context.
 *
 * Called by a transport's RX task before dispatching to serial_commands.
 * This determines which transport send functions are used for the response.
 */
void serial_protocol_set_active(transport_id_t id);

/**
 * @brief Get the currently active transport.
 */
transport_id_t serial_protocol_get_active(void);

/**
 * @brief Send a framed response via the currently active transport.
 */
esp_err_t serial_protocol_send_frame(const uint8_t *data, size_t len);

/**
 * @brief Send a JSON response via the currently active transport.
 */
esp_err_t serial_protocol_send_json(const char *json_str);

/**
 * @brief Get transport name string (for device.info responses).
 */
const char *serial_protocol_get_name(transport_id_t id);

#ifdef __cplusplus
}
#endif
