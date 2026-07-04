/**
 * uart_protocol.c — UART serial protocol handler.
 *
 * Runs a receive task on core 0 that reads UART bytes, assembles STX/ETX
 * frames, validates CRC16, and dispatches to serial_commands.
 *
 * Non-framed data (ESP_LOG output) is silently discarded by the frame parser.
 */
#include "uart_protocol.h"
#include "serial_protocol.h"
#include "frame_parser.h"

#include "driver/uart.h"
#include "driver/gpio.h"  /* gpio_pullup_dis() — see uart_protocol_init below */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "uart_proto";

/* RX buffer sizes */
#define UART_RX_BUF_SIZE   (8 * 1024)
#define UART_TX_BUF_SIZE   (4 * 1024)
#define UART_RX_CHUNK      512

/* Frame parser context — state machine shared with usb_cdc_protocol.c via
 * frame_parser.c. */
static frame_parser_t s_parser;

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

/* ── ESP-IDF log redirect (plain-text on UART1) ────────────────────────────
 *
 * ESP_LOG* calls are redirected to write raw ASCII text into the same UART1
 * stream that carries JSON-RPC frames. Logs and frames interleave on the
 * wire — the desktop app's frame parser handles the mix: STX-delimited
 * bytes go to the frame dispatcher, everything else gets surfaced as log
 * output in the UI. Same s_tx_mutex protects both writers so frames never
 * get mid-write text inserted.
 *
 * No early-boot buffering is needed: before this hook is installed (at the
 * end of uart_protocol_init), ESP-IDF's default console keeps writing
 * plain text to UART0 with pads still muxed to 43/44, so boot logs appear
 * normally in a serial monitor.
 * ──────────────────────────────────────────────────────────────────────── */

static volatile int s_log_reentry_depth = 0;
static bool s_log_hook_installed = false;

static int s_log_vprintf(const char *fmt, va_list args)
{
    /* Re-entry guard — uart_write_bytes can theoretically log on error,
       which would loop us back here. Drop the inner log. */
    if (s_log_reentry_depth > 0) return 0;
    s_log_reentry_depth++;

    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n < 0) {
        s_log_reentry_depth--;
        return 0;
    }
    if (n > (int)(sizeof(line) - 1)) n = (int)(sizeof(line) - 1);

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    uart_write_bytes(UART_PROTO_PORT_NUM, line, (size_t)n);
    xSemaphoreGive(s_tx_mutex);

    s_log_reentry_depth--;
    return n;
}

static void s_install_log_hook(void)
{
    if (s_log_hook_installed) return;
    s_log_hook_installed = true;
    esp_log_set_vprintf(s_log_vprintf);
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
            frame_parser_feed(&s_parser, rx_buf, (size_t)len, TRANSPORT_UART, TAG);
        }
    }
}

/* ── Initialisation ─────────────────────────────────────────────────────── */

esp_err_t uart_protocol_init(void)
{
    s_tx_mutex = xSemaphoreCreateMutex();
    if (!s_tx_mutex) return ESP_ERR_NO_MEM;

    frame_parser_reset(&s_parser);

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

    /* uart_set_pin() enables the internal pull-up on the RX pin by default;
     * we explicitly flip both pins to "pull-down only" to match the wire-
     * inputs path. Result: the lines read LOW when externally disconnected
     * (e.g. when the USB-UART switch is in its non-UART1 position), and
     * UART1 still works normally because the external USB-UART bridge has
     * a strong driver that overrides the weak internal pull-down. */
    gpio_pullup_dis(UART_PROTO_TX_PIN);
    gpio_pulldown_en(UART_PROTO_TX_PIN);
    gpio_pullup_dis(UART_PROTO_RX_PIN);
    gpio_pulldown_en(UART_PROTO_RX_PIN);

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

    /* Register with transport abstraction */
    serial_protocol_register(TRANSPORT_UART,
                             uart_protocol_send_frame,
                             uart_protocol_send_json);

    ESP_LOGI(TAG, "UART protocol initialised (port %d, %d baud)",
             UART_PROTO_PORT_NUM, UART_PROTO_BAUD_RATE);

    /* UART1 is up — redirect all further ESP_LOG output through this UART
       as plain text so the desktop app's frame parser can pick them up as
       non-framed bytes alongside JSON-RPC frames. Logs emitted before this
       point already went to UART0's default console on the same pads, so
       no buffering of pre-init logs is needed. */
    s_install_log_hook();
    return ESP_OK;
}
