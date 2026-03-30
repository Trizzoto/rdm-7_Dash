/**
 * serial_protocol.c — Transport abstraction for serial communication.
 *
 * Maintains a registry of transport send functions and a task-local
 * "active transport" that determines where responses are routed.
 */
#include "serial_protocol.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "serial_proto";

/* Transport registry */
static struct {
    transport_send_frame_fn send_frame;
    transport_send_json_fn  send_json;
    bool registered;
} s_transports[TRANSPORT_MAX];

/* Active transport — set by each RX task before dispatching.
 * Dispatch is synchronous within a task: the RX task sets the active ID,
 * calls serial_commands_dispatch, responses are sent, then it loops back.
 * Each transport has its own task so there's no contention. */
static volatile transport_id_t s_active = TRANSPORT_UART;

void serial_protocol_register(transport_id_t id,
                              transport_send_frame_fn send_frame,
                              transport_send_json_fn  send_json)
{
    if (id >= TRANSPORT_MAX) return;
    s_transports[id].send_frame = send_frame;
    s_transports[id].send_json  = send_json;
    s_transports[id].registered = true;
    ESP_LOGI(TAG, "Registered transport: %s", serial_protocol_get_name(id));
}

void serial_protocol_set_active(transport_id_t id)
{
    if (id < TRANSPORT_MAX) {
        s_active = id;
    }
}

transport_id_t serial_protocol_get_active(void)
{
    return s_active;
}

esp_err_t serial_protocol_send_frame(const uint8_t *data, size_t len)
{
    transport_id_t id = s_active;
    if (id >= TRANSPORT_MAX || !s_transports[id].registered ||
        !s_transports[id].send_frame) {
        ESP_LOGE(TAG, "No send_frame for transport %d", id);
        return ESP_ERR_INVALID_STATE;
    }
    return s_transports[id].send_frame(data, len);
}

esp_err_t serial_protocol_send_json(const char *json_str)
{
    transport_id_t id = s_active;
    if (id >= TRANSPORT_MAX || !s_transports[id].registered ||
        !s_transports[id].send_json) {
        ESP_LOGE(TAG, "No send_json for transport %d", id);
        return ESP_ERR_INVALID_STATE;
    }
    return s_transports[id].send_json(json_str);
}

const char *serial_protocol_get_name(transport_id_t id)
{
    switch (id) {
    case TRANSPORT_UART:    return "uart";
    case TRANSPORT_USB_CDC: return "usb_cdc";
    default:                return "unknown";
    }
}
