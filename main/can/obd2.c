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
    uint16_t pid;              /* 8-bit for Mode 01/21, 16-bit for Mode 22 */
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

/* ── Custom PID storage ────────────────────────────────────────────────
 *
 * User-defined PIDs from the layout's custom_pids JSON array. Each entry
 * presents as a regular obd2_pid_def_t via the sub_field path (single
 * sub-field = legacy single-value semantics), so the polling loop,
 * picker, and widget binding treat custom PIDs identically to built-ins.
 *
 * String fields (signal_name, human_name, unit, category) are stored in
 * the entry struct itself for stable lifetime — pointers in the def
 * reference these buffers. */
typedef struct {
    obd2_pid_def_t  def;
    obd2_subfield_t sub_field;
    char            signal_name[32];
    char            human_name[48];
    char            unit[8];
    char            category[16];
} obd2_custom_entry_t;

static obd2_custom_entry_t s_custom[OBD2_MAX_CUSTOM_PIDS];
static uint8_t             s_custom_count = 0;

/* ── One-shot test state ───────────────────────────────────────────────
 *
 * Captures the next matching response after obd2_test_pid() fires its
 * request. Cleared by either the response intercept in
 * _process_full_message or the timeout check in _poll_timer_cb. */
#define OBD2_TEST_TIMEOUT_MS 500

static struct {
    bool            active;
    uint8_t         service;
    uint8_t         pid;
    uint64_t        sent_ms;
    obd2_test_cb_t  cb;
    void           *user;
    uint8_t         data_offset;
    uint8_t         data_bytes;
    float           scale;
    float           offset;
    bool            is_signed;
} s_test = {0};

/* ── DTC read request state (Modes 03 / 07 / 0A) ───────────────────────
 *
 * Single in-flight at a time. Accumulator buffer holds up to OBD2_MAX_DTCS
 * codes from one response. Multi-frame ISO-TP responses are reassembled
 * by the shared s_isotp path; we just decode the resulting payload. */
#define OBD2_DTC_TIMEOUT_MS 2000   /* multi-frame allow ample time */

static struct {
    bool            active;
    uint8_t         mode;          /* 0x03 / 0x07 / 0x0A */
    uint64_t        sent_ms;
    obd2_dtc_cb_t   cb;
    void           *user;
    obd2_dtc_t      codes[OBD2_MAX_DTCS];
    uint8_t         count;
} s_dtc_req = {0};

/* ── Clear DTCs request state (Mode 04) ─────────────────────────────── */
#define OBD2_CLEAR_TIMEOUT_MS 1500   /* some ECUs respond slowly */

static struct {
    bool            active;
    uint64_t        sent_ms;
    obd2_clear_cb_t cb;
    void           *user;
} s_clear_req = {0};

/* ── VIN request state (Mode 09 PID 0x02) ──────────────────────────── */
#define OBD2_VIN_TIMEOUT_MS 1500

static struct {
    bool            active;
    uint64_t        sent_ms;
    obd2_vin_cb_t   cb;
    void           *user;
    char            vin[18];        /* 17 chars + NUL */
} s_vin_req = {0};

/* ── ECU name request state (Mode 09 PID 0x0A) ─────────────────────── */
#define OBD2_ECUNAME_TIMEOUT_MS 1500

static struct {
    bool                active;
    uint64_t            sent_ms;
    obd2_ecuname_cb_t   cb;
    void               *user;
    char                name[24];   /* up to 20 chars + slack + NUL */
} s_ecuname_req = {0};

/* ── Freeze frame request state (Mode 02) ──────────────────────────── */
#define OBD2_FREEZE_TIMEOUT_MS 600   /* single PID per call; 600 ms ample */

static struct {
    bool              active;
    uint64_t          sent_ms;
    obd2_freeze_cb_t  cb;
    void             *user;
    uint8_t           data_pid;   /* echoed in response, used to match */
    uint8_t           frame_no;   /* echoed in response */
} s_freeze_req = {0};

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
static void _send_pid_request(uint8_t service, uint16_t pid);
static void _send_multi_pid_request(uint8_t service, const uint8_t *pids, uint8_t n);
static void _register_pid_signal(const obd2_pid_def_t *def);
static void _scan_send(uint8_t pid);
static void _scan_finalize(bool completed);
static void _send_flow_control(uint32_t target_id);
static void _process_full_message(uint8_t *msg, uint16_t len);
static uint32_t _request_id_for_response(uint32_t resp_id);

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
        const obd2_pid_def_t *def = obd2_pid_find_svc(ps->service, ps->pid);
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

void obd2_start(const uint32_t *enabled_pids, uint8_t count)
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
        uint8_t service = obd2_decode_service(enabled_pids[i]);
        uint8_t pid     = obd2_decode_pid(enabled_pids[i]);
        const obd2_pid_def_t *def = obd2_pid_find_svc(service, pid);
        if (!def) {
            ESP_LOGW(TAG, "Skipping unknown PID svc 0x%02X / 0x%02X",
                     service, pid);
            continue;
        }
        /* Conflict guard: if a signal with this name already exists and
         * is bound to a real CAN broadcast (can_id != 0), the native
         * preset owns it — don't poll OBD2 too, or the response would
         * overwrite the broadcast value via signal_set_external_value.
         * Skipped for packed PIDs (def->signal_name is NULL — they own
         * their sub-fields' signals). */
        if (def->signal_name) {
            int16_t existing = signal_find_by_name(def->signal_name);
            if (existing >= 0) {
                signal_t *sig = signal_get_by_index((uint16_t)existing);
                if (sig && sig->can_id != 0) {
                    ESP_LOGD(TAG, "Skip OBD2 PID 0x%02X — '%s' owned by preset",
                             pid, def->signal_name);
                    continue;
                }
            }
        }
        obd2_poll_state_t *ns = &s_poll[s_poll_count++];
        memset(ns, 0, sizeof(*ns));
        ns->pid              = pid;
        ns->service          = service;
        ns->target_period_ms = _alive_period_for(def);
        for (uint8_t j = 0; j < prev_count; j++) {
            if (prev[j].pid == pid && prev[j].service == service) {
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

uint8_t obd2_get_enabled(uint32_t *out, uint8_t max)
{
    uint8_t n = s_poll_count < max ? s_poll_count : max;
    if (out && n) {
        for (uint8_t i = 0; i < n; i++) {
            out[i] = obd2_encode_pid(s_poll[i].service, s_poll[i].pid);
        }
    }
    return n;
}

void obd2_set_enabled(const uint32_t *pids, uint8_t count)
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

/* ── Custom PID storage API ────────────────────────────────────────── */

void obd2_custom_reset(void)
{
    s_custom_count = 0;
}

uint8_t obd2_custom_count(void)
{
    return s_custom_count;
}

const obd2_pid_def_t *obd2_custom_at(uint8_t index)
{
    if (index >= s_custom_count) return NULL;
    return &s_custom[index].def;
}

const obd2_pid_def_t *obd2_custom_find_svc(uint8_t service, uint16_t pid)
{
    if (service == 0) service = 0x01;
    for (uint8_t i = 0; i < s_custom_count; i++) {
        const obd2_pid_def_t *d = &s_custom[i].def;
        if (d->pid != pid) continue;
        uint8_t s = d->service ? d->service : 0x01;
        if (s == service) return d;
    }
    return NULL;
}

/* ── One-shot test ────────────────────────────────────────────────── */

void obd2_test_pid(uint8_t service, uint8_t pid, uint32_t request_id,
                   uint8_t data_offset, uint8_t data_bytes,
                   float scale, float offset, bool is_signed,
                   obd2_test_cb_t cb, void *user)
{
    if (!cb) return;

    if (s_test.active) {
        ESP_LOGW(TAG, "Test already in progress");
        cb(false, NULL, 0, 0.0f, 0, user);
        return;
    }
    if (service == 0) service = 0x01;
    if (data_bytes != 1 && data_bytes != 2 && data_bytes != 4) {
        ESP_LOGW(TAG, "Test: data_bytes must be 1/2/4 (got %u)", data_bytes);
        cb(false, NULL, 0, 0.0f, 0, user);
        return;
    }

    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);

    s_test.active      = true;
    s_test.service     = service;
    s_test.pid         = pid;
    s_test.sent_ms     = now;
    s_test.cb          = cb;
    s_test.user        = user;
    s_test.data_offset = data_offset;
    s_test.data_bytes  = data_bytes;
    s_test.scale       = scale;
    s_test.offset      = offset;
    s_test.is_signed   = is_signed;

    /* Fire one request. Multi-PID Mode 01 NOT used here — we want a
     * deterministic single-PID response for the test. */
    uint32_t tx_id = request_id ? request_id : OBD2_REQUEST_ID_BROADCAST;
    uint8_t data[8] = { 0x02, service, pid, 0x55, 0x55, 0x55, 0x55, 0x55 };
    esp_err_t err = can_transmit_frame(tx_id, data, 8);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Test TX failed: %s", esp_err_to_name(err));
        s_test.active = false;
        cb(false, NULL, 0, 0.0f, 0, user);
        return;
    }
    s_last_tx_ms_global = now;
}

/* Helper used by the RX intercept (declared inside _process_full_message
 * for locality, defined here so we can also call from the Mode 01 walk
 * branch). Captures the payload bytes the test asked about, runs the
 * decode, and fires the callback. */
static void _test_capture(const uint8_t *payload, uint16_t payload_len,
                          uint64_t now_ms)
{
    if (!s_test.active) return;

    uint8_t max_take = (payload_len < 64) ? (uint8_t)payload_len : 64;
    uint8_t raw[64];
    memcpy(raw, payload, max_take);

    bool ok = (payload_len >= (uint16_t)(s_test.data_offset + s_test.data_bytes));
    float decoded = 0.0f;
    if (ok) {
        const uint8_t *p = payload + s_test.data_offset;
        int32_t v = 0;
        for (uint8_t i = 0; i < s_test.data_bytes; i++) {
            v = (v << 8) | p[i];
        }
        if (s_test.is_signed && s_test.data_bytes < 4) {
            uint32_t sign_bit = 1u << ((s_test.data_bytes * 8) - 1);
            if ((uint32_t)v & sign_bit) {
                v |= ~((int32_t)((sign_bit << 1) - 1));
            }
        }
        decoded = (float)v * s_test.scale + s_test.offset;
    }

    uint32_t elapsed = (uint32_t)(now_ms - s_test.sent_ms);
    obd2_test_cb_t cb = s_test.cb;
    void *user = s_test.user;

    s_test.active = false;
    s_test.cb = NULL;
    s_test.user = NULL;

    cb(ok, raw, max_take, decoded, elapsed, user);
}

/* ── DTC / VIN / Clear request helpers ──────────────────────────────────
 *
 * All three follow the same shape as obd2_test_pid: stash a request
 * descriptor, fire one TX frame, let RX intercept fill it in, expire
 * via the poll-timer timeout check. Each has its own state struct so
 * they don't collide with each other or with a pending test_pid. */

static void _dtc_start(uint8_t mode, obd2_dtc_cb_t cb, void *user) {
    if (!cb) return;
    if (s_dtc_req.active) {
        ESP_LOGW(TAG, "DTC request already in progress");
        cb(false, NULL, 0, mode, user);
        return;
    }
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);
    s_dtc_req.active  = true;
    s_dtc_req.mode    = mode;
    s_dtc_req.sent_ms = now;
    s_dtc_req.cb      = cb;
    s_dtc_req.user    = user;
    s_dtc_req.count   = 0;
    memset(s_dtc_req.codes, 0, sizeof(s_dtc_req.codes));

    /* Mode 03/07/0A: single-byte service request, no PID. ISO-TP single
     * frame: [length=1][service]. ECUs may respond multi-frame if they
     * have many codes — handled by the shared ISO-TP RX reassembly. */
    uint8_t data[8] = { 0x01, mode, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 };
    esp_err_t err = can_transmit_frame(OBD2_REQUEST_ID_BROADCAST, data, 8);
    if (err != ESP_OK) {
        /* DEBUG level — TX failures here are normal when the bus is
         * inactive (bench, ignition off, no CAN dongle plugged in).
         * The caller (dtc_monitor) tracks consecutive failures and
         * backs off cadence, so we don't need a per-attempt warning. */
        ESP_LOGD(TAG, "DTC mode 0x%02X TX failed: %s", mode, esp_err_to_name(err));
        s_dtc_req.active = false;
        cb(false, NULL, 0, mode, user);
        return;
    }
    s_last_tx_ms_global = now;
}

void obd2_read_stored_dtcs(obd2_dtc_cb_t cb, void *user)   { _dtc_start(0x03, cb, user); }
void obd2_read_pending_dtcs(obd2_dtc_cb_t cb, void *user)  { _dtc_start(0x07, cb, user); }
void obd2_read_permanent_dtcs(obd2_dtc_cb_t cb, void *user){ _dtc_start(0x0A, cb, user); }

void obd2_clear_dtcs(obd2_clear_cb_t cb, void *user) {
    if (!cb) return;
    if (s_clear_req.active) {
        ESP_LOGW(TAG, "Clear DTC already in progress");
        cb(false, user);
        return;
    }
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);
    s_clear_req.active  = true;
    s_clear_req.sent_ms = now;
    s_clear_req.cb      = cb;
    s_clear_req.user    = user;

    /* Mode 04: single-byte service request. ECU responds with 0x44 on
     * success or 0x7F 04 NN as negative. Many cars require engine off
     * (NRC 0x22 conditionsNotCorrect if running). */
    uint8_t data[8] = { 0x01, 0x04, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55 };
    esp_err_t err = can_transmit_frame(OBD2_REQUEST_ID_BROADCAST, data, 8);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Clear DTC TX failed: %s", esp_err_to_name(err));
        s_clear_req.active = false;
        cb(false, user);
        return;
    }
    s_last_tx_ms_global = now;
}

void obd2_read_vin(obd2_vin_cb_t cb, void *user) {
    if (!cb) return;
    if (s_vin_req.active) {
        ESP_LOGW(TAG, "VIN request already in progress");
        cb(false, NULL, user);
        return;
    }
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);
    s_vin_req.active  = true;
    s_vin_req.sent_ms = now;
    s_vin_req.cb      = cb;
    s_vin_req.user    = user;
    memset(s_vin_req.vin, 0, sizeof(s_vin_req.vin));

    /* Mode 09 PID 0x02 = VIN. Single-frame request, multi-frame response
     * (17 chars + 3 byte preamble = 20 bytes typical). */
    uint8_t data[8] = { 0x02, 0x09, 0x02, 0x55, 0x55, 0x55, 0x55, 0x55 };
    esp_err_t err = can_transmit_frame(OBD2_REQUEST_ID_BROADCAST, data, 8);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "VIN TX failed: %s", esp_err_to_name(err));
        s_vin_req.active = false;
        cb(false, NULL, user);
        return;
    }
    s_last_tx_ms_global = now;
}

void obd2_read_freeze_pid(uint8_t data_pid, uint8_t frame_no,
                          obd2_freeze_cb_t cb, void *user) {
    if (!cb) return;
    if (s_freeze_req.active) {
        ESP_LOGW(TAG, "Freeze-frame request already in progress");
        cb(false, NULL, 0, user);
        return;
    }
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);
    s_freeze_req.active   = true;
    s_freeze_req.sent_ms  = now;
    s_freeze_req.cb       = cb;
    s_freeze_req.user     = user;
    s_freeze_req.data_pid = data_pid;
    s_freeze_req.frame_no = frame_no;

    /* Mode 02 request: length=3, service=0x02, then data_pid + frame_no.
     * ECU answers with 0x42 [data_pid] [frame_no] [...data...] — same
     * data shape as the Mode 01 response for that PID, just frozen. */
    uint8_t data[8] = { 0x03, 0x02, data_pid, frame_no,
                        0x55, 0x55, 0x55, 0x55 };
    esp_err_t err = can_transmit_frame(OBD2_REQUEST_ID_BROADCAST, data, 8);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "Freeze TX failed: %s", esp_err_to_name(err));
        s_freeze_req.active = false;
        cb(false, NULL, 0, user);
        return;
    }
    s_last_tx_ms_global = now;
}

void obd2_read_ecu_name(obd2_ecuname_cb_t cb, void *user) {
    if (!cb) return;
    if (s_ecuname_req.active) {
        ESP_LOGW(TAG, "ECU-name request already in progress");
        cb(false, NULL, user);
        return;
    }
    uint64_t now = (uint64_t)(esp_timer_get_time() / 1000ULL);
    s_ecuname_req.active  = true;
    s_ecuname_req.sent_ms = now;
    s_ecuname_req.cb      = cb;
    s_ecuname_req.user    = user;
    memset(s_ecuname_req.name, 0, sizeof(s_ecuname_req.name));

    /* Mode 09 PID 0x0A. Same framing as VIN. */
    uint8_t data[8] = { 0x02, 0x09, 0x0A, 0x55, 0x55, 0x55, 0x55, 0x55 };
    esp_err_t err = can_transmit_frame(OBD2_REQUEST_ID_BROADCAST, data, 8);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "ECU-name TX failed: %s", esp_err_to_name(err));
        s_ecuname_req.active = false;
        cb(false, NULL, user);
        return;
    }
    s_last_tx_ms_global = now;
}

/* Decode one DTC byte-pair per SAE J2012. The top 2 bits of byte A
 * select the category letter; next 2 bits the first digit (0-3); the
 * remaining 12 bits are 3 hex digits. */
static void _decode_dtc_pair(uint8_t a, uint8_t b, char out[6]) {
    static const char categories[4] = { 'P', 'C', 'B', 'U' };
    static const char hex[17]       = "0123456789ABCDEF";
    out[0] = categories[(a >> 6) & 0x03];
    out[1] = (char)('0' + ((a >> 4) & 0x03));
    out[2] = hex[a & 0x0F];
    out[3] = hex[(b >> 4) & 0x0F];
    out[4] = hex[b & 0x0F];
    out[5] = '\0';
}

/* Parse a DTC response payload (after the service+count framing has been
 * stripped) into the request accumulator. Walks 2-byte pairs, stops at
 * 0x00 0x00 padding or buffer limit. */
static void _dtc_decode_payload(const uint8_t *p, uint16_t len) {
    while (len >= 2 && s_dtc_req.count < OBD2_MAX_DTCS) {
        if (p[0] == 0x00 && p[1] == 0x00) break;  /* end-of-list pad */
        _decode_dtc_pair(p[0], p[1], s_dtc_req.codes[s_dtc_req.count].code);
        s_dtc_req.codes[s_dtc_req.count].status = 0;
        s_dtc_req.count++;
        p   += 2;
        len -= 2;
    }
}

bool obd2_custom_add(uint8_t service, uint16_t pid,
                     const char *signal_name,
                     const char *human_name,
                     const char *unit,
                     uint8_t data_offset, uint8_t data_bytes,
                     float scale, float offset, bool is_signed,
                     obd2_tier_t tier,
                     const char *category,
                     uint32_t request_id)
{
    if (s_custom_count >= OBD2_MAX_CUSTOM_PIDS) {
        ESP_LOGW(TAG, "Custom PID registry full (max %u)", OBD2_MAX_CUSTOM_PIDS);
        return false;
    }
    if (!signal_name || !signal_name[0]) {
        ESP_LOGW(TAG, "Custom PID needs a signal_name");
        return false;
    }
    if (data_bytes != 1 && data_bytes != 2 && data_bytes != 4) {
        ESP_LOGW(TAG, "Custom PID data_bytes must be 1, 2 or 4 (got %u)", data_bytes);
        return false;
    }
    if (service == 0) service = 0x01;

    obd2_custom_entry_t *e = &s_custom[s_custom_count];
    memset(e, 0, sizeof(*e));

    /* Copy strings into stable buffers owned by the entry. */
    strncpy(e->signal_name, signal_name, sizeof(e->signal_name) - 1);
    if (human_name && human_name[0]) {
        strncpy(e->human_name, human_name, sizeof(e->human_name) - 1);
    } else {
        strncpy(e->human_name, signal_name, sizeof(e->human_name) - 1);
    }
    if (unit && unit[0]) {
        strncpy(e->unit, unit, sizeof(e->unit) - 1);
    }
    if (category && category[0]) {
        strncpy(e->category, category, sizeof(e->category) - 1);
    }

    /* Wire the sub_field (the actual decoder). Using a sub-field even
     * for "single value" PIDs keeps the decode path uniform with packed
     * Toyota Mode 21 PIDs. */
    e->sub_field.signal_name = e->signal_name;
    e->sub_field.unit        = e->unit;
    e->sub_field.byte_offset = data_offset;
    e->sub_field.bytes       = data_bytes;
    e->sub_field.is_signed   = is_signed;
    e->sub_field.scale       = scale;
    e->sub_field.offset      = offset;

    /* Wire the def. signal_name = NULL marks this as a packed PID so the
     * picker/decoder paths walk sub_fields rather than the legacy
     * single-value fields. */
    e->def.pid              = pid;
    e->def.signal_name      = NULL;
    e->def.human_name       = e->human_name;
    e->def.unit             = e->unit;
    e->def.bytes            = 0;
    e->def.scale            = 0.0f;
    e->def.offset           = 0.0f;
    e->def.tier             = tier;
    e->def.default_enabled  = false;
    e->def.suggested_filler = false;
    e->def.service          = service;
    e->def.category         = e->category[0] ? e->category : NULL;
    e->def.sub_fields       = &e->sub_field;
    e->def.sub_field_count  = 1;
    e->def.request_id       = request_id;

    s_custom_count++;
    ESP_LOGI(TAG, "Added custom PID svc 0x%02X 0x%04X -> '%s' (%u total)",
             service, pid, signal_name, s_custom_count);
    return true;
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

    /* Packed Mode 01 PIDs (diesel EGT/DPF/boost/rail pressure — 9+ byte
     * responses) get sent SOLO, not batched. Two reasons:
     *  - Some ECUs reject multi-PID requests whose total response would
     *    exceed an internal buffer (~31 bytes typical limit).
     *  - The multi-PID walker advances by `1 + span` per PID; if any
     *    ECU truncates the packed response under batching pressure, the
     *    walker falls off the end and trailing PIDs go un-decoded.
     * Single-PID requests for packed PIDs are clean: one stream, one
     * decode, no ambiguity. */
    const obd2_pid_def_t *top_def = obd2_pid_find_svc(0x01, top->pid);
    bool top_packed = (top_def && top_def->sub_fields &&
                       top_def->sub_field_count > 0);
    if (top_packed) {
        _send_pid_request(0x01, top->pid);
        top->last_tx_ms = now;
        s_last_tx_ms_global = now;
        return;
    }

    /* Mode 01: batch up to OBD2_MAX_BATCH_PIDS due+alive single-value
     * PIDs into one request. Cuts bus traffic ~2.4x vs polling each
     * individually and shrinks effective latency because the car sends
     * one ISO-TP stream instead of N single-frame responses. Dead PIDs
     * stay on their 5-sec individual probe schedule (excluded from
     * batches — including them risks NRC from ECUs that reject the
     * whole batch when any one PID is unknown). Packed PIDs likewise
     * excluded — see SOLO branch above. */
    uint8_t batch[OBD2_MAX_BATCH_PIDS];
    uint8_t batch_n = 0;
    batch[batch_n++] = top->pid;
    top->last_tx_ms = now;

    for (uint8_t j = 0; j < s_poll_count && batch_n < OBD2_MAX_BATCH_PIDS; j++) {
        if (j == best_idx) continue;
        obd2_poll_state_t *ps = &s_poll[j];
        if (ps->service != 0x01) continue;
        if (ps->target_period_ms >= OBD2_PERIOD_DEAD_MS) continue;
        const obd2_pid_def_t *cand = obd2_pid_find_svc(0x01, ps->pid);
        if (cand && cand->sub_fields && cand->sub_field_count > 0) continue;
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

    /* One-shot test timeout — fire callback with ok=false if the ECU
     * never answered within OBD2_TEST_TIMEOUT_MS. */
    if (s_test.active && (now - s_test.sent_ms) > OBD2_TEST_TIMEOUT_MS) {
        obd2_test_cb_t cb = s_test.cb;
        void *user = s_test.user;
        uint32_t elapsed = (uint32_t)(now - s_test.sent_ms);
        s_test.active = false;
        s_test.cb = NULL;
        s_test.user = NULL;
        if (cb) cb(false, NULL, 0, 0.0f, elapsed, user);
    }

    /* DTC read timeout — multi-frame ISO-TP may legitimately take ~1 s
     * on cars with many codes; cap at OBD2_DTC_TIMEOUT_MS. */
    if (s_dtc_req.active && (now - s_dtc_req.sent_ms) > OBD2_DTC_TIMEOUT_MS) {
        obd2_dtc_cb_t cb = s_dtc_req.cb;
        void *user = s_dtc_req.user;
        uint8_t mode = s_dtc_req.mode;
        ESP_LOGW(TAG, "DTC mode 0x%02X timed out", mode);
        s_dtc_req.active = false;
        if (cb) cb(false, NULL, 0, mode, user);
    }

    /* Clear DTC timeout. */
    if (s_clear_req.active && (now - s_clear_req.sent_ms) > OBD2_CLEAR_TIMEOUT_MS) {
        obd2_clear_cb_t cb = s_clear_req.cb;
        void *user = s_clear_req.user;
        ESP_LOGW(TAG, "Clear DTC timed out");
        s_clear_req.active = false;
        if (cb) cb(false, user);
    }

    /* VIN timeout. */
    if (s_vin_req.active && (now - s_vin_req.sent_ms) > OBD2_VIN_TIMEOUT_MS) {
        obd2_vin_cb_t cb = s_vin_req.cb;
        void *user = s_vin_req.user;
        ESP_LOGW(TAG, "VIN read timed out");
        s_vin_req.active = false;
        if (cb) cb(false, NULL, user);
    }

    /* ECU name timeout. */
    if (s_ecuname_req.active && (now - s_ecuname_req.sent_ms) > OBD2_ECUNAME_TIMEOUT_MS) {
        obd2_ecuname_cb_t cb = s_ecuname_req.cb;
        void *user = s_ecuname_req.user;
        ESP_LOGW(TAG, "ECU name read timed out");
        s_ecuname_req.active = false;
        if (cb) cb(false, NULL, user);
    }

    /* Freeze-frame timeout — short because each query is single-PID. */
    if (s_freeze_req.active && (now - s_freeze_req.sent_ms) > OBD2_FREEZE_TIMEOUT_MS) {
        obd2_freeze_cb_t cb = s_freeze_req.cb;
        void *user = s_freeze_req.user;
        ESP_LOGD(TAG, "Freeze PID 0x%02X timed out", s_freeze_req.data_pid);
        s_freeze_req.active = false;
        if (cb) cb(false, NULL, 0, user);
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

static void _send_pid_request(uint8_t service, uint16_t pid)
{
    /* ISO 15765-4 single-frame requests:
     *   Mode 01 / Mode 21 (8-bit PID):
     *     byte 0: length 2  (service + PID byte)
     *     byte 1: service (0x01 / 0x21)
     *     byte 2: PID
     *     byte 3..7: 0x55 padding
     *   Mode 22 (16-bit PID, UDS Read Data By Identifier):
     *     byte 0: length 3  (service + 2 PID bytes)
     *     byte 1: 0x22
     *     byte 2: PID hi byte
     *     byte 3: PID lo byte
     *     byte 4..7: 0x55 padding
     *
     * Per-PID request_id override addresses a specific ECU (e.g. 0x7E0
     * = engine ECU). Used for Mode 21 to avoid NRCs from other ECUs
     * that don't recognise the PID. Default = broadcast 0x7DF. */
    if (service == 0) service = 0x01;
    const obd2_pid_def_t *def = obd2_pid_find_svc(service, pid);
    uint32_t tx_id = (def && def->request_id) ? def->request_id
                                              : OBD2_REQUEST_ID_BROADCAST;
    uint8_t data[8] = { 0, 0, 0, 0x55, 0x55, 0x55, 0x55, 0x55 };
    if (service == 0x22) {
        data[0] = 0x03;
        data[1] = 0x22;
        data[2] = (uint8_t)((pid >> 8) & 0xFF);
        data[3] = (uint8_t)(pid & 0xFF);
    } else {
        data[0] = 0x02;
        data[1] = service;
        data[2] = (uint8_t)(pid & 0xFF);
    }
    esp_err_t err = can_transmit_frame(tx_id, data, 8);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "TX failed for service 0x%02X PID 0x%04X: %s",
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
    const obd2_pid_def_t *def0 = obd2_pid_find_svc(service, pids[0]);
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

    /* ── Negative Response Code (NRC) — service rejected ─────────────
     * Format: 0x7F <requested_service> <NRC_byte>. Common NRCs:
     *   0x11 service not supported / 0x12 sub-function not supported
     *   0x22 conditionsNotCorrect (e.g. clear DTC with engine running)
     *   0x33 securityAccessDenied
     *
     * For pending request types that block on a specific service we
     * fire their callback with ok=false here so the user gets a
     * timely failure instead of waiting for timeout. */
    if (resp_service == 0x7F && len >= 3) {
        uint8_t rejected_svc = msg[1];
        if (s_clear_req.active && rejected_svc == 0x04) {
            ESP_LOGW(TAG, "Mode 04 (clear) rejected — NRC 0x%02X", msg[2]);
            obd2_clear_cb_t cb = s_clear_req.cb;
            void *user = s_clear_req.user;
            s_clear_req.active = false;
            cb(false, user);
            return;
        }
        if (s_dtc_req.active && rejected_svc == s_dtc_req.mode) {
            ESP_LOGW(TAG, "DTC mode 0x%02X rejected — NRC 0x%02X",
                     rejected_svc, msg[2]);
            obd2_dtc_cb_t cb = s_dtc_req.cb;
            void *user = s_dtc_req.user;
            uint8_t mode = s_dtc_req.mode;
            s_dtc_req.active = false;
            cb(false, NULL, 0, mode, user);
            return;
        }
        if (s_vin_req.active && rejected_svc == 0x09) {
            ESP_LOGW(TAG, "Mode 09 (VIN) rejected — NRC 0x%02X", msg[2]);
            obd2_vin_cb_t cb = s_vin_req.cb;
            void *user = s_vin_req.user;
            s_vin_req.active = false;
            cb(false, NULL, user);
            return;
        }
        if (s_ecuname_req.active && rejected_svc == 0x09) {
            ESP_LOGW(TAG, "Mode 09 (ECU name) rejected — NRC 0x%02X", msg[2]);
            obd2_ecuname_cb_t cb = s_ecuname_req.cb;
            void *user = s_ecuname_req.user;
            s_ecuname_req.active = false;
            cb(false, NULL, user);
            return;
        }
        if (s_freeze_req.active && rejected_svc == 0x02) {
            /* NRC 0x12 (sub-function not supported) is the typical
             * response when no freeze frame exists for the requested
             * PID — fire ok=false so the UI shows "not stored". */
            ESP_LOGD(TAG, "Mode 02 (freeze) rejected — NRC 0x%02X (PID 0x%02X)",
                     msg[2], s_freeze_req.data_pid);
            obd2_freeze_cb_t cb = s_freeze_req.cb;
            void *user = s_freeze_req.user;
            s_freeze_req.active = false;
            cb(false, NULL, 0, user);
            return;
        }
        return;
    }

    if ((resp_service & 0xC0) != 0x40) return;   /* not a positive response */

    uint8_t service = (uint8_t)(resp_service - 0x40);

    /* ── Mode 02 — Freeze Frame response ─────────────────────────────
     * Format: 0x42 [data_pid] [frame_no] [data_bytes...]
     * Match against the pending request's (pid, frame_no) so a stale
     * response from a different call (rare but possible if the user
     * fires fast) doesn't get attributed to the wrong query. */
    if (s_freeze_req.active && service == 0x02 && len >= 3) {
        uint8_t resp_pid   = msg[1];
        uint8_t resp_frame = msg[2];
        if (resp_pid == s_freeze_req.data_pid &&
            resp_frame == s_freeze_req.frame_no) {
            const uint8_t *payload = (len > 3) ? &msg[3] : NULL;
            uint8_t payload_len    = (len > 3) ? (uint8_t)(len - 3) : 0;
            obd2_freeze_cb_t cb = s_freeze_req.cb;
            void *user = s_freeze_req.user;
            s_freeze_req.active = false;
            cb(true, payload, payload_len, user);
            return;
        }
    }

    /* ── Mode 03/07/0A — DTC read response ────────────────────────────
     * Payload format: [count_byte (optional)][DTC_pairs...]
     * Newer SAE J1979: explicit count byte at msg[1].
     * Older / some ECUs: no count, pairs start at msg[1].
     * Heuristic: if count*2+1 == len-1, treat byte as count; else
     * decode from msg[1] directly. */
    if (s_dtc_req.active &&
        (service == 0x03 || service == 0x07 || service == 0x0A) &&
        service == s_dtc_req.mode) {
        const uint8_t *p   = &msg[1];
        uint16_t      plen = (uint16_t)(len - 1);
        if (plen >= 1) {
            uint8_t maybe_count = p[0];
            if ((uint16_t)(maybe_count * 2 + 1) == plen) {
                p++;
                plen--;
            }
        }
        _dtc_decode_payload(p, plen);
        obd2_dtc_cb_t cb = s_dtc_req.cb;
        void *user = s_dtc_req.user;
        uint8_t mode = s_dtc_req.mode;
        uint8_t count = s_dtc_req.count;
        s_dtc_req.active = false;
        cb(true, count > 0 ? s_dtc_req.codes : NULL, count, mode, user);
        return;
    }

    /* ── Mode 04 — Clear DTCs ack ────────────────────────────────────
     * Positive response is just the service echo (0x44) with no payload.
     * NRC was handled above. */
    if (s_clear_req.active && service == 0x04) {
        ESP_LOGI(TAG, "Clear DTC acknowledged");
        obd2_clear_cb_t cb = s_clear_req.cb;
        void *user = s_clear_req.user;
        s_clear_req.active = false;
        cb(true, user);
        return;
    }

    /* ── Mode 09 PID 0x02 — VIN ──────────────────────────────────────
     * Response: 0x49 0x02 0x01 <17 ASCII bytes>. The 0x01 between PID
     * and payload is the "data identifier" / message count — discarded.
     * Some ECUs return without it; handle both shapes. */
    if (s_vin_req.active && service == 0x09 && len >= 3 && msg[1] == 0x02) {
        const uint8_t *p   = &msg[2];
        uint16_t      plen = (uint16_t)(len - 2);
        if (plen > 0 && p[0] >= 0x01 && p[0] <= 0x05 && plen >= (uint16_t)(p[0] * 17 + 1)) {
            /* DI byte present; skip it. */
            p++;
            plen--;
        }
        uint8_t take = (plen < 17) ? (uint8_t)plen : 17;
        memcpy(s_vin_req.vin, p, take);
        /* Pad / trim — VIN is 17 fixed ASCII chars; sometimes leading
         * null bytes precede the VIN on certain ECUs. Strip them. */
        s_vin_req.vin[17] = '\0';
        char *start = s_vin_req.vin;
        while (*start && (*start < 0x20 || *start > 0x7E)) start++;
        if (start != s_vin_req.vin) {
            memmove(s_vin_req.vin, start, strlen(start) + 1);
        }
        ESP_LOGI(TAG, "VIN received: '%s'", s_vin_req.vin);

        obd2_vin_cb_t cb = s_vin_req.cb;
        void *user = s_vin_req.user;
        s_vin_req.active = false;
        cb(true, s_vin_req.vin, user);
        return;
    }

    /* ── Mode 09 PID 0x0A — ECU Name ─────────────────────────────────
     * Same shape as VIN. Up to 20 ASCII chars (typically the module's
     * family code, e.g. "ECM-EngineControl" or "TCM"). */
    if (s_ecuname_req.active && service == 0x09 && len >= 3 && msg[1] == 0x0A) {
        const uint8_t *p   = &msg[2];
        uint16_t      plen = (uint16_t)(len - 2);
        if (plen > 0 && p[0] >= 0x01 && p[0] <= 0x05) {
            /* DI byte present; skip it (same convention as VIN). */
            p++;
            plen--;
        }
        uint16_t take = (plen < sizeof(s_ecuname_req.name) - 1)
                        ? plen : (uint16_t)(sizeof(s_ecuname_req.name) - 1);
        memcpy(s_ecuname_req.name, p, take);
        s_ecuname_req.name[take] = '\0';
        /* Strip leading non-printable chars; some ECUs pre-pad with NULs. */
        char *start = s_ecuname_req.name;
        while (*start && (*start < 0x20 || *start > 0x7E)) start++;
        if (start != s_ecuname_req.name) {
            memmove(s_ecuname_req.name, start, strlen(start) + 1);
        }
        /* Trim trailing whitespace / non-printable. */
        size_t blen = strlen(s_ecuname_req.name);
        while (blen > 0 && (s_ecuname_req.name[blen-1] <= 0x20)) {
            s_ecuname_req.name[--blen] = '\0';
        }
        ESP_LOGI(TAG, "ECU name received: '%s'", s_ecuname_req.name);

        obd2_ecuname_cb_t cb = s_ecuname_req.cb;
        void *user = s_ecuname_req.user;
        s_ecuname_req.active = false;
        cb(true, s_ecuname_req.name, user);
        return;
    }

    /* Services we currently decode. Mode 22 (16-bit PIDs / UDS Read
     * Data By Identifier) gets its own walk further down. */
    if (service != 0x01 && service != 0x21 && service != 0x22) return;

    /* ── Mode 22 path: 16-bit PID at msg[1..2], payload from msg[3]. ── */
    if (service == 0x22) {
        if (len < 3) return;
        uint16_t pid22 = (uint16_t)(((uint16_t)msg[1] << 8) | msg[2]);
        const uint8_t *payload = &msg[3];
        uint16_t payload_len = (uint16_t)(len - 3);

        /* Test intercept (Mode 22). */
        if (s_test.active && s_test.service == 0x22 && s_test.pid == pid22) {
            uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            _test_capture(payload, payload_len, now_ms);
        }

        const obd2_pid_def_t *def22 = obd2_pid_find_svc(0x22, pid22);
        if (!def22) return;
        if (def22->sub_fields && def22->sub_field_count > 0) {
            _decode_packed(def22, payload, payload_len);
        } else {
            if (payload_len < def22->bytes) return;
            _decode_and_push(def22, payload);
        }

        uint64_t now22 = (uint64_t)(esp_timer_get_time() / 1000ULL);
        for (uint8_t i = 0; i < s_poll_count; i++) {
            if (s_poll[i].pid == pid22 && s_poll[i].service == 0x22) {
                s_poll[i].last_response_ms = now22;
                break;
            }
        }
        _try_dispatch(now22);
        return;
    }

    uint8_t first_pid = msg[1];

    /* ── One-shot test intercept ─────────────────────────────────────
     * If a test is pending and this response matches its (service, pid)
     * target, capture and call back. Test runs BEFORE the scan/decode
     * branches so we don't double-process. For Mode 01 the test PID
     * could appear later in a multi-PID response — handled in the
     * Mode 01 walk branch below. */
    if (s_test.active && service == s_test.service && first_pid == s_test.pid) {
        uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
        _test_capture(&msg[2], (uint16_t)(len - 2), now_ms);
        /* Fall through — test_capture doesn't suppress normal decode;
         * the same response might still be useful (e.g. a stale signal
         * picking up its value). */
    }

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
            const obd2_pid_def_t *def = obd2_pid_find_svc(0x01, pid);
            if (!def) {
                ESP_LOGD(TAG, "Multi-PID resp: no decoder for 0x%02X, stopping walk", pid);
                return;
            }

            /* Determine how many data bytes this PID's response carries
             * and how to decode them. Both branches must agree on the
             * `consumed` count so the walk stays in sync for any
             * trailing PIDs in a multi-PID batch response. */
            uint8_t  consumed;
            uint16_t data_len;
            bool     packed = (def->sub_fields && def->sub_field_count > 0);

            if (packed) {
                /* Packed Mode 01 (diesel EGT / DPF / boost / rail
                 * pressure etc.). Compute the byte span as the max
                 * (byte_offset + bytes) across all sub-fields — that's
                 * how many data bytes the ECU emits for this PID. */
                uint16_t span = 0;
                for (uint8_t i = 0; i < def->sub_field_count; i++) {
                    uint16_t end = (uint16_t)def->sub_fields[i].byte_offset +
                                    (uint16_t)def->sub_fields[i].bytes;
                    if (end > span) span = end;
                }
                if (span == 0) { ESP_LOGW(TAG, "Packed PID 0x%02X span=0", pid); return; }
                data_len = span;
                consumed = (uint8_t)(1 + span);
            } else {
                data_len = def->bytes;
                consumed = (uint8_t)(1 + def->bytes);
            }
            if (rem < consumed) return;

            if (packed) {
                _decode_packed(def, &p[1], data_len);
            } else {
                _decode_and_push(def, &p[1]);
            }

            /* Test intercept inside multi-PID walk: catches the case
             * where the test PID is at position 2+ of a response that
             * happened to land in the same batch. */
            if (s_test.active && s_test.service == 0x01 &&
                pid == s_test.pid) {
                _test_capture(&p[1], (uint16_t)(rem - 1), now);
            }

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
    const obd2_pid_def_t *def = obd2_pid_find_svc(service, first_pid);
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
