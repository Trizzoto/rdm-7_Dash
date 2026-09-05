/**
 * serial_protocol.c — Transport abstraction for serial communication.
 *
 * Maintains a registry of transport send functions and a task-local
 * "active transport" that determines where responses are routed.
 */
#include "serial_protocol.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

static const char *TAG = "serial_proto";

/* Transport registry */
static struct {
    transport_send_frame_fn send_frame;
    transport_send_json_fn  send_json;
    bool registered;
} s_transports[TRANSPORT_MAX];

/* Active transport, PER DISPATCHING TASK.
 *
 * Dispatch is synchronous within a task: the RX task sets the active id,
 * calls serial_commands_dispatch(), the handler's reply goes out, and it
 * loops back. Every reply is sent on the task that received the request —
 * no deferred callback answers a request — so keying on the task is exact.
 *
 * This used to be one global, on the assumption written here that "each
 * transport has its own task so there's no contention". That is no longer
 * true: Bluetooth shares the UART dispatch task to save a 6 KB stack (see
 * ble_protocol.h), and USB CDC has a task of its own. With a single global,
 * a frame arriving on one transport could move the target out from under a
 * reply being composed for another, and the answer would leave down the
 * wrong pipe — the requester waiting forever for a reply that went out the
 * serial port. Keyed by task, that cannot happen. */
#define ACTIVE_SLOTS 4
static struct {
    TaskHandle_t   task;
    transport_id_t id;
} s_active[ACTIVE_SLOTS];
static portMUX_TYPE s_active_lock = portMUX_INITIALIZER_UNLOCKED;

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
    if (id >= TRANSPORT_MAX) return;

    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    bool placed = false;

    taskENTER_CRITICAL(&s_active_lock);
    int spare = -1;
    for (int i = 0; i < ACTIVE_SLOTS; i++) {
        if (s_active[i].task == me) {
            s_active[i].id = id;
            placed = true;
            break;
        }
        if (s_active[i].task == NULL && spare < 0) spare = i;
    }
    if (!placed && spare >= 0) {
        s_active[spare].task = me;
        s_active[spare].id   = id;
        placed = true;
    }
    taskEXIT_CRITICAL(&s_active_lock);

    /* Outside the critical section: ESP_LOG takes locks of its own. */
    if (!placed)
        ESP_LOGE(TAG, "no slot for dispatch task — replies will route to %s",
                 serial_protocol_get_name(TRANSPORT_UART));
}

transport_id_t serial_protocol_get_active(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    transport_id_t id = TRANSPORT_UART;

    taskENTER_CRITICAL(&s_active_lock);
    for (int i = 0; i < ACTIVE_SLOTS; i++) {
        if (s_active[i].task == me) {
            id = s_active[i].id;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_active_lock);
    return id;
}

esp_err_t serial_protocol_send_frame(const uint8_t *data, size_t len)
{
    transport_id_t id = serial_protocol_get_active();
    if (id >= TRANSPORT_MAX || !s_transports[id].registered ||
        !s_transports[id].send_frame) {
        ESP_LOGE(TAG, "No send_frame for transport %d", id);
        return ESP_ERR_INVALID_STATE;
    }
    return s_transports[id].send_frame(data, len);
}

esp_err_t serial_protocol_send_json(const char *json_str)
{
    transport_id_t id = serial_protocol_get_active();
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
    case TRANSPORT_BLE:     return "ble";
    default:                return "unknown";
    }
}
