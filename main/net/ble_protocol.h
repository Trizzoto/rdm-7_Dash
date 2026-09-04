/**
 * ble_protocol.h — Bluetooth Low Energy serial transport.
 *
 * BLE is a TRANSPORT here, not a protocol. It carries exactly the same
 * STX / length / payload / CRC16 / ETX frames as uart_protocol.c and
 * usb_cdc_protocol.c, feeds them into the same shared frame_parser.c, and
 * registers through serial_protocol.h like any other pipe — so every method
 * serial_commands.c already answers (device.info, layout.list, layout.raw,
 * layout.save, layout.set, storage.info, system.*) works over BLE the day this
 * compiles, with no new firmware API surface and nothing to keep in sync.
 *
 * WHY BLE AT ALL, given the dash already has WiFi and a web server: joining the
 * dash's own access point costs the phone its internet connection, and a phone
 * cannot be on the dash's AP and the house WiFi at once. BLE leaves the phone's
 * network alone and can be held at the same time as the GPS puck's link — which
 * is what lets the app talk to both devices in one session.
 *
 * WIRE FORMAT
 *
 *   Service  0x1FF9      (deliberately adjacent to the RDM GPS puck's 0x1FF8)
 *     0x0001  RX  WRITE           phone → dash, framed bytes, any chunking
 *     0x0002  TX  READ + NOTIFY   dash → phone, framed bytes, MTU-sized chunks
 *
 * Chunking needs no protocol of its own in either direction: the frame parser
 * is a byte-stream state machine, so a frame split across twenty notifications
 * reassembles exactly as it does across UART reads.
 *
 * PAIRING is Just Works, no bonding — the same posture the puck takes, and the
 * same trust level the dash's HTTP API already has on a local network. A
 * paddock is a more crowded room than a workshop, so a confirm-on-the-dash step
 * belongs here eventually; it is deliberately not in this first cut.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 16-bit UUIDs on the Bluetooth base UUID, as the puck does it. */
#define BLE_PROTO_SERVICE_UUID16  0x1FF9u
#define BLE_PROTO_CHR_RX_UUID16   0x0001u /* write:  phone → dash */
#define BLE_PROTO_CHR_TX_UUID16   0x0002u /* notify: dash → phone */

/**
 * @brief Claim the Bluetooth controller's memory, as early in boot as possible.
 *
 * The BT controller needs about 30 KB of CONTIGUOUS internal DMA-capable RAM,
 * and it is the one allocation on this board that cannot be pushed out to
 * PSRAM. By the time LVGL's draw buffers, the CAN driver and WiFi have taken
 * their share there is no such block left, and the controller dies inside
 * esp_bt_controller_init() with a bare "Malloc failed" followed by an
 * interrupt-watchdog panic — a boot loop, not a disabled radio.
 *
 * So the claim is made first and the radio is switched on later:
 * ble_protocol_reserve() runs near the top of app_main and only brings up the
 * controller and the NimBLE host's pools, while ble_protocol_init() registers
 * the service and starts advertising once the dash is actually ready to answer
 * a phone. Calling init alone still works — it reserves first if nobody did.
 *
 * Safe to call when Bluetooth is disabled in the build — returns
 * ESP_ERR_NOT_SUPPORTED without touching anything.
 */
esp_err_t ble_protocol_reserve(void);

/**
 * @brief Publish the service and start advertising.
 *
 * Call ble_protocol_reserve() early in boot first; see the note there for why
 * the two halves are separate.
 *
 * Safe to call when Bluetooth is disabled in the build — returns
 * ESP_ERR_NOT_SUPPORTED without touching anything.
 */
esp_err_t ble_protocol_init(void);

/**
 * @brief Hold advertising off the air, or let it resume.
 *
 * WiFi association is a timed handshake and the two radios share one antenna.
 * Measured on this board: with BLE advertising, the dash dropped its link 4-5
 * times a minute (reason 2 AUTH_EXPIRE, reason 39 TIMEOUT) against a strong AP
 * with correct credentials; the same build with Bluetooth compiled out held it
 * with zero disconnects. esp_coex_preference_set(ESP_COEX_PREFER_WIFI) was not
 * enough on its own. So advertising is simply held for the couple of seconds
 * the handshake takes. A connected central is never disturbed.
 */
void ble_protocol_hold_advertising(bool hold);

/**
 * @brief Drain queued RX bytes and dispatch them. Returns true if it did work.
 *
 * Lets ANOTHER task own the dispatch loop instead of BLE running one of its
 * own. Both transports carry the same framed protocol into the same
 * frame_parser and the same serial_commands dispatcher, so the 6 KB stack that
 * path needs was simply being paid for twice — and on this board those 6 KB
 * are the difference between the desktop USB protocol and the web server both
 * fitting, or not. `scratch` is the caller's buffer, so the pump adds no stack
 * of its own.
 */
bool ble_protocol_pump(uint8_t *scratch, size_t scratch_len);

/**
 * @brief Tell BLE that someone else will call ble_protocol_pump().
 *
 * Must be called before ble_protocol_init(), which then skips creating its own
 * RX task. uart_protocol_init() runs first in app_main and does exactly this.
 */
void ble_protocol_use_external_pump(void);

/** @brief True once a central is connected and subscribed to notifications. */
bool ble_protocol_connected(void);

/** @brief Advertised name, for device.info and the setup UI. */
const char *ble_protocol_name(void);

/**
 * @brief Send one framed payload to the connected central.
 *
 * Registered with serial_protocol so responses route back over BLE whenever a
 * request arrived on it. Called from the BLE RX task, never from the NimBLE
 * host task.
 */
esp_err_t ble_protocol_send_frame(const uint8_t *data, size_t len);

/** @brief Send a JSON payload, tagged UART_PAYLOAD_JSON. */
esp_err_t ble_protocol_send_json(const char *json_str);

#ifdef __cplusplus
}
#endif
