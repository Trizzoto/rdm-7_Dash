/**
 * obd2.c — OBD2 polling + decode (see obd2.h).
 *
 * Implements Mode 01 (and the framework to extend to Mode 22) requests
 * over ISO 15765-4 CAN. Two big design choices:
 *
 * 1. Pipelined TX (no wait-for-response gate). The poll timer fires one
 *    request per tick (30 ms) and the RX handler decodes responses
 *    asynchronously by looking up the PID echo in the static decode
 *    table. This is ~3-5x faster than the prior wait-then-send model
 *    that left the bus idle most of the time.
 *
 * 2. Per-PID adaptive scheduling. Each PID has its own target poll
 *    period and a per-PID last_tx / last_response timestamp. PIDs that
 *    don't respond are downgraded to "dead" (5 sec retry rate); PIDs
 *    that do respond get the rate of their tier (fast = 10 Hz, slow =
 *    1 Hz). The scheduler picks the most-starved PID each tick. On
 *    cars like a Toyota HiAce that only supports ~10 of 30 default
 *    PIDs, this naturally focuses bandwidth on the live ones.
 *
 * Threading: everything runs on the LVGL task — the timer is an LVGL
 * timer, the RX hook is called from can_process_queued_frames (also
 * LVGL). No locks needed inside; can_transmit_frame is thread-safe.
 */
#include "obd2.h"

#include "can_manager.h"
#include "signal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "obd2";

/* Tick = TX cadence. 30 ms = 33 req/sec — fast enough to keep the bus
 * busy without overwhelming Japanese ECUs that throttle requests faster
 * than ~10 ms apart. */
#define OBD2_TICK_MS                 30

/* Target poll periods per tier (ms between requests for that PID). */
#define OBD2_PERIOD_FAST_MS          100      /* ~10 Hz */
#define OBD2_PERIOD_SLOW_MS         1000      /* ~1 Hz */
#define OBD2_PERIOD_DEAD_MS         5000      /* unresponsive: probe every 5 s */

/* Time after the last response before a PID is declared "dead". */
#define OBD2_DEAD_THRESHOLD_MS      3000

/* Scan: each Mode 01 PID-0x00/0x20/... query opens a collection window
 * during which we accept responses from any ECU on 0x7E8-0x7EF and
 * dedup-merge their supported-PID bitmasks. 250 ms is enough for slower
 * ECUs (TCU, hybrid battery) to chime in after the engine ECU. */
#define OBD2_SCAN_WINDOW_MS          250

/* ── Per-PID poll state ───────────────────────────────────────────────── */

typedef struct {
    uint8_t  pid;
    uint8_t  service;          /* cached from def, used in TX framing */
    uint16_t target_period_ms; /* current poll rate (alive uses tier;
                                  dead uses OBD2_PERIOD_DEAD_MS) */
    uint64_t last_tx_ms;
    uint64_t last_response_ms;
} obd2_poll_state_t;

static obd2_poll_state_t s_poll[OBD2_MAX_ENABLED];
static uint8_t           s_poll_count = 0;
static lv_timer_t       *s_poll_timer = NULL;
static bool              s_running    = false;

/* ── Discovery scan state ─────────────────────────────────────────────── */

typedef enum {
    SCAN_IDLE = 0,
    SCAN_WINDOW_OPEN,    /* request sent, accepting responses */
} obd2_scan_state_t;

static obd2_scan_state_t  s_scan_state         = SCAN_IDLE;
static uint8_t            s_scan_query_pid     = 0x00;
static uint64_t           s_scan_window_start  = 0;
static bool               s_scan_advance       = false;
static uint8_t            s_scan_next_query    = 0x00;
static bool               s_scan_got_any       = false;
static obd2_scan_result_t s_scan_result        = {0};
static obd2_scan_cb_t     s_scan_cb            = NULL;
static void              *s_scan_user          = NULL;

/* Forward decls */
static void _poll_timer_cb(lv_timer_t *t);
static void _send_pid_request(uint8_t service, uint8_t pid);
static void _register_pid_signal(const obd2_pid_def_t *def);
static void _scan_send(uint8_t pid);
static void _scan_finalize(bool completed);

/* ── Period helpers ───────────────────────────────────────────────────── */

static uint16_t _alive_period_for(const obd2_pid_def_t *def)
{
    if (!def) return OBD2_PERIOD_SLOW_MS;
    return (def->tier == OBD2_TIER_FAST) ? OBD2_PERIOD_FAST_MS
                                         : OBD2_PERIOD_SLOW_MS;
}

/* Per tick, mark each PID alive/dead based on its last response time.
 * Cheap O(n) walk; n is <= OBD2_MAX_ENABLED (48). */
static void _refresh_periods(uint64_t now)
{
    for (uint8_t i = 0; i < s_poll_count; i++) {
        obd2_poll_state_t *ps = &s_poll[i];
        const obd2_pid_def_t *def = obd2_pid_find(ps->pid);
        bool alive = (ps->last_response_ms != 0) &&
                     ((now - ps->last_response_ms) < OBD2_DEAD_THRESHOLD_MS);
        ps->target_period_ms = alive ? _alive_period_for(def)
                                     : OBD2_PERIOD_DEAD_MS;
    }
}

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

void obd2_init(void)
{
    if (!s_running && s_poll_count == 0) {
        memset(s_poll, 0, sizeof(s_poll));
    }
}

void obd2_start(const uint8_t *enabled_pids, uint8_t count)
{
    /* Preserve per-PID state for PIDs that are still in the new list —
     * keeps last_response_ms / "alive" classification across saves so a
     * user toggling unrelated PIDs doesn't reset everyone's freshness. */
    obd2_poll_state_t prev[OBD2_MAX_ENABLED];
    uint8_t prev_count = s_poll_count;
    memcpy(prev, s_poll, sizeof(prev));

    obd2_stop();

    if (count > OBD2_MAX_ENABLED) count = OBD2_MAX_ENABLED;
    s_poll_count = 0;
    for (uint8_t i = 0; i < count; i++) {
        const obd2_pid_def_t *def = obd2_pid_find(enabled_pids[i]);
        if (!def) {
            ESP_LOGW(TAG, "Skipping unknown PID 0x%02X in enable list",
                     enabled_pids[i]);
            continue;
        }
        obd2_poll_state_t *ns = &s_poll[s_poll_count++];
        memset(ns, 0, sizeof(*ns));
        ns->pid              = enabled_pids[i];
        ns->service          = obd2_def_service(def);
        ns->target_period_ms = _alive_period_for(def);
        for (uint8_t j = 0; j < prev_count; j++) {
            if (prev[j].pid == enabled_pids[i]) {
                ns->last_tx_ms       = prev[j].last_tx_ms;
                ns->last_response_ms = prev[j].last_response_ms;
                break;
            }
        }
        _register_pid_signal(def);
    }

    if (s_poll_count == 0) {
        ESP_LOGI(TAG, "No PIDs enabled — polling stays idle");
        return;
    }

    s_poll_timer = lv_timer_create(_poll_timer_cb, OBD2_TICK_MS, NULL);
    s_running = true;
    ESP_LOGI(TAG, "Started polling %u PIDs (pipelined, adaptive)", s_poll_count);
}

void obd2_stop(void)
{
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
    s_running = false;
    /* Don't clear s_poll[] here — obd2_get_enabled() may be queried by
     * UI code after stop, and obd2_start() repopulates from scratch. */
    ESP_LOGI(TAG, "Stopped polling");
}

bool obd2_is_running(void)
{
    return s_running;
}

/* ── Enabled list ──────────────────────────────────────────────────────── */

uint8_t obd2_get_enabled(uint8_t *out, uint8_t max)
{
    uint8_t n = s_poll_count < max ? s_poll_count : max;
    if (out && n) {
        for (uint8_t i = 0; i < n; i++) out[i] = s_poll[i].pid;
    }
    return n;
}

void obd2_set_enabled(const uint8_t *pids, uint8_t count)
{
    obd2_start(pids, count);
}

/* ── Signal registration ───────────────────────────────────────────────── */

static void _register_pid_signal(const obd2_pid_def_t *def)
{
    if (!def || !def->signal_name) return;
    if (signal_find_by_name(def->signal_name) >= 0) return;

    int16_t idx = signal_register(def->signal_name,
                                  /*can_id=*/0,
                                  /*bit_start=*/0,
                                  /*bit_length=*/0,
                                  /*scale=*/1.0f,
                                  /*offset=*/0.0f,
                                  /*is_signed=*/false,
                                  /*endian=*/1,
                                  def->unit);
    if (idx < 0) {
        ESP_LOGW(TAG, "Failed to register OBD2 signal '%s'", def->signal_name);
    }
}

/* ── Polling timer — starvation-based scheduler ───────────────────────── */

static void _poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);

    /* Discovery scan owns the bus until its window closes. */
    if (s_scan_state == SCAN_WINDOW_OPEN) {
        if ((now - s_scan_window_start) >= OBD2_SCAN_WINDOW_MS) {
            if (s_scan_advance) {
                s_scan_advance = false;
                _scan_send(s_scan_next_query);
            } else {
                _scan_finalize(s_scan_got_any);
            }
        }
        return;
    }

    if (s_poll_count == 0) return;

    _refresh_periods(now);

    /* Pick the most "starved" PID — the one that's gone longest past
     * its target poll period without being sent. Ensures fast-tier PIDs
     * naturally get the most attention while dead PIDs only get probed
     * every 5 seconds. */
    int      best_idx        = -1;
    int64_t  best_starvation = INT64_MIN;
    for (uint8_t i = 0; i < s_poll_count; i++) {
        obd2_poll_state_t *ps = &s_poll[i];
        int64_t since_tx = (int64_t)(now - ps->last_tx_ms);
        int64_t starvation = since_tx - (int64_t)ps->target_period_ms;
        if (starvation > best_starvation) {
            best_starvation = starvation;
            best_idx        = i;
        }
    }

    if (best_idx < 0 || best_starvation < 0) return;

    obd2_poll_state_t *ps = &s_poll[best_idx];
    _send_pid_request(ps->service, ps->pid);
    ps->last_tx_ms = now;
}

static void _send_pid_request(uint8_t service, uint8_t pid)
{
    /* ISO 15765-4 single-frame request:
     *   byte 0: length (2 for Mode 01)
     *   byte 1: service (0x01 for Mode 01, 0x22 for Mode 22)
     *   byte 2: PID
     *   byte 3..7: padding (0x55 conventional per spec, 0x00 works)
     *
     * Mode 22 has 16-bit PIDs and uses length=3 + two PID bytes —
     * would extend this path. */
    if (service == 0) service = 0x01;
    uint8_t data[8] = { 0x02, service, pid, 0x55, 0x55, 0x55, 0x55, 0x55 };
    esp_err_t err = can_transmit_frame(OBD2_REQUEST_ID_BROADCAST, data, 8);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "TX failed for service 0x%02X PID 0x%02X: %s",
                 service, pid, esp_err_to_name(err));
    }
}

/* ── RX handler ────────────────────────────────────────────────────────── */

static void _decode_and_push(const obd2_pid_def_t *def, const uint8_t *payload)
{
    uint32_t raw;
    if (def->bytes == 2) {
        raw = ((uint32_t)payload[0] << 8) | payload[1];
    } else {
        raw = payload[0];
    }
    float value = (float)raw * def->scale + def->offset;
    signal_set_external_value(def->signal_name, value);
}

/* Merge a Mode 01 supported-PID bitmask into the scan result. Each
 * bitmask is 4 bytes representing PIDs in the next 32-PID block. Top
 * bit of byte 0 = (base + 1), bottom bit of byte 3 = (base + 32).
 * Dedup against existing entries — multiple ECUs may report overlapping
 * PIDs (engine + trans + hybrid all support speed, for instance). */
static void _scan_consume_bitmask(uint8_t base_pid, const uint8_t *bm)
{
    for (int i = 0; i < 32; i++) {
        int byte = i / 8;
        int bit  = 7 - (i % 8);
        if (!(bm[byte] & (1u << bit))) continue;
        uint8_t pid = base_pid + 1 + i;
        bool dup = false;
        for (uint8_t j = 0; j < s_scan_result.count; j++) {
            if (s_scan_result.pids[j] == pid) { dup = true; break; }
        }
        if (!dup && s_scan_result.count < OBD2_SCAN_MAX_PIDS) {
            s_scan_result.pids[s_scan_result.count++] = pid;
        }
    }
}

void obd2_rx_handler(uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    if (can_id < OBD2_RESPONSE_ID_FIRST || can_id > OBD2_RESPONSE_ID_LAST) return;
    if (!data || dlc < 3) return;

    /* ISO-TP single-frame layout for live PID responses:
     *   byte 0 = 0x0N (N = payload length, upper nibble must be 0)
     *   byte 1 = service + 0x40 (0x41 for Mode 01, 0x62 for Mode 22)
     *   byte 2 = PID echo
     *   bytes 3.. = data
     *
     * Multi-frame responses (length nibble != 0) are Mode 22 / DTC / VIN
     * territory — out of scope for now. */
    uint8_t len_nibble = data[0] >> 4;
    uint8_t len_bytes  = data[0] & 0x0F;
    if (len_nibble != 0) return;
    if (len_bytes < 3 || len_bytes > 7) return;
    uint8_t resp_service = data[1];
    if ((resp_service & 0xC0) != 0x40) return;   /* not a positive response */
    if (resp_service != 0x41) return;            /* only Mode 01 wired for now */
    if (dlc < (uint8_t)(1 + len_bytes)) return;

    uint8_t pid = data[2];

    /* ── Discovery scan: collect into the open window. ───────────────── */
    if (s_scan_state == SCAN_WINDOW_OPEN && pid == s_scan_query_pid &&
        (pid == 0x00 || pid == 0x20 || pid == 0x40 || pid == 0x60 ||
         pid == 0x80 || pid == 0xA0 || pid == 0xC0 || pid == 0xE0)) {
        if (len_bytes >= 6) {
            s_scan_got_any = true;
            _scan_consume_bitmask(pid, &data[3]);
            /* Bottom bit of the last byte indicates whether the next
             * 32-PID block is supported by *this* ECU. We OR across
             * all responders — if any ECU says yes, we advance to the
             * next query when the window closes. */
            bool next_supported = (data[6] & 0x01) != 0;
            uint8_t next_pid = pid + 0x20;
            if (next_supported && next_pid <= 0xC0) {
                s_scan_advance    = true;
                s_scan_next_query = next_pid;
            }
        }
        return;
    }

    /* ── Regular live-data response. ─────────────────────────────────── */
    const obd2_pid_def_t *def = obd2_pid_find(pid);
    if (!def) return;

    uint8_t expected_payload = (uint8_t)(len_bytes - 2);
    if (expected_payload < def->bytes) return;

    _decode_and_push(def, &data[3]);

    /* Mark every matching enabled-PID slot as freshly-responded. With
     * pipelining there's no single "pending" — responses just close out
     * whatever pid they happen to match. */
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);
    for (uint8_t i = 0; i < s_poll_count; i++) {
        if (s_poll[i].pid == pid) {
            s_poll[i].last_response_ms = now;
            break;
        }
    }
}

/* ── Discovery scan ────────────────────────────────────────────────────── */

void obd2_discovery_start(obd2_scan_cb_t cb, void *user)
{
    if (s_scan_state != SCAN_IDLE) {
        ESP_LOGW(TAG, "Discovery already in progress");
        return;
    }
    memset(&s_scan_result, 0, sizeof(s_scan_result));
    s_scan_cb        = cb;
    s_scan_user      = user;
    s_scan_advance   = false;
    s_scan_next_query = 0x00;
    s_scan_got_any   = false;

    /* Make sure the poll timer exists so the scan state machine ticks. */
    if (!s_poll_timer) {
        s_poll_timer = lv_timer_create(_poll_timer_cb, OBD2_TICK_MS, NULL);
    }
    _scan_send(0x00);
}

bool obd2_discovery_in_progress(void)
{
    return s_scan_state != SCAN_IDLE;
}

static void _scan_send(uint8_t pid)
{
    s_scan_query_pid    = pid;
    s_scan_window_start = (uint64_t)(esp_timer_get_time() / 1000ULL);
    s_scan_state        = SCAN_WINDOW_OPEN;
    /* Discovery always uses Mode 01 — the supported-PID bitmask query
     * is part of the standard J1979 service. */
    _send_pid_request(0x01, pid);
}

static void _scan_finalize(bool completed)
{
    s_scan_result.completed = completed;
    s_scan_state            = SCAN_IDLE;

    obd2_scan_cb_t cb  = s_scan_cb;
    void          *usr = s_scan_user;
    s_scan_cb   = NULL;
    s_scan_user = NULL;

    ESP_LOGI(TAG, "Discovery complete (%s): %u PIDs supported",
             completed ? "OK" : "NO_RESPONSE", s_scan_result.count);

    if (cb) cb(&s_scan_result, usr);

    /* If polling wasn't running before the scan, retire the timer. */
    if (!s_running && s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
}
