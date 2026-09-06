/**
 * ble_protocol.c — BLE serial transport. See ble_protocol.h for the wire
 * format and the reasoning; this file is the plumbing.
 */
#include "ble_protocol.h"

#include "frame_parser.h"
#include "serial_protocol.h"
#include "uart_protocol.h" /* STX/ETX, payload tags, uart_protocol_crc16() */

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "ble_proto";

#if !CONFIG_BT_NIMBLE_ENABLED

esp_err_t ble_protocol_reserve(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ble_protocol_pump(uint8_t *b, size_t n) { (void)b; (void)n; return false; }
void ble_protocol_use_external_pump(void) {}
void ble_protocol_hold_advertising(bool hold) { (void)hold; }
esp_err_t ble_protocol_init(void)
{
    ESP_LOGW(TAG, "NimBLE not enabled in this build");
    return ESP_ERR_NOT_SUPPORTED;
}
bool ble_protocol_connected(void) { return false; }
const char *ble_protocol_name(void) { return ""; }
esp_err_t ble_protocol_send_frame(const uint8_t *d, size_t l) { (void)d; (void)l; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ble_protocol_send_json(const char *j) { (void)j; return ESP_ERR_NOT_SUPPORTED; }

#else

#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

void ble_store_config_init(void);

/**
 * Bytes arriving from the central are parked here and drained by our own task.
 *
 * They cannot be parsed on the NimBLE host task: a dispatched command reads and
 * writes LittleFS (layout.raw, layout.save), which can block for hundreds of
 * milliseconds, and stalling the host task stalls the whole radio — dropped
 * notifications, missed connection events, and eventually a supervision
 * timeout that the phone reports as "the dash disconnected".
 */
#define BLE_RX_STREAM_BYTES 8192
/* 6144, and do not trim it to buy internal RAM: a request is dispatched on
 * this stack, and serial_commands runs cJSON over a layout and touches
 * LittleFS. 3072 overflowed on the first layout.list and rebooted the dash. */
#define BLE_RX_TASK_STACK   6144
#define BLE_RX_TASK_PRIO    5

/* Notifications are dropped into NimBLE's mbuf pool; when it is momentarily
 * full the send is retried rather than failed, because a half-sent frame is a
 * CRC error at the other end. */
/* 4 seconds of patience, not 0.4. ENOMEM here does not mean "broken", it
 * means "the radio has not drained the queue yet" — and when WiFi is busy
 * (worst case: stuck retrying an association) the controller can go far longer
 * than 400 ms without draining. A 41 KB screenshot died after 15 of ~81
 * notifications on the old budget. Retry long enough to outlast congestion;
 * the caller is a bulk transfer on its own task and can afford to wait. */
#define BLE_TX_RETRIES      200
#define BLE_TX_RETRY_MS     20

/* A screenshot is ~41 KB, which at a 517-byte MTU is over eighty notifications
 * back to back. Queued flat out — with WiFi also busy, e.g. serving the live
 * preview — that starves the Bluetooth controller badly enough to trip the
 * interrupt watchdog and reboot the dash. The two radios share one antenna and
 * a coexistence scheduler that needs slots to hand out, so a bulk transfer has
 * to breathe: yield for a tick every few chunks. Costs ~100 ms on a 41 KB
 * payload and makes the difference between a screenshot and a reboot. */
#define BLE_TX_YIELD_EVERY  8

/* Buffer-pool headroom to leave for the OTHER direction while we transmit.
 *
 * notify_all() used to take mbufs until the pool was dry, then sleep for
 * more. That is fine for our own sends — they wait — but an ATT write from
 * the phone arriving in that window has nothing to be received into, and
 * NimBLE answers it with ATT error 0x11, INSUFFICIENT_RESOURCES. The phone
 * reported exactly that, on the request it sent while a 5 KB layout reply
 * was still streaming out in eleven notifications: "layout.list failed ...
 * att 17 GATT_INSUF_RESOURCE". The dash never saw that request at all.
 *
 * A full-MTU write needs three 256-byte blocks; keep twice that free so the
 * host can also service its own housekeeping. Costs nothing in throughput
 * — a 24-block pool still carries several notifications in flight. */
#define BLE_TX_POOL_RESERVE 6

static StreamBufferHandle_t s_rx_stream;
static frame_parser_t       s_parser;
static TaskHandle_t         s_rx_task;

static uint8_t  s_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle;
static bool     s_subscribed;
static bool     s_started;
static bool     s_reserved;
static bool     s_adv_held;
static bool     s_external_pump;
static char     s_name[24];

static void advertise(void);

/* ── GATT ───────────────────────────────────────────────────────────────── */

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* A write may arrive as a chain of mbufs; the parser does not care
         * where the boundaries fall, only that the bytes stay in order. */
        for (struct os_mbuf *om = ctxt->om; om != NULL; om = SLIST_NEXT(om, om_next)) {
            const uint8_t *data = om->om_data;
            size_t len = om->om_len;
            size_t sent = xStreamBufferSend(s_rx_stream, data, len, 0);
            if (sent != len) {
                /* The RX task is behind. Dropping bytes corrupts the frame in
                 * flight, so say so — the CRC will fail and the phone retries. */
                ESP_LOGW(TAG, "RX overflow, dropped %u bytes", (unsigned)(len - sent));
            }
        }
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0; /* TX is notify-driven; a bare read is empty by design. */
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLE_PROTO_SERVICE_UUID16),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(BLE_PROTO_CHR_RX_UUID16),
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID16_DECLARE(BLE_PROTO_CHR_TX_UUID16),
                .access_cb = chr_access,
                .val_handle = &s_tx_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ── TX ─────────────────────────────────────────────────────────────────── */

/** One notification's worth of payload, given what the MTU turned out to be. */
static uint16_t chunk_size(void)
{
    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu < 23) mtu = 23; /* the default, if the exchange has not happened */
    uint16_t chunk = mtu - 3; /* ATT notification header */

    /* An attribute value is at most 512 octets, whatever the MTU. Phones
     * negotiate 517, MTU minus three is 514, and Android drops every
     * notification that size on the floor without a word — measured on the
     * bench: of a 5 KB layout reply sent as ten 514-byte notifications and
     * one of 325, the phone received only the 325. Windows tolerated the
     * oversize, which is why a PC never reproduced it. Cap at the limit;
     * the cost is two bytes per notification. */
    if (chunk > 512) chunk = 512;
    return chunk;
}

static esp_err_t notify_all(const uint8_t *data, size_t len)
{
    if (!s_subscribed || s_conn == BLE_HS_CONN_HANDLE_NONE) return ESP_ERR_INVALID_STATE;

    size_t offset = 0;
    unsigned chunks = 0;
    const uint16_t chunk = chunk_size();

    while (offset < len) {
        size_t take = len - offset;
        if (take > chunk) take = chunk;

        int rc = BLE_HS_ENOMEM;
        for (int attempt = 0; attempt < BLE_TX_RETRIES; attempt++) {
            /* Leave room for an incoming write before taking blocks for
             * this notification (see BLE_TX_POOL_RESERVE). Blocks needed:
             * the payload rounded up to the block size, plus one for the
             * header mbuf. */
            int need = (int)((take + 255) / 256) + 1;
            if (os_msys_num_free() < need + BLE_TX_POOL_RESERVE) {
                vTaskDelay(pdMS_TO_TICKS(BLE_TX_RETRY_MS));
                continue;
            }
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data + offset, take);
            if (!om) {
                vTaskDelay(pdMS_TO_TICKS(BLE_TX_RETRY_MS));
                continue;
            }
            rc = ble_gatts_notify_custom(s_conn, s_tx_val_handle, om);
            if (rc == 0) break;
            /* ble_gatts_notify_custom consumes the mbuf even when it fails. */
            if (rc != BLE_HS_ENOMEM) break; /* permanent — do not spin on it */
            vTaskDelay(pdMS_TO_TICKS(BLE_TX_RETRY_MS));
        }

        if (rc != 0) {
            ESP_LOGW(TAG, "notify failed after retries: %d (%u of %u bytes sent)",
                     rc, (unsigned)offset, (unsigned)len);
            return ESP_FAIL;
        }
        offset += take;

        if ((++chunks % BLE_TX_YIELD_EVERY) == 0) vTaskDelay(1);
    }
    return ESP_OK;
}

esp_err_t ble_protocol_send_frame(const uint8_t *data, size_t len)
{
    if (!data || len == 0 || len > UART_PROTO_MAX_PAYLOAD) return ESP_ERR_INVALID_ARG;

    uint8_t header[5] = {
        UART_PROTO_STX,
        (uint8_t)(len & 0xFF),
        (uint8_t)((len >> 8) & 0xFF),
        (uint8_t)((len >> 16) & 0xFF),
        (uint8_t)((len >> 24) & 0xFF),
    };
    uint16_t crc = uart_protocol_crc16(data, len);
    uint8_t trailer[3] = { (uint8_t)(crc & 0xFF), (uint8_t)((crc >> 8) & 0xFF), UART_PROTO_ETX };

    esp_err_t err = notify_all(header, sizeof(header));
    if (err == ESP_OK) err = notify_all(data, len);
    if (err == ESP_OK) err = notify_all(trailer, sizeof(trailer));
    return err;
}

esp_err_t ble_protocol_send_json(const char *json_str)
{
    if (!json_str) return ESP_ERR_INVALID_ARG;

    size_t json_len = strlen(json_str);
    uint8_t *buf = malloc(1 + json_len);
    if (!buf) return ESP_ERR_NO_MEM;

    buf[0] = UART_PAYLOAD_JSON;
    memcpy(buf + 1, json_str, json_len);

    esp_err_t ret = ble_protocol_send_frame(buf, 1 + json_len);
    free(buf);
    return ret;
}

/* ── RX task ────────────────────────────────────────────────────────────── */

bool ble_protocol_pump(uint8_t *scratch, size_t scratch_len)
{
    if (!s_rx_stream || !scratch || scratch_len == 0) return false;
    size_t got = xStreamBufferReceive(s_rx_stream, scratch, scratch_len, 0);
    if (got == 0) return false;
    frame_parser_feed(&s_parser, scratch, got, TRANSPORT_BLE, TAG);
    return true;
}

void ble_protocol_use_external_pump(void)
{
    s_external_pump = true;
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    for (;;) {
        size_t got = xStreamBufferReceive(s_rx_stream, buf, sizeof(buf), portMAX_DELAY);
        if (got > 0) frame_parser_feed(&s_parser, buf, got, TRANSPORT_BLE, TAG);
    }
}

/* ── GAP ────────────────────────────────────────────────────────────────── */

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            ESP_LOGI(TAG, "central connected");
            /* Ask for a short connection interval. The central picks the
             * initial one and Windows in particular starts slow; on top of
             * that, WiFi coexistence hands BLE only some of the connection
             * events. Left alone, a busy WiFi link drops throughput to a few
             * packets a second — a 4 KB layout then takes longer than the
             * app's timeout, and a screenshot never finishes. 15-30 ms with
             * zero slave latency is the difference between "instant" and
             * "appears broken". The central may refuse, which is why nothing
             * here depends on it succeeding. */
            struct ble_gap_upd_params params = {
                .itvl_min            = 12,  /* 15.0 ms (units of 1.25 ms) */
                .itvl_max            = 24,  /* 30.0 ms */
                .latency             = 0,
                .supervision_timeout = 400, /* 4 s (units of 10 ms) */
            };
            int urc = ble_gap_update_params(event->connect.conn_handle, &params);
            if (urc != 0) ESP_LOGW(TAG, "conn param update rejected: %d", urc);
        } else {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "central disconnected: %d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        /* A frame was very likely cut in half by the drop. Start the next
         * connection from a known state rather than mid-payload. */
        frame_parser_reset(&s_parser);
        xStreamBufferReset(s_rx_stream);
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_subscribed = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "notifications %s", s_subscribed ? "on" : "off");
        }
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "conn interval now %u.%02u ms (latency %u)",
                     desc.conn_itvl * 125 / 100, (desc.conn_itvl * 125) % 100,
                     desc.conn_latency);
        }
        return 0;
    }

    case BLE_GAP_EVENT_MTU:
        /* Worth one line: a 23-byte default MTU means 20-byte chunks, and a
         * 30 KB layout then takes fifteen hundred notifications. */
        ESP_LOGI(TAG, "MTU now %d", event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    default:
        return 0;
    }
}

static void advertise(void)
{
    if (s_adv_held) return; /* WiFi is mid-handshake — stay off the air */

    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.name = (uint8_t *)s_name;
    fields.name_len = strlen(s_name);
    fields.name_is_complete = 1;

    /* The service UUID goes in the advertisement so the app can filter for RDM
     * devices rather than showing the user every doorbell in the paddock. */
    static ble_uuid16_t uuid = BLE_UUID16_INIT(BLE_PROTO_SERVICE_UUID16);
    fields.uuids16 = &uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    struct ble_gap_adv_params adv = { 0 };
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;

    /* Advertise slowly (~300-500 ms) rather than at NimBLE's default rate.
     * The dash advertises forever, and every advertising event takes the
     * antenna away from WiFi through the coexistence scheduler. At the default
     * rate that was enough to break WiFi ASSOCIATION — auth and assoc are a
     * timed handshake, and losing slots in the middle of it surfaces as
     * reason 2 (AUTH_EXPIRE) and reason 39 (TIMEOUT) with a perfectly good AP
     * in range. A phone scanning for a few seconds still finds the dash well
     * inside one scan window, so the cost is nil and the link comes back. */
    adv.itvl_min = 480; /* 300 ms, in 0.625 ms units */
    adv.itvl_max = 800; /* 500 ms */

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGE(TAG, "adv_start failed: %d", rc);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable BLE address: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "address type inference failed: %d", rc);
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset: %d", reason);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ── Init ───────────────────────────────────────────────────────────────── */

#define BLE_INTERNAL_DMA (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)

esp_err_t ble_protocol_reserve(void)
{
    if (s_reserved) return ESP_OK;

    /* Logged either side of the claim because when this fails it fails as a
     * boot loop, and these two numbers are the whole diagnosis: the controller
     * wants one ~30 KB CONTIGUOUS block, so the largest block matters more
     * than the total. */
    ESP_LOGI(TAG, "reserving controller RAM: %u B internal DMA free, largest block %u B",
             (unsigned)heap_caps_get_free_size(BLE_INTERNAL_DMA),
             (unsigned)heap_caps_get_largest_free_block(BLE_INTERNAL_DMA));

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_reserved = true;

    ESP_LOGI(TAG, "controller up: %u B internal DMA free, largest block %u B",
             (unsigned)heap_caps_get_free_size(BLE_INTERNAL_DMA),
             (unsigned)heap_caps_get_largest_free_block(BLE_INTERNAL_DMA));
    return ESP_OK;
}

esp_err_t ble_protocol_init(void)
{
    if (s_started) return ESP_OK;

    esp_err_t err = ble_protocol_reserve();
    if (err != ESP_OK) return err;

    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_name, sizeof(s_name), "RDM-7 %02X%02X", mac[4], mac[5]);

    /* In PSRAM. Internal SRAM on this board is the scarce resource — the BT
     * controller, WiFi and LVGL's DMA buffers all need it and nothing else can
     * use it — and 8 KB of inbound byte queue has no reason to sit there. It is
     * written from the NimBLE host task and read by ours, both ordinary task
     * contexts, so external memory is safe here in a way a task STACK would not
     * be: the RX task writes LittleFS, and a stack must stay internal to
     * survive the cache being disabled during a flash write. */
    s_rx_stream = xStreamBufferCreateWithCaps(BLE_RX_STREAM_BYTES, 1,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx_stream) return ESP_ERR_NO_MEM;

    frame_parser_reset(&s_parser);

    /* Only run our own dispatch task when nobody else is pumping us. When the
     * UART protocol is up it owns the loop and calls ble_protocol_pump(),
     * which saves this 6 KB stack — see ble_protocol.h. */
    if (!s_external_pump) {
        if (xTaskCreate(rx_task, "ble_rx", BLE_RX_TASK_STACK, NULL,
                        BLE_RX_TASK_PRIO, &s_rx_task) != pdPASS) {
            vStreamBufferDeleteWithCaps(s_rx_stream);
            s_rx_stream = NULL;
            return ESP_ERR_NO_MEM;
        }
    } else {
        ESP_LOGI(TAG, "sharing the UART dispatch task (no ble_rx task)");
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /* Just Works, and nothing kept. This is the same posture the GPS puck
     * takes: pairing is accepted so a phone that insists on encrypting can,
     * but no keys are stored, so a phone can never reconnect believing it has
     * a relationship the dash no longer honours. */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;
    ble_store_config_init();

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT registration failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(s_name);
    if (rc != 0) ESP_LOGW(TAG, "could not set the GAP device name: %d", rc);

    nimble_port_freertos_init(host_task);

    serial_protocol_register(TRANSPORT_BLE, ble_protocol_send_frame, ble_protocol_send_json);
    s_started = true;

    ESP_LOGI(TAG, "BLE serial transport up as '%s'", s_name);
    return ESP_OK;
}

void ble_protocol_hold_advertising(bool hold)
{
    if (!s_started || s_adv_held == hold) return;
    s_adv_held = hold;
    if (hold) {
        ble_gap_adv_stop();
        ESP_LOGI(TAG, "advertising held (WiFi connecting)");
    } else if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGI(TAG, "advertising resumed");
        advertise();
    }
}

bool ble_protocol_connected(void)
{
    return s_started && s_conn != BLE_HS_CONN_HANDLE_NONE && s_subscribed;
}

const char *ble_protocol_name(void)
{
    return s_name;
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */
