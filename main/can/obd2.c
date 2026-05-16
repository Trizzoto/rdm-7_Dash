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

/* Mode 01 supports up to 6 PIDs per request (length nibble allows 7
 * bytes after PCI; service byte takes 1, leaving 6 PIDs). Batching
 * reduces bus chatter ~2.4x and cuts effective refresh latency in
 * half on cars with several enabled signals. */
#define OBD2_MAX_BATCH_PIDS          6

/* Minimum gap between successive TX. The premium ELM327 STN-chipset
 * adapters call this their "adaptive timing minimum"; it prevents
 * spamming the ECU faster than its receive ISR can keep up.
 * 5 ms = max 200 req/sec which is way past any ECU's response rate.
 * Real-world bus rate is capped by the ECU's response speed, not
 * our TX schedule. */
#define OBD2_MIN_INTER_TX_MS         5

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

/* ISO-TP multi-frame RX. Toyota Mode 21 PID 0x80 returns ~24-28 bytes
 * (an FF + 3 CFs). Buffer comfortably above the largest expected
 * response. Single in-flight stream — if two ECUs answer the same
 * multi-frame request we lose the second; in practice that's fine for
 * the engine-ECU-targeted Mode 21 requests we use. */
#define OBD2_ISOTP_BUF_LEN           64
#define OBD2_ISOTP_TIMEOUT_MS       500

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

/* Last TX timestamp across the whole module — used by _try_dispatch to
 * enforce the OBD2_MIN_INTER_TX_MS floor. Reset on obd2_start so a fresh
 * polling session immediately fires its first request. */
static uint64_t s_last_tx_ms_global = 0;

/* ── ISO-TP RX (single in-flight stream) ──────────────────────────────── */

typedef enum {
    ISOTP_IDLE = 0,
    ISOTP_RECEIVING,
} obd2_isotp_state_t;

static struct {
    obd2_isotp_state_t state;
    uint32_t           source_id;
    uint8_t            buf[OBD2_ISOTP_BUF_LEN];
    uint16_t           total_bytes;     /* expected total payload length */
    uint16_t           received;
    uint8_t            next_seq;        /* 0-15, wraps */
    uint64_t           last_frame_ms;
} s_isotp = {0};

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
static void _try_dispatch(uint64_t now);
static void _send_pid_request(uint8_t service, uint8_t pid);
static void _send_multi_pid_request(uint8_t service, const uint8_t *pids, uint8_t n);
static void _register_pid_signal(const obd2_pid_def_t *def);
static void _scan_send(uint8_t pid);
static void _scan_finalize(bool completed);
static void _send_flow_control(uint32_t target_id);
static void _process_full_message(uint8_t *msg, uint16_t len);
static uint32_t _request_id_for_response(uint32_t resp_id);
static const obd2_pid_def_t *_find_for_service(uint8_t service, uint8_t pid);

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
        /* Conflict guard: if a signal with this name already exists and
         * is bound to a real CAN broadcast (can_id != 0), the native
         * preset owns it — don't poll OBD2 too, or the response would
         * overwrite the broadcast value via signal_set_external_value.
         * Picker UI prevents the user from selecting these but the
         * modal's preview-poll-all path could pass them through. */
        int16_t existing = signal_find_by_name(def->signal_name);
        if (existing >= 0) {
            signal_t *sig = signal_get_by_index((uint16_t)existing);
            if (sig && sig->can_id != 0) {
                ESP_LOGD(TAG, "Skip OBD2 PID 0x%02X — '%s' owned by preset",
                         def->pid, def->signal_name);
                continue;
            }
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

    s_last_tx_ms_global = 0;     /* fire first request immediately */
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
    if (!def) return;

    /* Packed multi-value PID (Mode 21 Toyota engine block, future Mode
     * 22 OEM blocks): register one signal per sub-field. Without this,
     * decoded values get dropped by signal_set_external_value because
     * the names don't exist in the registry. */
    if (def->sub_fields && def->sub_field_count > 0) {
        for (uint8_t i = 0; i < def->sub_field_count; i++) {
            const obd2_subfield_t *sf = &def->sub_fields[i];
            if (!sf->signal_name) continue;
            if (signal_find_by_name(sf->signal_name) >= 0) continue;
            int16_t idx = signal_register(sf->signal_name,
                                          /*can_id=*/0,
                                          /*bit_start=*/0,
                                          /*bit_length=*/0,
                                          /*scale=*/1.0f,
                                          /*offset=*/0.0f,
                                          sf->is_signed,
                                          /*endian=*/1,
                                          sf->unit ? sf->unit : "");
            if (idx < 0) {
                ESP_LOGW(TAG, "Failed to register OBD2 signal '%s'",
                         sf->signal_name);
            }
        }
        return;
    }

    /* Legacy single-value PID. */
    if (!def->signal_name) return;
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

/* ── Scheduler ─────────────────────────────────────────────────────────
 *
 * The poll timer is a watchdog/safety net at 30 ms — but the real
 * dispatch driver is _try_dispatch, which is called from both:
 *   - the poll timer (so polling kicks off even if no responses arrive)
 *   - the RX handler after each response (so the next request fires
 *     immediately, not after waiting for the next 30 ms tick)
 *
 * This response-triggered behaviour mirrors the "pending response" mode
 * of premium ELM327 / STN-chipset adapters: the bus stays saturated up
 * to the ECU's natural response rate instead of being throttled by our
 * timer. Typical effect: 30-50 Hz round-trips instead of 33 Hz capped.
 *
 * Hard floor: OBD2_MIN_INTER_TX_MS gap between TX so we can never spam
 * an ECU faster than ~200 req/sec even if responses are arriving in
 * back-to-back IRQs. */

/* Pick the best PID to send right now and TX (single or batched).
 * No-op if scan in progress, ISO-TP still reassembling, nothing due,
 * or we're within the min-inter-TX window. */
static void _try_dispatch(uint64_t now)
{
    if (s_scan_state != SCAN_IDLE) return;
    if (s_isotp.state == ISOTP_RECEIVING) return;
    if ((now - s_last_tx_ms_global) < OBD2_MIN_INTER_TX_MS) return;
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

    obd2_poll_state_t *top = &s_poll[best_idx];

    /* Mode 21+ (Toyota etc.): always single-PID. Multi-PID is a Mode 01
     * spec feature; other services need their own framing. */
    if (top->service != 0x01) {
        _send_pid_request(top->service, top->pid);
        top->last_tx_ms = now;
        s_last_tx_ms_global = now;
        return;
    }

    /* Mode 01: batch up to OBD2_MAX_BATCH_PIDS due+alive PIDs into one
     * request. Cuts bus traffic ~2.4x vs polling each PID individually
     * and shrinks effective latency because the car sends one ISO-TP
     * stream instead of N single-frame responses. Dead PIDs stay on
     * their 5-sec individual probe schedule (excluded from batches —
     * including them risks NRC from ECUs that reject the whole batch
     * when any one PID is unknown). */
    uint8_t batch[OBD2_MAX_BATCH_PIDS];
    uint8_t batch_n = 0;
    batch[batch_n++] = top->pid;
    top->last_tx_ms = now;

    for (uint8_t j = 0; j < s_poll_count && batch_n < OBD2_MAX_BATCH_PIDS; j++) {
        if (j == best_idx) continue;
        obd2_poll_state_t *ps = &s_poll[j];
        if (ps->service != 0x01) continue;
        if (ps->target_period_ms >= OBD2_PERIOD_DEAD_MS) continue;
        int64_t starv = (int64_t)(now - ps->last_tx_ms)
                        - (int64_t)ps->target_period_ms;
        if (starv < 0) continue;     /* not yet due — let it wait */
        batch[batch_n++] = ps->pid;
        ps->last_tx_ms = now;
    }

    if (batch_n == 1) {
        _send_pid_request(0x01, batch[0]);
    } else {
        _send_multi_pid_request(0x01, batch, batch_n);
    }
    s_last_tx_ms_global = now;
}

/* Periodic safety-net tick. Drives ISO-TP timeout, scan window advance,
 * and a baseline call to _try_dispatch so polling restarts even after
 * silent ECUs. _try_dispatch also fires from obd2_rx_handler on every
 * response so the bus stays saturated between ticks. */
static void _poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);

    /* ISO-TP RX timeout — if we sent a flow control then never got the
     * expected CFs, abandon and let the next poll re-request. Prevents
     * a stuck stream from gating future ECU responses. */
    if (s_isotp.state == ISOTP_RECEIVING &&
        (now - s_isotp.last_frame_ms) > OBD2_ISOTP_TIMEOUT_MS) {
        ESP_LOGD(TAG, "ISO-TP RX timeout from 0x%03lX (%u/%u bytes)",
                 (unsigned long)s_isotp.source_id,
                 (unsigned)s_isotp.received, (unsigned)s_isotp.total_bytes);
        s_isotp.state = ISOTP_IDLE;
    }

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

    _try_dispatch(now);
}

static void _send_pid_request(uint8_t service, uint8_t pid)
{
    /* ISO 15765-4 single-frame request:
     *   byte 0: length (2 for 8-bit PID)
     *   byte 1: service (0x01 Mode 01, 0x21 Mode 21, 0x22 Mode 22)
     *   byte 2: PID
     *   byte 3..7: padding (0x55 conventional)
     *
     * Mode 22 has 16-bit PIDs and uses length=3 + two PID bytes —
     * would extend this path when wired.
     *
     * Per-PID request_id override addresses a specific ECU (e.g. 0x7E0
     * = engine ECU). Used for Mode 21 to avoid NRCs from other ECUs
     * that don't recognise the PID. Default = broadcast 0x7DF. */
    if (service == 0) service = 0x01;
    const obd2_pid_def_t *def = _find_for_service(service, pid);
    uint32_t tx_id = (def && def->request_id) ? def->request_id
                                              : OBD2_REQUEST_ID_BROADCAST;
    uint8_t data[8] = { 0x02, service, pid, 0x55, 0x55, 0x55, 0x55, 0x55 };
    esp_err_t err = can_transmit_frame(tx_id, data, 8);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "TX failed for service 0x%02X PID 0x%02X: %s",
                 service, pid, esp_err_to_name(err));
    }
}

/* Build and send a Mode 01 multi-PID request. Frame layout:
 *   byte 0 = PCI: SF | length = N + 1 (service byte + N PID bytes)
 *   byte 1 = service (0x01)
 *   byte 2..(1+N) = PID bytes
 *   remaining = 0x55 padding
 * Up to OBD2_MAX_BATCH_PIDS PIDs fit in a single CAN frame. The
 * response comes back as concatenated [PID, data...] pairs which
 * _process_full_message walks via the Mode 01 multi-PID path. */
static void _send_multi_pid_request(uint8_t service, const uint8_t *pids, uint8_t n)
{
    if (!pids || n == 0 || n > OBD2_MAX_BATCH_PIDS) return;
    if (service == 0) service = 0x01;

    /* Per-PID request_id override: use the first PID's def (in practice
     * Mode 01 PIDs all broadcast on 0x7DF — no per-PID overrides — but
     * be defensive). */
    const obd2_pid_def_t *def0 = _find_for_service(service, pids[0]);
    uint32_t tx_id = (def0 && def0->request_id) ? def0->request_id
                                                : OBD2_REQUEST_ID_BROADCAST;

    uint8_t data[8] = {0};
    data[0] = (uint8_t)(n + 1);
    data[1] = service;
    memcpy(&data[2], pids, n);
    for (uint8_t i = (uint8_t)(2 + n); i < 8; i++) data[i] = 0x55;

    esp_err_t err = can_transmit_frame(tx_id, data, 8);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Multi-PID TX failed (%u PIDs, svc 0x%02X): %s",
                 n, service, esp_err_to_name(err));
    }
}

/* Find a PID definition matching both service and PID byte. Used by RX
 * decode (we know the service from the response code) and by TX setup
 * for the request_id override. Falls back to first-match-by-PID-byte
 * if no exact match exists, which preserves behaviour for the common
 * case where service==0 in the def is treated as Mode 01. */
static const obd2_pid_def_t *_find_for_service(uint8_t service, uint8_t pid)
{
    if (service == 0) service = 0x01;
    for (int i = 0; i < OBD2_PIDS_COUNT; i++) {
        if (OBD2_PIDS[i].pid != pid) continue;
        uint8_t s = OBD2_PIDS[i].service ? OBD2_PIDS[i].service : 0x01;
        if (s == service) return &OBD2_PIDS[i];
    }
    return obd2_pid_find(pid);
}

/* Send a flow-control frame back to the ECU that just sent us a first
 * frame. {0x30, BS=0 (no block limit), STmin=0 (no separation)} tells
 * the ECU to send all remaining consecutive frames at full speed.
 * target_id is the ECU's REQUEST id (inverse of its response id —
 * 0x7E8 response → 0x7E0 request). */
static void _send_flow_control(uint32_t target_id)
{
    uint8_t data[8] = { 0x30, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55 };
    esp_err_t err = can_transmit_frame(target_id, data, 8);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "ISO-TP FC TX failed: %s", esp_err_to_name(err));
    }
}

static uint32_t _request_id_for_response(uint32_t resp_id)
{
    if (resp_id >= OBD2_RESPONSE_ID_FIRST && resp_id <= OBD2_RESPONSE_ID_LAST) {
        return 0x7E0u + (resp_id - OBD2_RESPONSE_ID_FIRST);
    }
    return OBD2_REQUEST_ID_BROADCAST;
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
    if (!data || dlc < 2) return;

    uint8_t pci         = data[0];
    uint8_t pci_type    = pci >> 4;       /* 0=SF, 1=FF, 2=CF, 3=FC */
    uint64_t now        = (uint64_t)(esp_timer_get_time() / 1000ULL);

    if (pci_type == 0) {
        /* ── Single frame — payload fits in one CAN frame. ───────── */
        uint8_t len_bytes = pci & 0x0F;
        if (len_bytes < 2 || len_bytes > 7) return;
        if (dlc < (uint8_t)(1 + len_bytes)) return;
        _process_full_message((uint8_t *)&data[1], len_bytes);
        return;
    }

    if (pci_type == 1) {
        /* ── First frame — multi-frame response begins. ──────────── */
        if (dlc < 8) return;
        uint16_t total = (((uint16_t)(pci & 0x0F)) << 8) | data[1];
        if (total < 2 || total > OBD2_ISOTP_BUF_LEN) return;

        /* If we were mid-stream from another source, drop it. Single
         * in-flight stream — multi-ECU concurrent multi-frame would
         * collide anyway and Mode 21 requests are addressed. */
        s_isotp.state         = ISOTP_RECEIVING;
        s_isotp.source_id     = can_id;
        s_isotp.total_bytes   = total;
        s_isotp.received      = 0;
        s_isotp.next_seq      = 1;
        s_isotp.last_frame_ms = now;

        /* FF carries 6 payload bytes (data[2..7]). */
        uint8_t take = (total < 6) ? total : 6;
        memcpy(s_isotp.buf, &data[2], take);
        s_isotp.received = take;

        /* Ask the ECU to keep streaming consecutive frames. FC goes back
         * to the ECU's request id (response 0x7E8 → request 0x7E0). */
        _send_flow_control(_request_id_for_response(can_id));

        if (s_isotp.received >= s_isotp.total_bytes) {
            _process_full_message(s_isotp.buf, s_isotp.received);
            s_isotp.state = ISOTP_IDLE;
        }
        return;
    }

    if (pci_type == 2) {
        /* ── Consecutive frame. ──────────────────────────────────── */
        if (s_isotp.state != ISOTP_RECEIVING) return;
        if (s_isotp.source_id != can_id) return;
        uint8_t seq = pci & 0x0F;
        if (seq != s_isotp.next_seq) {
            ESP_LOGD(TAG, "ISO-TP seq mismatch: expected %u got %u",
                     s_isotp.next_seq, seq);
            s_isotp.state = ISOTP_IDLE;
            return;
        }
        s_isotp.next_seq      = (uint8_t)((seq + 1) & 0x0F);
        s_isotp.last_frame_ms = now;

        uint16_t remaining = s_isotp.total_bytes - s_isotp.received;
        uint8_t  avail     = (dlc > 1) ? (uint8_t)(dlc - 1) : 0;
        uint8_t  take      = (remaining < avail) ? (uint8_t)remaining : avail;
        if (s_isotp.received + take > OBD2_ISOTP_BUF_LEN) {
            s_isotp.state = ISOTP_IDLE;
            return;
        }
        memcpy(&s_isotp.buf[s_isotp.received], &data[1], take);
        s_isotp.received += take;

        if (s_isotp.received >= s_isotp.total_bytes) {
            _process_full_message(s_isotp.buf, s_isotp.received);
            s_isotp.state = ISOTP_IDLE;
        }
        return;
    }
    /* pci_type == 3 = flow control we sent; ignore loopback. */
}

/* Extract a 1/2/4 byte big-endian value, optionally sign-extending. */
static int32_t _extract_bytes(const uint8_t *p, uint8_t bytes, bool is_signed)
{
    uint32_t v = 0;
    for (uint8_t i = 0; i < bytes; i++) {
        v = (v << 8) | p[i];
    }
    if (is_signed && bytes < 4) {
        uint32_t sign_bit = 1u << ((bytes * 8) - 1);
        if (v & sign_bit) v |= ~((sign_bit << 1) - 1);
    }
    return (int32_t)v;
}

/* Apply a packed PID definition to its sub-fields, pushing each value
 * into the signal registry. */
static void _decode_packed(const obd2_pid_def_t *def,
                           const uint8_t *payload, uint16_t payload_len)
{
    for (uint8_t i = 0; i < def->sub_field_count; i++) {
        const obd2_subfield_t *sf = &def->sub_fields[i];
        if ((uint16_t)(sf->byte_offset + sf->bytes) > payload_len) continue;
        int32_t raw = _extract_bytes(&payload[sf->byte_offset],
                                     sf->bytes, sf->is_signed);
        float value = (float)raw * sf->scale + sf->offset;
        signal_set_external_value(sf->signal_name, value);
    }
}

/* Process a fully-reassembled response message. `msg` points at the
 * service-echo byte (e.g. 0x41 / 0x61), `len` is total bytes including
 * service+PID echoes. Routes to discovery-scan handling, Mode 01
 * multi-PID walking, or Mode 21 packed/single-value decode. */
static void _process_full_message(uint8_t *msg, uint16_t len)
{
    if (len < 2) return;
    uint8_t resp_service = msg[0];
    if ((resp_service & 0xC0) != 0x40) return;   /* not a positive response */
    if (resp_service == 0x7F) return;            /* NRC (negative response) */

    uint8_t service = (uint8_t)(resp_service - 0x40);
    /* Only services we know how to decode. Add 0x22 here when Mode 22
     * (16-bit PIDs) lands — it'd need different framing on TX too. */
    if (service != 0x01 && service != 0x21) return;

    uint8_t first_pid = msg[1];

    /* ── Discovery scan: only relevant for Mode 01 bitmask queries. ── */
    if (s_scan_state == SCAN_WINDOW_OPEN && service == 0x01 &&
        first_pid == s_scan_query_pid &&
        (first_pid == 0x00 || first_pid == 0x20 || first_pid == 0x40 ||
         first_pid == 0x60 || first_pid == 0x80 || first_pid == 0xA0 ||
         first_pid == 0xC0 || first_pid == 0xE0)) {
        const uint8_t *payload = &msg[2];
        uint16_t payload_len = (uint16_t)(len - 2);
        if (payload_len >= 4) {
            s_scan_got_any = true;
            _scan_consume_bitmask(first_pid, payload);
            bool next_supported = (payload[3] & 0x01) != 0;
            uint8_t next_pid = (uint8_t)(first_pid + 0x20);
            if (next_supported && next_pid <= 0xC0) {
                s_scan_advance    = true;
                s_scan_next_query = next_pid;
            }
        }
        return;
    }

    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);

    /* ── Mode 01: walk as [PID, data...] pairs. ──────────────────────
     * Same code handles a single-PID response (N=1, one pass through
     * the loop) and a multi-PID response (N>1, one pass per PID).
     * Walks until either all bytes consumed or an unknown PID byte
     * appears (no decoder = can't know how many data bytes to skip). */
    if (service == 0x01) {
        const uint8_t *p = &msg[1];
        uint16_t rem = (uint16_t)(len - 1);
        while (rem >= 2) {
            uint8_t pid = p[0];
            const obd2_pid_def_t *def = _find_for_service(0x01, pid);
            if (!def) {
                ESP_LOGD(TAG, "Multi-PID resp: no decoder for 0x%02X, stopping walk", pid);
                return;
            }
            /* Packed sub_fields shouldn't appear under Mode 01 (only Mode 21
             * uses them) but guard anyway. */
            if (def->sub_fields && def->sub_field_count > 0) return;
            uint8_t consumed = (uint8_t)(1 + def->bytes);
            if (rem < consumed) return;

            _decode_and_push(def, &p[1]);

            for (uint8_t i = 0; i < s_poll_count; i++) {
                if (s_poll[i].pid == pid && s_poll[i].service == 0x01) {
                    s_poll[i].last_response_ms = now;
                    break;
                }
            }

            p   += consumed;
            rem -= consumed;
        }
        /* Response done — kick off next request immediately. See note
         * at end of function for the response-triggered dispatch path. */
        _try_dispatch(now);
        return;
    }

    /* ── Mode 21 (and future Mode 22): single-PID, possibly packed. ── */
    const obd2_pid_def_t *def = _find_for_service(service, first_pid);
    if (!def) return;
    const uint8_t *payload = &msg[2];
    uint16_t payload_len = (uint16_t)(len - 2);

    if (def->sub_fields && def->sub_field_count > 0) {
        _decode_packed(def, payload, payload_len);
    } else {
        if (payload_len < def->bytes) return;
        _decode_and_push(def, payload);
    }

    for (uint8_t i = 0; i < s_poll_count; i++) {
        if (s_poll[i].pid == first_pid && s_poll[i].service == service) {
            s_poll[i].last_response_ms = now;
            break;
        }
    }

    /* Response done — kick off next request immediately if anything else
     * is due. This is the response-triggered dispatch path that lets us
     * saturate the bus at the ECU's natural response rate instead of
     * waiting for the 30 ms timer tick (~3x throughput on busy buses). */
    _try_dispatch(now);
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
