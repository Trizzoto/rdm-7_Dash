/**
 * obd2.c — OBD2 polling + decode (see obd2.h).
 *
 * Implements Mode 01 single-frame requests over ISO 15765-4 CAN with a
 * round-robin poll cadence. The poll cycle alternates between fast-tier
 * and slow-tier PIDs:
 *   - Each cycle: every enabled fast PID once + ONE slow PID (rotating).
 *   - Tick period: 50 ms.
 * So with 7 fast + 23 slow PIDs you get ~10 Hz on fast and ~1 Hz on slow,
 * which is the J1979 sweet spot.
 *
 * Threading: everything runs on the LVGL task — the timer is an LVGL
 * timer, the RX hook is called from can_process_queued_frames (also LVGL).
 * No locks needed inside; can_transmit_frame is thread-safe.
 */
#include "obd2.h"

#include "can_manager.h"
#include "signal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <string.h>

static const char *TAG = "obd2";

#define OBD2_TICK_MS         50
#define OBD2_REQUEST_TIMEOUT 200    /* ms — drop pending if no response */

/* ── Module state ─────────────────────────────────────────────────────── */

static uint8_t        s_enabled[OBD2_MAX_ENABLED];
static uint8_t        s_enabled_count = 0;
static lv_timer_t    *s_poll_timer    = NULL;
static bool           s_running       = false;

/* Round-robin index into s_enabled. We sub-divide each tick: cycle through
 * one fast PID per tick, and inject one slow PID every Nth tick. */
static uint8_t        s_fast_idx = 0;
static uint8_t        s_slow_idx = 0;
static uint32_t       s_tick_count = 0;

/* Pending request — used to gate the next TX until response/timeout. */
static uint8_t        s_pending_pid       = 0xFF;
static uint64_t       s_pending_sent_ms   = 0;

/* Discovery scan state — runs on the same timer in a sub-state machine. */
typedef enum {
    SCAN_IDLE = 0,
    SCAN_WAIT_RESPONSE,
    SCAN_DONE,
} obd2_scan_state_t;

static obd2_scan_state_t s_scan_state    = SCAN_IDLE;
static uint8_t           s_scan_next_pid = 0x00;
static obd2_scan_result_t s_scan_result  = {0};
static obd2_scan_cb_t    s_scan_cb       = NULL;
static void             *s_scan_user     = NULL;
static uint64_t          s_scan_sent_ms  = 0;
#define SCAN_TIMEOUT_MS  400

/* Forward decls */
static void _poll_timer_cb(lv_timer_t *t);
static void _send_pid_request(uint8_t pid);
static void _register_pid_signal(const obd2_pid_def_t *def);
static void _scan_send_next(void);
static void _scan_finalize(bool completed);

/* ── Lifecycle ─────────────────────────────────────────────────────────── */

void obd2_init(void)
{
    /* Idempotent: timer is allocated lazily in obd2_start. */
    if (!s_running && s_enabled_count == 0) {
        memset(s_enabled, 0, sizeof(s_enabled));
    }
}

void obd2_start(const uint8_t *enabled_pids, uint8_t count)
{
    obd2_stop();

    if (count > OBD2_MAX_ENABLED) count = OBD2_MAX_ENABLED;
    s_enabled_count = 0;
    for (uint8_t i = 0; i < count; i++) {
        const obd2_pid_def_t *def = obd2_pid_find(enabled_pids[i]);
        if (!def) {
            ESP_LOGW(TAG, "Skipping unknown PID 0x%02X in enable list",
                     enabled_pids[i]);
            continue;
        }
        s_enabled[s_enabled_count++] = enabled_pids[i];
        _register_pid_signal(def);
    }

    if (s_enabled_count == 0) {
        ESP_LOGI(TAG, "No PIDs enabled — polling stays idle");
        return;
    }

    s_fast_idx        = 0;
    s_slow_idx        = 0;
    s_tick_count      = 0;
    s_pending_pid     = 0xFF;
    s_pending_sent_ms = 0;

    s_poll_timer = lv_timer_create(_poll_timer_cb, OBD2_TICK_MS, NULL);
    s_running = true;
    ESP_LOGI(TAG, "Started polling %u PIDs", s_enabled_count);
}

void obd2_stop(void)
{
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
    s_running     = false;
    s_pending_pid = 0xFF;
    /* Don't clear s_enabled[] here — obd2_get_enabled() may be queried by
     * UI code after stop. obd2_start() will overwrite when restarting. */
    ESP_LOGI(TAG, "Stopped polling");
}

bool obd2_is_running(void)
{
    return s_running;
}

/* ── Enabled list ──────────────────────────────────────────────────────── */

uint8_t obd2_get_enabled(uint8_t *out, uint8_t max)
{
    uint8_t n = s_enabled_count < max ? s_enabled_count : max;
    if (out && n) memcpy(out, s_enabled, n);
    return n;
}

void obd2_set_enabled(const uint8_t *pids, uint8_t count)
{
    obd2_start(pids, count);
}

/* ── Signal registration ───────────────────────────────────────────────── */

/* Register an OBD2-driven signal in the registry if not already present.
 * Uses can_id=0 to signal "external value source" — signal_dispatch_frame
 * skips can_id==0 because real CAN IDs are never zero. The signal carries
 * name + unit only; bit-decoding fields are unused.
 *
 * NOTE: if a native preset already registered a signal with this name
 * (e.g. RPM), we leave it alone — the preset path wins, and supplemental
 * OBD2 should never claim a conflicting name. The picker UI prevents this
 * but we double-guard here. */
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

/* ── Polling timer ─────────────────────────────────────────────────────── */

/* Split enabled[] into fast and slow groups in-place each tick is cheap
 * — but easier: enumerate and pick by tier on each cycle pass. With <=48
 * PIDs total this is trivial cost. */
static int _next_fast(uint8_t start)
{
    for (uint8_t i = 0; i < s_enabled_count; i++) {
        uint8_t k = (start + i) % s_enabled_count;
        const obd2_pid_def_t *d = obd2_pid_find(s_enabled[k]);
        if (d && d->tier == OBD2_TIER_FAST) return k;
    }
    return -1;
}

static int _next_slow(uint8_t start)
{
    for (uint8_t i = 0; i < s_enabled_count; i++) {
        uint8_t k = (start + i) % s_enabled_count;
        const obd2_pid_def_t *d = obd2_pid_find(s_enabled[k]);
        if (d && d->tier == OBD2_TIER_SLOW) return k;
    }
    return -1;
}

static void _poll_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);

    /* If a discovery scan is in flight, it owns the bus until done. */
    if (s_scan_state != SCAN_IDLE) {
        if (s_scan_state == SCAN_WAIT_RESPONSE &&
            (now - s_scan_sent_ms) > SCAN_TIMEOUT_MS) {
            ESP_LOGW(TAG, "Scan timed out waiting for PID 0x%02X",
                     s_scan_next_pid);
            _scan_finalize(false);
        }
        return;
    }

    /* Pending request timeout — drop and move on. Lets a missing ECU not
     * stall the poll cycle indefinitely. */
    if (s_pending_pid != 0xFF) {
        if ((now - s_pending_sent_ms) > OBD2_REQUEST_TIMEOUT) {
            ESP_LOGD(TAG, "Timeout on PID 0x%02X", s_pending_pid);
            s_pending_pid = 0xFF;
        } else {
            return; /* still waiting for response */
        }
    }

    if (s_enabled_count == 0) return;

    s_tick_count++;

    /* Schedule: every tick send one fast PID. Every 10th tick send one
     * slow PID instead — so slow PIDs share their tick with the round
     * the slow rotation advances. This keeps fast cadence at ~10 Hz and
     * slow at ~1 Hz with even spacing. */
    uint8_t target_pid = 0xFF;
    bool prefer_slow = (s_tick_count % 10 == 0);

    if (prefer_slow) {
        int k = _next_slow(s_slow_idx);
        if (k >= 0) {
            target_pid = s_enabled[k];
            s_slow_idx = (k + 1) % s_enabled_count;
        } else {
            /* No slow PIDs — fall through to fast. */
            int kf = _next_fast(s_fast_idx);
            if (kf >= 0) {
                target_pid = s_enabled[kf];
                s_fast_idx = (kf + 1) % s_enabled_count;
            }
        }
    } else {
        int k = _next_fast(s_fast_idx);
        if (k >= 0) {
            target_pid = s_enabled[k];
            s_fast_idx = (k + 1) % s_enabled_count;
        } else {
            /* No fast PIDs — fall through to slow. */
            int ks = _next_slow(s_slow_idx);
            if (ks >= 0) {
                target_pid = s_enabled[ks];
                s_slow_idx = (ks + 1) % s_enabled_count;
            }
        }
    }

    if (target_pid != 0xFF) {
        _send_pid_request(target_pid);
        s_pending_pid     = target_pid;
        s_pending_sent_ms = now;
    }
}

static void _send_pid_request(uint8_t pid)
{
    /* ISO 15765-4 single-frame Mode 01 request:
     *   byte 0: length (2)
     *   byte 1: 0x01 (Mode 01)
     *   byte 2: PID
     *   byte 3..7: padding (0x55 per spec is conventional, 0x00 works) */
    uint8_t data[8] = { 0x02, 0x01, pid, 0x55, 0x55, 0x55, 0x55, 0x55 };
    esp_err_t err = can_transmit_frame(OBD2_REQUEST_ID_BROADCAST, data, 8);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "TX failed for PID 0x%02X: %s", pid, esp_err_to_name(err));
    }
}

/* ── RX handler ────────────────────────────────────────────────────────── */

/* Decode + push value into the signal registry. */
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

/* Aggregate scan-bitmask response into s_scan_result.pids[]. Each bitmask
 * is 4 bytes representing PIDs in the next 32-PID block. Top bit of byte 0
 * = (base + 1), bottom bit of byte 3 = (base + 32). */
static void _scan_consume_bitmask(uint8_t base_pid, const uint8_t *bm)
{
    for (int i = 0; i < 32; i++) {
        int byte = i / 8;
        int bit  = 7 - (i % 8);
        if (bm[byte] & (1u << bit)) {
            uint8_t pid = base_pid + 1 + i;
            if (s_scan_result.count < OBD2_SCAN_MAX_PIDS) {
                s_scan_result.pids[s_scan_result.count++] = pid;
            }
        }
    }
}

void obd2_rx_handler(uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    if (can_id < OBD2_RESPONSE_ID_FIRST || can_id > OBD2_RESPONSE_ID_LAST) return;
    if (!data || dlc < 3) return;

    /* ISO-TP single-frame layout for live PID responses:
     *   byte 0 = 0x0N (N = payload length, upper nibble must be 0)
     *   byte 1 = 0x41 (Mode 01 + 0x40)
     *   byte 2 = PID echo
     *   bytes 3.. = data */
    uint8_t len_nibble = data[0] >> 4;
    uint8_t len_bytes  = data[0] & 0x0F;
    if (len_nibble != 0) return;          /* multi-frame — defer to v2 */
    if (len_bytes < 3 || len_bytes > 7) return;
    if (data[1] != 0x41) return;          /* not a Mode 01 positive response */
    if (dlc < (uint8_t)(1 + len_bytes)) return;

    uint8_t pid = data[2];

    /* Scan in progress? Capture the bitmask response. */
    if (s_scan_state == SCAN_WAIT_RESPONSE &&
        (pid == 0x00 || pid == 0x20 || pid == 0x40 || pid == 0x60 ||
         pid == 0x80 || pid == 0xA0 || pid == 0xC0 || pid == 0xE0)) {
        if (len_bytes >= 6) {
            _scan_consume_bitmask(pid, &data[3]);
            /* Bottom bit of the last byte (data[6]) indicates whether the
             * next block (pid + 0x20) is supported. If yes, send the next
             * bitmask query; otherwise we're done. */
            bool next_supported = (data[6] & 0x01) != 0;
            uint8_t next_pid = pid + 0x20;
            if (next_supported && next_pid <= 0xC0) {
                s_scan_next_pid = next_pid;
                _scan_send_next();
            } else {
                _scan_finalize(true);
            }
        }
        return;
    }

    /* Regular live-data response → look up decoder, push value. */
    const obd2_pid_def_t *def = obd2_pid_find(pid);
    if (!def) return;

    uint8_t expected_payload = (uint8_t)(len_bytes - 2); /* minus 0x41 + PID */
    if (expected_payload < def->bytes) return;

    _decode_and_push(def, &data[3]);

    /* Clear pending if this matches what we last requested. ECUs sometimes
     * answer on different IDs (0x7E9, 0x7EA) than 0x7E8 — we accept any. */
    if (pid == s_pending_pid) s_pending_pid = 0xFF;
}

/* ── Discovery scan ────────────────────────────────────────────────────── */

void obd2_discovery_start(obd2_scan_cb_t cb, void *user)
{
    if (s_scan_state != SCAN_IDLE) {
        ESP_LOGW(TAG, "Discovery already in progress");
        return;
    }
    memset(&s_scan_result, 0, sizeof(s_scan_result));
    s_scan_cb       = cb;
    s_scan_user     = user;
    s_scan_next_pid = 0x00;
    s_scan_state    = SCAN_WAIT_RESPONSE;

    /* Make sure the poll timer exists so the scan state machine ticks. */
    if (!s_poll_timer) {
        s_poll_timer = lv_timer_create(_poll_timer_cb, OBD2_TICK_MS, NULL);
    }
    _scan_send_next();
}

bool obd2_discovery_in_progress(void)
{
    return s_scan_state != SCAN_IDLE;
}

static void _scan_send_next(void)
{
    _send_pid_request(s_scan_next_pid);
    s_scan_sent_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    s_scan_state   = SCAN_WAIT_RESPONSE;
}

static void _scan_finalize(bool completed)
{
    s_scan_result.completed = completed;
    s_scan_state            = SCAN_IDLE;

    obd2_scan_cb_t cb   = s_scan_cb;
    void          *usr  = s_scan_user;
    s_scan_cb   = NULL;
    s_scan_user = NULL;

    ESP_LOGI(TAG, "Discovery complete (%s): %u PIDs supported",
             completed ? "OK" : "TIMEOUT", s_scan_result.count);

    if (cb) cb(&s_scan_result, usr);

    /* If polling wasn't running before the scan, retire the timer. */
    if (!s_running && s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
}
