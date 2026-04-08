/**
 * usb_cdc_protocol.c — USB CDC ACM serial transport.
 *
 * Uses TinyUSB CDC ACM to provide the same STX/ETX/CRC16 framed protocol
 * as uart_protocol.c, but over the ESP32-S3's native USB (12 Mbps).
 *
 * The frame parser is identical to the UART version — only the I/O layer
 * differs (tinyusb_cdcacm_read / tinyusb_cdcacm_write_queue).
 */
#include "usb_cdc_protocol.h"
#include "serial_protocol.h"
#include "serial_commands.h"
#include "uart_protocol.h"          /* reuse uart_protocol_crc16() */

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

/* Frame parser states (same as uart_protocol.c) */
typedef enum {
    STATE_IDLE,
    STATE_LENGTH,
    STATE_PAYLOAD,
    STATE_CRC,
    STATE_ETX,
} cdc_parse_state_t;

static struct {
    cdc_parse_state_t state;
    uint8_t  len_buf[4];
    uint8_t  len_pos;
    uint32_t payload_len;
    uint8_t *payload;
    uint32_t payload_pos;
    uint8_t  crc_buf[2];
    uint8_t  crc_pos;
} s_parser;

static SemaphoreHandle_t s_tx_mutex;

/* ── Frame parser ───────────────────────────────────────────────────────── */

static void _parser_reset(void)
{
    if (s_parser.payload) {
        free(s_parser.payload);
        s_parser.payload = NULL;
    }
    s_parser.state = STATE_IDLE;
    s_parser.len_pos = 0;
    s_parser.payload_len = 0;
    s_parser.payload_pos = 0;
    s_parser.crc_pos = 0;
}

static void _process_complete_frame(void)
{
    uint16_t received_crc = (uint16_t)s_parser.crc_buf[0] |
                            ((uint16_t)s_parser.crc_buf[1] << 8);
    uint16_t computed_crc = uart_protocol_crc16(s_parser.payload,
                                                 s_parser.payload_len);

    if (received_crc != computed_crc) {
        ESP_LOGW(TAG, "CRC mismatch: got 0x%04X, expected 0x%04X",
                 received_crc, computed_crc);
        _parser_reset();
        return;
    }

    if (s_parser.payload_len < 1) {
        ESP_LOGW(TAG, "Empty payload");
        _parser_reset();
        return;
    }

    /* Set active transport to USB CDC before dispatching */
    serial_protocol_set_active(TRANSPORT_USB_CDC);

    uint8_t payload_type = s_parser.payload[0];
    uint8_t *payload_data = s_parser.payload + 1;
    size_t payload_data_len = s_parser.payload_len - 1;

    if (payload_type == UART_PAYLOAD_JSON) {
        char *json_str = malloc(payload_data_len + 1);
        if (json_str) {
            memcpy(json_str, payload_data, payload_data_len);
            json_str[payload_data_len] = '\0';
            serial_commands_dispatch(json_str, payload_data_len);
            free(json_str);
        } else {
            ESP_LOGE(TAG, "OOM for JSON dispatch (%u bytes)",
                     (unsigned)payload_data_len);
        }
    } else if (payload_type == UART_PAYLOAD_BINARY) {
        serial_commands_handle_binary(payload_data, payload_data_len);
    } else {
        ESP_LOGW(TAG, "Unknown payload type: 0x%02X", payload_type);
    }

    _parser_reset();
}

static void _parser_feed(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t byte = data[i];

        switch (s_parser.state) {
        case STATE_IDLE:
            if (byte == UART_PROTO_STX) {
                s_parser.state = STATE_LENGTH;
                s_parser.len_pos = 0;
            }
            break;

        case STATE_LENGTH:
            s_parser.len_buf[s_parser.len_pos++] = byte;
            if (s_parser.len_pos == 4) {
                s_parser.payload_len = (uint32_t)s_parser.len_buf[0] |
                                       ((uint32_t)s_parser.len_buf[1] << 8) |
                                       ((uint32_t)s_parser.len_buf[2] << 16) |
                                       ((uint32_t)s_parser.len_buf[3] << 24);
                if (s_parser.payload_len == 0 ||
                    s_parser.payload_len > UART_PROTO_MAX_PAYLOAD) {
                    ESP_LOGW(TAG, "Invalid frame length: %u",
                             (unsigned)s_parser.payload_len);
                    _parser_reset();
                    break;
                }
                s_parser.payload = malloc(s_parser.payload_len);
                if (!s_parser.payload) {
                    ESP_LOGE(TAG, "OOM for frame (%u bytes)",
                             (unsigned)s_parser.payload_len);
                    _parser_reset();
                    break;
                }
                s_parser.payload_pos = 0;
                s_parser.state = STATE_PAYLOAD;
            }
            break;

        case STATE_PAYLOAD:
            s_parser.payload[s_parser.payload_pos++] = byte;
            if (s_parser.payload_pos == s_parser.payload_len) {
                s_parser.state = STATE_CRC;
                s_parser.crc_pos = 0;
            }
            break;

        case STATE_CRC:
            s_parser.crc_buf[s_parser.crc_pos++] = byte;
            if (s_parser.crc_pos == 2) {
                s_parser.state = STATE_ETX;
            }
            break;

        case STATE_ETX:
            if (byte == UART_PROTO_ETX) {
                _process_complete_frame();
            } else {
                ESP_LOGW(TAG, "Expected ETX, got 0x%02X", byte);
                _parser_reset();
            }
            break;
        }
    }
}

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
        _parser_feed(buf, rx_size);
    }
}

/* ── Initialisation ─────────────────────────────────────────────────────── */

esp_err_t usb_cdc_protocol_init(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) return ESP_ERR_NO_MEM;

    _parser_reset();

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
