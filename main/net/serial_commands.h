/**
 * serial_commands.h — JSON-RPC command dispatcher for UART serial protocol.
 *
 * Maps method names (e.g. "layout.list", "image.upload.start") to handler
 * functions that call the same core logic as the HTTP web server.
 */
#pragma once

#include "esp_err.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Dispatch a JSON-RPC request and send the response over UART.
 *
 * Parses the JSON request, looks up the method in the dispatch table,
 * calls the handler, and sends the JSON response frame.
 *
 * @param json_str  Null-terminated JSON request string.
 * @param len       Length of the string (not including null terminator).
 * @return ESP_OK on success.
 */
esp_err_t serial_commands_dispatch(const char *json_str, size_t len);

/**
 * @brief Handle a binary chunk frame (image/font/OTA upload).
 *
 * @param data   Raw binary payload (after type tag byte).
 * @param len    Length of payload.
 * @return ESP_OK on success.
 */
esp_err_t serial_commands_handle_binary(const uint8_t *data, size_t len);

/**
 * @brief Binary upload session info (used for chunked transfers).
 */
typedef struct {
    char     type[16];       /* "image", "font", or "ota" */
    char     name[32];       /* Asset name */
    uint32_t total_size;     /* Total expected bytes */
    uint32_t received;       /* Bytes received so far */
    uint16_t total_chunks;   /* Total chunk count */
    uint16_t next_chunk;     /* Next expected chunk index */
    uint8_t *buffer;         /* Accumulation buffer (PSRAM) */
    uint64_t session_id;     /* Session identifier */
    bool     active;         /* Session is in progress */
    /* OTA-specific fields */
    void    *ota_handle;     /* esp_ota_handle_t for OTA writes */
    void    *ota_partition;  /* Target OTA partition */
} serial_upload_session_t;

#ifdef __cplusplus
}
#endif
