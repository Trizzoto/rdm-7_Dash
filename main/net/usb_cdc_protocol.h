/**
 * usb_cdc_protocol.h — USB CDC ACM serial transport.
 *
 * Provides the same framed protocol (STX/ETX/CRC16) over the ESP32-S3's
 * native USB peripheral via TinyUSB CDC ACM class.
 *
 * USB 1.1 Full Speed (12 Mbps) — roughly 10x faster than 921600 baud UART.
 *
 * ── BUILD STATUS ────────────────────────────────────────────────────────
 * Intentionally NOT listed in main/CMakeLists.txt SRCS. USB CDC ACM and
 * the USB Serial JTAG console share the same physical USB peripheral on
 * the ESP32-S3 and cannot coexist. The USB Serial JTAG console is the
 * default boot/log path; enabling CDC requires disabling that first.
 *
 * To enable: see the step-by-step block in main/main.c near the
 * `usb_cdc_protocol_init()` call site (currently commented out). Once
 * sdkconfig is updated, add this file to SRCS in main/CMakeLists.txt.
 *
 * Keeping the source in-tree so the protocol implementation stays
 * version-controlled and discoverable — re-enabling is a config flip,
 * not a re-port.
 * ────────────────────────────────────────────────────────────────────────
 */
#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise USB CDC protocol handler.
 *
 * Configures TinyUSB device stack with CDC ACM class and starts
 * the RX processing task on core 0.
 *
 * @return ESP_OK on success.
 */
esp_err_t usb_cdc_protocol_init(void);

/**
 * @brief Send a framed response over USB CDC.
 *
 * Wraps the payload in STX/Length/CRC/ETX and transmits via CDC ACM.
 *
 * @param data    Payload bytes.
 * @param len     Length of payload.
 * @return ESP_OK on success.
 */
esp_err_t usb_cdc_protocol_send_frame(const uint8_t *data, size_t len);

/**
 * @brief Send a JSON response string as a framed message over USB CDC.
 *
 * Prepends the JSON payload type tag (0x00) and frames it.
 *
 * @param json_str  Null-terminated JSON string.
 * @return ESP_OK on success.
 */
esp_err_t usb_cdc_protocol_send_json(const char *json_str);

#ifdef __cplusplus
}
#endif
