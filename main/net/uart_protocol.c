/**
 * uart_protocol.c — UART serial protocol handler.
 *
 * Runs a receive task on core 0 that reads UART bytes, assembles STX/ETX
 * frames, validates CRC16, and dispatches to serial_commands.
 *
 * Non-framed data (ESP_LOG output) is silently discarded by the frame parser.
 */
#include "uart_protocol.h"
#include "serial_commands.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "uart_proto";

/* RX buffer sizes */
#define UART_RX_BUF_SIZE   (8 * 1024)
#define UART_TX_BUF_SIZE   (4 * 1024)
#define UART_RX_CHUNK      512

/* Frame parser states */
typedef enum {
    STATE_IDLE,       /* Waiting for STX */
    STATE_LENGTH,     /* Reading 4-byte length field */
    STATE_PAYLOAD,    /* Reading payload bytes */
    STATE_CRC,        /* Reading 2-byte CRC */
    STATE_ETX,        /* Expecting ETX byte */
} parse_state_t;

/* Parser context */
static struct {
    parse_state_t state;
    uint8_t  len_buf[4];
    uint8_t  len_pos;
    uint32_t payload_len;
    uint8_t *payload;
    uint32_t payload_pos;
    uint8_t  crc_buf[2];
    uint8_t  crc_pos;
} s_parser;

/* TX mutex for thread-safe sending */
static SemaphoreHandle_t s_tx_mutex;

/* ── CRC16-CCITT ────────────────────────────────────────────────────────── */

uint16_t uart_protocol_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ── Frame sending ──────────────────────────────────────────────────────── */

esp_err_t uart_protocol_send_frame(const uint8_t *data, size_t len)
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
    uart_write_bytes(UART_PROTO_PORT_NUM, header, sizeof(header));
    uart_write_bytes(UART_PROTO_PORT_NUM, data, len);
    uart_write_bytes(UART_PROTO_PORT_NUM, trailer, sizeof(trailer));
    xSemaphoreGive(s_tx_mutex);

    return ESP_OK;
}

esp_err_t uart_protocol_send_json(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;

    size_t json_len = strlen(json_str);
    size_t total = 1 + json_len; /* type tag + JSON */

    uint8_t *buf = malloc(total);
    if (!buf) return ESP_ERR_NO_MEM;

    buf[0] = UART_PAYLOAD_JSON;
    memcpy(buf + 1, json_str, json_len);

    esp_err_t ret = uart_protocol_send_frame(buf, total);
    free(buf);
    return ret;
}

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
    /* Validate CRC */
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

    uint8_t payload_type = s_parser.payload[0];
    uint8_t *payload_data = s_parser.payload + 1;
    size_t payload_data_len = s_parser.payload_len - 1;

    if (payload_type == UART_PAYLOAD_JSON) {
        /* Null-terminate the JSON string */
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

    /* Don't free payload here — _parser_reset handles it */
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
            /* Non-STX bytes (ESP_LOG output) are silently discarded */
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

/* ── UART RX task ───────────────────────────────────────────────────────── */

static void uart_rx_task(void *arg)
{
    (void)arg;
    uint8_t rx_buf[UART_RX_CHUNK];

    ESP_LOGI(TAG, "UART RX task started (port %d, %d baud)",
             UART_PROTO_PORT_NUM, UART_PROTO_BAUD_RATE);

    while (true) {
        int len = uart_read_bytes(UART_PROTO_PORT_NUM, rx_buf,
                                  sizeof(rx_buf), pdMS_TO_TICKS(50));
        if (len > 0) {
            _parser_feed(rx_buf, (size_t)len);
        }
    }
}

/* ── Initialisation ─────────────────────────────────────────────────────── */

esp_err_t uart_protocol_init(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) return ESP_ERR_NO_MEM;

    _parser_reset();

    /* Configure UART — reuse default console UART pins.
     * On ESP32-S3, UART0 TX=GPIO43, RX=GPIO44 (connected to USB-UART bridge). */
    uart_config_t uart_config = {
        .baud_rate  = UART_PROTO_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t ret = uart_param_config(UART_PROTO_PORT_NUM, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Set pins for UART1 */
    ret = uart_set_pin(UART_PROTO_PORT_NUM,
                       UART_PROTO_TX_PIN, UART_PROTO_RX_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = uart_driver_install(UART_PROTO_PORT_NUM,
                              UART_RX_BUF_SIZE, UART_TX_BUF_SIZE,
                              0, NULL, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Start RX task on core 0, priority 5 (below CAN RX at 7) */
    BaseType_t xret = xTaskCreatePinnedToCore(
        uart_rx_task, "uart_rx", 6144, NULL, 5, NULL, 0);
    if (xret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UART RX task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UART protocol initialised (port %d, %d baud)",
             UART_PROTO_PORT_NUM, UART_PROTO_BAUD_RATE);
    return ESP_OK;
}
