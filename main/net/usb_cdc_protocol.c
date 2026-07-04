/**
 * usb_cdc_protocol.c — USB CDC ACM serial transport.
 *
 * Uses TinyUSB CDC ACM to provide the same STX/ETX/CRC16 framed protocol
 * as uart_protocol.c, but over the ESP32-S3's native USB (12 Mbps).
 *
 * The frame parser state machine itself lives in frame_parser.c, shared
 * with uart_protocol.c — only the I/O layer differs here
 * (tinyusb_cdcacm_read / tinyusb_cdcacm_write_queue).
 *
 * NOTE: This file is intentionally NOT included in the firmware build —
 * see the BUILD STATUS block in usb_cdc_protocol.h for the rationale and
 * re-enable steps.
 */
#include "usb_cdc_protocol.h"
#include "serial_protocol.h"
#include "uart_protocol.h"          /* reuse uart_protocol_crc16() */
#include "frame_parser.h"

#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "usb_cdc";

/* Re-use the same protocol constants from uart_protocol.h */
#define CDC_RX_BUF_SIZE   (8 * 1024)
#define CDC_TX_CHUNK       512

/* Frame parser context — state machine shared with uart_protocol.c via
 * frame_parser.c. */
static frame_parser_t s_parser;

static SemaphoreHandle_t s_tx_mutex;

/* ── Frame sending ──────────────────────────────────────────────────────── */

esp_err_t usb_cdc_protocol_send_frame(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > UART_PROTO_MAX_PAYLOAD)
        return ESP_ERR_INVALID_ARG;

    uint8_t header[5];
    header[0] = UART_PROTO_STX;
    header[1] = (uint8_t)(len & 0xFF);
    header[2] = (uint8_t)((len >> 8) & 0xFF);
    header[3] = (uint8_t)((len >> 16) & 0xFF);
    header[4] = (uint8_t)((len >> 24) & 0xFF);

    uint16_t crc = uart_protocol_crc16(data, len);
    uint8_t trailer[3];
    trailer[0] = (uint8_t)(crc & 0xFF);
    trailer[1] = (uint8_t)((crc >> 8) & 0xFF);
    trailer[2] = UART_PROTO_ETX;

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);

    /* Write in chunks — tinyusb_cdcacm_write_queue has a limited buffer */
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, header, sizeof(header));

    /* Send payload in chunks to avoid overwhelming the USB buffer */
    size_t offset = 0;
    while (offset < len) {
        size_t chunk = (len - offset > CDC_TX_CHUNK) ? CDC_TX_CHUNK : (len - offset);
        tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, data + offset, chunk);
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));
        offset += chunk;
    }

    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, trailer, sizeof(trailer));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));

    xSemaphoreGive(s_tx_mutex);

    return ESP_OK;
}

esp_err_t usb_cdc_protocol_send_json(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;

    size_t json_len = strlen(json_str);
    size_t total = 1 + json_len;

    uint8_t *buf = malloc(total);
    if (!buf) return ESP_ERR_NO_MEM;

    buf[0] = UART_PAYLOAD_JSON;
    memcpy(buf + 1, json_str, json_len);

    esp_err_t ret = usb_cdc_protocol_send_frame(buf, total);
    free(buf);
    return ret;
}

/* ── CDC RX callback → feed parser ──────────────────────────────────────── */

static void _cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    (void)itf;
    uint8_t buf[CDC_RX_BUF_SIZE];
    size_t rx_size = 0;

    esp_err_t ret = tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, buf,
                                          sizeof(buf), &rx_size);
    if (ret == ESP_OK && rx_size > 0) {
        frame_parser_feed(&s_parser, buf, rx_size, TRANSPORT_USB_CDC, TAG);
    }
}

/* ── Initialisation ─────────────────────────────────────────────────────── */

esp_err_t usb_cdc_protocol_init(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) return ESP_ERR_NO_MEM;

    frame_parser_reset(&s_parser);

    ESP_LOGI(TAG, "Initialising TinyUSB CDC ACM...");

    /* TinyUSB device configuration (v2 API — use defaults from Kconfig) */
    const tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
    };

    esp_err_t ret = tinyusb_driver_install(&tusb_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TinyUSB driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* CDC ACM configuration (v2 API) */
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
        .callback_rx = &_cdc_rx_callback,
        .callback_rx_wanted_char = NULL,
        .callback_line_state_changed = NULL,
        .callback_line_coding_changed = NULL,
    };

    ret = tinyusb_cdcacm_init(&acm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CDC ACM init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Register with transport abstraction */
    serial_protocol_register(TRANSPORT_USB_CDC,
                             usb_cdc_protocol_send_frame,
                             usb_cdc_protocol_send_json);

    ESP_LOGI(TAG, "USB CDC protocol initialised (12 Mbps Full Speed)");
    return ESP_OK;
}
