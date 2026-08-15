/*
 * rdm_bus.c — the dash's half of the RDM device bus.
 *
 * Two jobs. Announce who and where we are on the fixed discovery id, and
 * reconcile track outlines with whatever else is on the bus: whichever end
 * holds the newer revision sends. That symmetry is deliberate — Studio should
 * only ever have to reach ONE device, and which one is reachable on the day is
 * not something the firmware gets to assume.
 *
 * Everything here runs on the LVGL task: RX comes in through
 * rdm_bus_on_can_frame() from the CAN drain, the tick is an LVGL timer. No
 * locking, no cross-task state.
 *
 * The transfer is deliberately unhurried. It happens once, when a track
 * changes, and it shares a bus with an engine — so data frames are paced a few
 * per tick rather than blasted, and the whole thing defers while the car is
 * moving. Getting the outline across two seconds later costs nothing; a burst
 * of 500 frames during a corner could cost something real.
 */
#include "rdm_bus.h"

#include "rdm_bus_proto.h"
#include "can_manager.h"
#include "widgets/track_map_geo.h"
#include "storage/config_store.h"
#include "widgets/signal.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "ui/lvgl_helpers.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "rdm_bus";

#define BUS_TICK_MS            200u    /* announce every 5th tick = 1 Hz */
#define BUS_ANNOUNCE_EVERY     5u      /* 1 Hz once something is out there */
#define BUS_ANNOUNCE_EVERY_QUIET 150u  /* 30 s while nothing has ever answered */
#define BUS_PEER_STALE_MS      5000u
#define BUS_FRAMES_PER_TICK    24u     /* ~120 frames/s: a 3.2 KB outline in
                                        * ~4.5 s, and never a burst */
#define BUS_RX_TIMEOUT_MS      3000u
#define BUS_NAK_GAP_MS         400u

/* Speed above which a bulk transfer waits. The dash sees GPS speed as a
 * channel; below this the car is stationary or crawling in the pits, which is
 * when a few hundred frames are free. */
#define BUS_MOVING_KMH         5.0f

typedef enum {
    ST_IDLE = 0,
    ST_RECEIVING,   /* we asked; frames are arriving          */
    ST_SENDING,     /* peer asked; we are streaming           */
} bus_state_t;

static uint16_t s_base       = RDM_BUS_DASH_BASE_DEFAULT;
static char     s_track_name[32] = {0};
static uint32_t s_track_rev  = 0;

static uint16_t s_peer_base  = 0;
static uint32_t s_peer_rev   = 0;
static uint8_t  s_peer_caps  = 0;
static int64_t  s_peer_seen_us = 0;

static bus_state_t s_state = ST_IDLE;
static int64_t     s_state_us = 0;
static int64_t     s_last_nak_us = 0;

/* Receive side */
static uint8_t     s_rx_buf[RDM_BUS_ASSET_MAX_BYTES];
static rdm_bus_rx_t s_rx;
static uint32_t    s_rx_rev = 0;

/* Send side */
static uint8_t     s_tx_buf[RDM_BUS_ASSET_MAX_BYTES];
static uint16_t    s_tx_len = 0;
static uint16_t    s_tx_seq = 0;
static uint16_t    s_tx_total_seq = 0;

static lv_timer_t *s_timer = NULL;
static uint32_t    s_tick = 0;

/* ── helpers ──────────────────────────────────────────────────────────────── */

static inline uint32_t _ms_since(int64_t us) {
    if (us == 0) return UINT32_MAX;
    return (uint32_t)((esp_timer_get_time() - us) / 1000);
}

static bool _car_is_moving(void) {
    /* Best-effort. If the speed signal is absent or stale we call the car
     * stopped rather than blocking the transfer forever: a bench dash has no
     * GPS at all, and refusing to move a track there would be the wrong
     * failure to pick. GPS_SPEED first (the puck's own, and the puck is the
     * peer we are talking to), VEHICLE_SPEED as the fallback. */
    static const char *const kNames[] = { "GPS_SPEED", "VEHICLE_SPEED" };
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++) {
        int16_t idx = signal_find_by_name(kNames[i]);
        if (idx < 0) continue;
        signal_t *s = signal_get_by_index((uint16_t)idx);
        if (!s || s->is_stale) continue;
        return s->current_value > BUS_MOVING_KMH;
    }
    return false;
}

static void _send_ctrl_to_peer(const rdm_bus_ctrl_t *c) {
    if (!s_peer_base) return;
    uint8_t f[8];
    rdm_bus_ctrl_pack(c, f);
    /* Control rides OUR block: the peer learned our base from discovery, so
     * neither side ever has to be configured with the other's address. */
    can_transmit_frame(s_base + RDM_BUS_OFF_ASSET_RX, f, 8);
}

static void _abort(uint8_t reason) {
    rdm_bus_ctrl_t c = { .msg = RDM_BUS_MSG_ABORT, .track_rev = s_rx_rev,
                         .reason = reason };
    _send_ctrl_to_peer(&c);
    s_state = ST_IDLE;
    memset(&s_rx, 0, sizeof(s_rx));
}

/* ── publishing a received outline ───────────────────────────────────────── */

/* Same contract as the HTTP upload: parse with the parser the widget uses,
 * then stage a .tmp and rename. A half-written outline that still passes the
 * magic check would draw a circuit with its back half missing, which looks
 * like a track rather than like a failure. */
static bool _publish(const char *name, const uint8_t *buf, size_t len, uint32_t rev) {
    track_map_file_t parsed;
    if (track_map_parse(buf, len, &parsed) != TRACK_MAP_OK) {
        ESP_LOGW(TAG, "received outline did not parse — discarding");
        return false;
    }

    struct stat st;
    if (stat(TRACK_MAP_LFS_DIR, &st) != 0) mkdir(TRACK_MAP_LFS_DIR, 0755);

    char path[128], tmp[160];
    snprintf(path, sizeof(path), "%s/%s.rdmtrk", TRACK_MAP_LFS_DIR, name);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = (fwrite(buf, 1, len, f) == len);
    if (ok) ok = (fflush(f) == 0 && fsync(fileno(f)) == 0);
    if (fclose(f) != 0) ok = false;
    if (!ok) { remove(tmp); return false; }

    remove(path);
    if (rename(tmp, path) != 0) { remove(tmp); return false; }

    rdm_bus_set_local_track(name, rev);
    ESP_LOGI(TAG, "track '%s' received over CAN (%u pts, rev %lu)",
             name, (unsigned)parsed.n_points, (unsigned long)rev);
    return true;
}

/* ── receive path ─────────────────────────────────────────────────────────── */

static void _on_asset_from_peer(const uint8_t *d, uint8_t dlc) {
    /* A BEGIN is a control frame that arrives on the holder's TX id ahead of
     * the data, so both shapes share this id and are told apart by trying the
     * control decode first. Data frames start with a sequence number, which
     * cannot collide with a message enum because rdm_bus_ctrl_unpack rejects
     * anything it does not recognise. */
    rdm_bus_ctrl_t c;
    if (dlc == 8 && rdm_bus_ctrl_unpack(d, &c) && c.msg == RDM_BUS_MSG_BEGIN) {
        if (!rdm_bus_rx_begin(&s_rx, s_rx_buf, sizeof(s_rx_buf), &c)) {
            _abort(RDM_BUS_ABORT_TOO_BIG);
            return;
        }
        s_rx_rev  = c.track_rev;
        s_state   = ST_RECEIVING;
        s_state_us = esp_timer_get_time();
        ESP_LOGI(TAG, "incoming track rev %lu, %u bytes",
                 (unsigned long)c.track_rev, (unsigned)c.total_bytes);
        return;
    }

    if (s_state != ST_RECEIVING) return;

    uint16_t seq; const uint8_t *pl; uint8_t len;
    if (!rdm_bus_data_unpack(d, dlc, &seq, &pl, &len)) return;
    rdm_bus_rx_data(&s_rx, seq, pl, len);
    s_state_us = esp_timer_get_time();

    if (rdm_bus_rx_first_missing(&s_rx) >= 0) return;

    /* Complete — but only publish if the CRC agrees. */
    if (!rdm_bus_rx_complete(&s_rx)) {
        ESP_LOGW(TAG, "transfer complete but CRC mismatched");
        _abort(RDM_BUS_ABORT_BAD_CRC);
        return;
    }

    /* The name travels in the file itself, so a circuit keeps the name its
     * author gave it rather than one invented by the transport. */
    track_map_file_t parsed;
    char name[TRACK_MAP_NAME_LEN] = {0};
    if (track_map_parse(s_rx_buf, s_rx.total_bytes, &parsed) == TRACK_MAP_OK)
        snprintf(name, sizeof(name), "%s", parsed.name);
    if (name[0] == '\0') snprintf(name, sizeof(name), "track%lu",
                                  (unsigned long)s_rx_rev);

    if (_publish(name, s_rx_buf, s_rx.total_bytes, s_rx_rev)) {
        rdm_bus_ctrl_t done = { .msg = RDM_BUS_MSG_DONE, .track_rev = s_rx_rev };
        _send_ctrl_to_peer(&done);
    } else {
        _abort(RDM_BUS_ABORT_BAD_DATA);
        return;
    }
    s_state = ST_IDLE;
    memset(&s_rx, 0, sizeof(s_rx));
}

/* ── send path ────────────────────────────────────────────────────────────── */

static bool _load_local_track(void) {
    if (s_track_name[0] == '\0') return false;
    char path[128];
    snprintf(path, sizeof(path), "%s/%s.rdmtrk", TRACK_MAP_LFS_DIR, s_track_name);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(s_tx_buf, 1, sizeof(s_tx_buf), f);
    fclose(f);
    if (n == 0) return false;
    s_tx_len = (uint16_t)n;
    s_tx_seq = 0;
    s_tx_total_seq = (uint16_t)((n + RDM_BUS_DATA_PAYLOAD - 1) / RDM_BUS_DATA_PAYLOAD);
    return true;
}

static void _on_ctrl_to_us(const uint8_t *d, uint8_t dlc) {
    if (dlc != 8) return;
    rdm_bus_ctrl_t c;
    if (!rdm_bus_ctrl_unpack(d, &c)) return;

    switch (c.msg) {
    case RDM_BUS_MSG_REQUEST:
        if (s_state != ST_IDLE) return;
        if (_car_is_moving()) { _abort(RDM_BUS_ABORT_MOVING); return; }
        if (!_load_local_track()) { _abort(RDM_BUS_ABORT_BAD_DATA); return; }
        {
            rdm_bus_ctrl_t b = {
                .msg = RDM_BUS_MSG_BEGIN,
                .track_rev = s_track_rev,
                .total_bytes = s_tx_len,
                .crc32 = rdm_bus_crc32(s_tx_buf, s_tx_len),
            };
            uint8_t f[8];
            rdm_bus_ctrl_pack(&b, f);
            can_transmit_frame(s_base + RDM_BUS_OFF_ASSET_TX, f, 8);
        }
        s_state = ST_SENDING;
        s_state_us = esp_timer_get_time();
        ESP_LOGI(TAG, "serving track '%s' rev %lu (%u B)",
                 s_track_name, (unsigned long)s_track_rev, (unsigned)s_tx_len);
        break;

    case RDM_BUS_MSG_NAK:
        /* Resume at the gap the receiver named. Anything it already has is
         * ignored on arrival, so overshooting is harmless. */
        if (s_state == ST_SENDING && c.seq < s_tx_total_seq) s_tx_seq = c.seq;
        break;

    case RDM_BUS_MSG_DONE:
    case RDM_BUS_MSG_ABORT:
        s_state = ST_IDLE;
        break;

    default:
        break;
    }
}

static void _pump_send(void) {
    if (s_state != ST_SENDING) return;
    for (uint32_t i = 0; i < BUS_FRAMES_PER_TICK && s_tx_seq < s_tx_total_seq; i++) {
        size_t off = (size_t)s_tx_seq * RDM_BUS_DATA_PAYLOAD;
        size_t n = s_tx_len - off;
        if (n > RDM_BUS_DATA_PAYLOAD) n = RDM_BUS_DATA_PAYLOAD;
        uint8_t f[8];
        rdm_bus_data_pack(s_tx_seq, s_tx_buf + off, (uint8_t)n, f);
        if (can_transmit_frame(s_base + RDM_BUS_OFF_ASSET_TX, f, (uint8_t)(2 + n)) != ESP_OK)
            return;   /* TX queue full: pick up next tick rather than spin */
        s_tx_seq++;
    }
    /* Sender goes quiet once it has streamed everything. If frames were lost
     * the receiver NAKs and s_tx_seq winds back; if not, DONE ends it. */
}

/* ── discovery + reconcile ───────────────────────────────────────────────── */

static void _on_announce(const uint8_t *d, uint8_t dlc) {
    if (dlc != 8) return;
    rdm_bus_announce_t a;
    if (!rdm_bus_announce_unpack(d, &a)) return;
    if (a.devtype == RDM_BUS_DEV_DASH) return;        /* ourselves, or another dash */
    if (a.proto_ver != RDM_BUS_PROTO_VERSION) {
        ESP_LOGW(TAG, "peer speaks protocol v%u, we speak v%u — ignoring",
                 a.proto_ver, RDM_BUS_PROTO_VERSION);
        return;
    }

    if (a.base_id != s_peer_base)
        ESP_LOGI(TAG, "peer type %u at base 0x%03X", a.devtype, a.base_id);

    s_peer_base    = a.base_id;
    s_peer_rev     = a.track_rev;
    s_peer_caps    = a.caps;
    s_peer_seen_us = esp_timer_get_time();
}

static void _announce(void) {
    rdm_bus_announce_t a = {
        .devtype   = RDM_BUS_DEV_DASH,
        .proto_ver = RDM_BUS_PROTO_VERSION,
        .base_id   = s_base,
        /* Keyed on actually HOLDING a file, not on the revision. The two
         * came apart the moment a track was deleted: the revision is the sync
         * watermark and has to stay put, or the peer instantly looks newer
         * and pushes the circuit straight back — deleting would not stick. */
        .caps      = RDM_BUS_CAP_WANTS_OUTLINE |
                     (s_track_name[0] ? (RDM_BUS_CAP_HAS_OUTLINE | RDM_BUS_CAP_CAN_SEND) : 0),
        .track_rev = s_track_rev,
    };
    uint8_t f[8];
    rdm_bus_announce_pack(&a, f);
    can_transmit_frame(RDM_BUS_DISCOVERY_ID, f, 8);
}

static void _reconcile(void) {
    if (s_state != ST_IDLE) return;
    if (!s_peer_base) return;
    if (_ms_since(s_peer_seen_us) > BUS_PEER_STALE_MS) return;
    if (!(s_peer_caps & RDM_BUS_CAP_HAS_OUTLINE)) return;
    if (!(s_peer_caps & RDM_BUS_CAP_CAN_SEND)) return;
    if (s_peer_rev <= s_track_rev) return;             /* highest rev wins */
    if (_car_is_moving()) return;                      /* not mid-lap */

    rdm_bus_ctrl_t req = { .msg = RDM_BUS_MSG_REQUEST, .track_rev = s_peer_rev };
    _send_ctrl_to_peer(&req);
    s_state_us = esp_timer_get_time();
}

static void _tick(lv_timer_t *t) {
    (void)t;
    s_tick++;

    /* Announce rate backs off on a bus that has never said anything. A dash
     * wired to nothing (a bench, or a single-device install) has no other node
     * to ACK its frames, so every announce fails and retries — the bench dash
     * reached 17.8 million bus errors in a minute doing exactly this. One
     * received frame, from anything, is proof there is somebody out there and
     * puts it straight back to 1 Hz. */
    bool bus_is_alive = (can_get_rx_frame_count() > 0);
    uint32_t every = bus_is_alive ? BUS_ANNOUNCE_EVERY : BUS_ANNOUNCE_EVERY_QUIET;
    if ((s_tick % every) == 0) {
        _announce();
        _reconcile();
    }

    _pump_send();

    /* A stalled receive NAKs its first gap rather than waiting out the whole
     * timeout — the common stall is a handful of frames lost to a crank, and
     * naming the gap restarts the flow in one round trip. */
    if (s_state == ST_RECEIVING) {
        uint32_t idle = _ms_since(s_state_us);
        if (idle > BUS_NAK_GAP_MS && _ms_since(s_last_nak_us) > BUS_NAK_GAP_MS) {
            int32_t miss = rdm_bus_rx_first_missing(&s_rx);
            if (miss >= 0) {
                rdm_bus_ctrl_t n = { .msg = RDM_BUS_MSG_NAK, .track_rev = s_rx_rev,
                                     .seq = (uint16_t)miss };
                _send_ctrl_to_peer(&n);
                s_last_nak_us = esp_timer_get_time();
            }
        }
        if (idle > BUS_RX_TIMEOUT_MS) {
            ESP_LOGW(TAG, "transfer timed out — will retry on the next announce");
            _abort(RDM_BUS_ABORT_TIMEOUT);
        }
    }

    if (s_state == ST_SENDING && _ms_since(s_state_us) > BUS_RX_TIMEOUT_MS * 4)
        s_state = ST_IDLE;
}

/* ── public ───────────────────────────────────────────────────────────────── */

void rdm_bus_on_can_frame(uint32_t id, bool extd, const uint8_t *data, uint8_t dlc) {
    if (extd || !data) return;                       /* 11-bit only */

    if (id == RDM_BUS_DISCOVERY_ID) { _on_announce(data, dlc); return; }
    if (!s_peer_base) return;

    if (id == (uint32_t)(s_peer_base + RDM_BUS_OFF_ASSET_TX)) {
        _on_asset_from_peer(data, dlc);
    } else if (id == (uint32_t)(s_peer_base + RDM_BUS_OFF_ASSET_RX)) {
        _on_ctrl_to_us(data, dlc);
    }
}

void rdm_bus_set_local_track(const char *name, uint32_t rev) {
    if (name) snprintf(s_track_name, sizeof(s_track_name), "%s", name);
    s_track_rev = rev & RDM_BUS_TRACK_REV_MAX;
    config_store_save_bus_track(s_track_name, s_track_rev);
}

uint32_t rdm_bus_local_rev(void) { return s_track_rev; }

const char *rdm_bus_state_str(void) {
    switch (s_state) {
    case ST_RECEIVING: return "receiving";
    case ST_SENDING:   return "sending";
    default:           return "idle";
    }
}

const char *rdm_bus_local_track(void) { return s_track_name; }

void rdm_bus_forget_local_track(const char *name) {
    /* Only forget the one that was actually deleted. Deleting some other
     * circuit must not stop us advertising the one we still hold. */
    if (!name || s_track_name[0] == '\0') return;
    if (strcmp(name, s_track_name) != 0) return;

    /* Name goes, revision STAYS. The revision is the high-water mark of what
     * this dash has seen, so holding it means the peer's copy is not "newer"
     * and does not come flooding back the instant it is deleted. Pushing a
     * new track from Studio bumps the revision and starts things moving
     * again, which is the only place a delete should be undone. */
    s_track_name[0] = '\0';
    config_store_save_bus_track(s_track_name, s_track_rev);
}

void rdm_bus_set_base(uint16_t base) {
    if (base > 0x7F0u || (base & 0x0Fu)) return;
    s_base = base;
    config_store_save_bus_base(base);
    _announce();   /* tell the peer immediately rather than making it wait */
}

uint16_t rdm_bus_get_base(void) { return s_base; }

bool rdm_bus_peer_info(uint16_t *base_out, uint32_t *rev_out, uint32_t *age_ms_out) {
    if (!s_peer_base) return false;
    if (base_out)   *base_out = s_peer_base;
    if (rev_out)    *rev_out = s_peer_rev;
    if (age_ms_out) *age_ms_out = _ms_since(s_peer_seen_us);
    return true;
}

void rdm_bus_init(void) {
    uint16_t b = 0;
    if (config_store_load_bus_base(&b) == ESP_OK && b && b <= 0x7F0u && !(b & 0x0Fu))
        s_base = b;
    uint32_t rev = 0;
    config_store_load_bus_track(s_track_name, sizeof(s_track_name), &rev);
    s_track_rev = rev & RDM_BUS_TRACK_REV_MAX;

    /* The LVGL task is already running lv_timer_handler() on the other core
     * and lv_timer_create() mutates the shared global timer list, so this must
     * hold the lock — the same reason the WiFi-on-boot timer in main.c does.
     * Calling it before lv_init() panicked the whole dash on boot and the
     * bootloader rolled the image back, which reads as "the flash didn't take"
     * rather than as a crash. */
    if (!s_timer) {
        if (rdm_lvgl_lock(-1)) {
            s_timer = lv_timer_create(_tick, BUS_TICK_MS, NULL);
            rdm_lvgl_unlock();
        } else {
            ESP_LOGE(TAG, "could not take the LVGL lock — bus timer not started");
        }
    }
    ESP_LOGI(TAG, "bus up: base 0x%03X, discovery 0x%03X, track '%s' rev %lu",
             s_base, RDM_BUS_DISCOVERY_ID, s_track_name, (unsigned long)s_track_rev);
}
