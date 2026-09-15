/*
 * can_sim.c — see can_sim.h.
 */
#include "can_sim.h"

#include "can_manager.h"
#include "can_decode.h"
#include "ui/settings/preset_picker.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *TAG = "can_sim";

#define SIM_MAX_FRAMES  48       /* distinct (id, mux) payloads */
#define SIM_PERIOD_MS   100      /* every payload at 10 Hz */
#define SIM_RATE_HZ     (1000 / SIM_PERIOD_MS)

typedef struct {
    uint32_t id;
    uint16_t mux;
    uint8_t  mux_start, mux_len;
    uint8_t  endian;
    uint8_t  dlc;
    int      first_row, n_rows;   /* rows in s_rows[] that live in this frame */
} sim_frame_t;

typedef struct {
    const preconfig_item_t *it;
    int8_t map[64];               /* frame bit (byte*8+bit) -> field bit, -1 = not in field */
} sim_row_t;

/* PSRAM, like every table the menus added: internal RAM is WiFi's at boot. */
static EXT_RAM_BSS_ATTR sim_frame_t s_frames[SIM_MAX_FRAMES];
static int s_n_frames = 0;
static sim_row_t *s_rows = NULL;
static int s_n_rows = 0;
static char s_ecu[32], s_version[32];
static volatile bool s_active = false;
static TaskHandle_t s_task = NULL;
static uint8_t s_bitrate_idx = 3;

/* ── Which frame bit is which field bit, asked of the real decoder ─────── */

static void _map_row(sim_row_t *r, uint8_t start, uint8_t len, uint8_t endian)
{
    memset(r->map, -1, sizeof(r->map));
    for (int b = 0; b < 64; b++) {
        uint8_t data[8] = {0};
        data[b / 8] = (uint8_t)(1u << (b % 8));
        int64_t v = can_extract_bits(data, start, len, endian, false);
        if (v <= 0) continue;
        uint64_t u = (uint64_t)v;
        if (u & (u - 1)) continue;            /* not a single bit: ignore */
        int k = 0;
        while (u >>= 1) k++;
        r->map[b] = (int8_t)k;
    }
}

static void _put_field(uint8_t *data, const int8_t *map, uint64_t raw)
{
    for (int b = 0; b < 64; b++) {
        if (map[b] < 0) continue;
        if (raw & (1ull << map[b])) data[b / 8] |= (uint8_t)(1u << (b % 8));
        else data[b / 8] &= (uint8_t)~(1u << (b % 8));
    }
}

/* ── What an idling, warm engine reads ─────────────────────────────────── */

static bool _has(const char *label, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = label; *p; p++)
        if (!strncasecmp(p, word, n)) return true;
    return false;
}

static float _value_for(const preconfig_item_t *it, float t)
{
    const char *label = it->label;
    float wob = sinf(t * 1.3f) * 0.5f + sinf(t * 0.37f) * 0.5f;   /* -1..1, slow */
    /* One mixture, whatever unit a row carries it in: Haltech sends each
     * wideband once and the preset reads those bytes both as AFR (0.0147/bit)
     * and as lambda (0.001/bit), so both rows must encode the same raw value
     * or the later row in a frame overwrites the other with a different one. */
    if (!_has(label, "TARGET") && !_has(label, "CORRECTION") &&
        (_has(label, "WIDEBAND") || _has(label, "LAMBDA") || _has(label, "AFR"))) {
        float lambda = 0.99f + 0.01f * wob;
        float full_scale = (float)((1ull << it->bit_length) - 1) * it->scale;
        return full_scale > 4.0f ? lambda * 14.7f : lambda;   /* AFR-scaled row */
    }
    if (_has(label, "RPM") && !_has(label, "LAUNCH")) return 850.0f + 25.0f * wob;
    if (_has(label, "LAUNCH END RPM"))    return 4500.0f;
    if (_has(label, "COOLANT PRESS"))     return 115.0f;
    if (_has(label, "COOLANT"))           return 86.0f + 0.4f * wob;
    if (_has(label, "OIL PRESS"))         return 290.0f + 8.0f * wob;
    if (_has(label, "OIL TEMP") && !_has(label, "GEARBOX") && !_has(label, "DIFF"))
                                          return 91.0f;
    if (_has(label, "GEARBOX OIL"))       return 58.0f;
    if (_has(label, "DIFF OIL"))          return 46.0f;
    if (_has(label, "FUEL TEMP"))         return 29.0f;
    if (_has(label, "AMBIENT"))           return 23.0f;
    if (_has(label, "AIR TEMP") || _has(label, "INTAKE AIR") || _has(label, "IAT"))
                                          return 31.0f + 0.3f * wob;
    if (_has(label, "FUEL PRESS"))        return 300.0f + 3.0f * wob;
    if (_has(label, "WASTEGATE"))         return 0.0f;
    if (_has(label, "BARO"))              return 101.3f;
    if (_has(label, "MANIFOLD") || _has(label, "MAP"))
                                          return 36.0f + 1.5f * wob;
    if (_has(label, "BOOST"))             return -65.0f;
    if (_has(label, "THROTTLE") || _has(label, "TPS"))
                                          return 1.8f + 0.2f * wob;
    if (_has(label, "PEDAL"))             return 0.0f;
    if (_has(label, "BATT") || _has(label, "VOLT"))
                                          return 14.1f + 0.05f * wob;
    if (_has(label, "TARGET") && (_has(label, "LAMBDA") || _has(label, "AFR")))
                                          return _has(label, "AFR") ? 14.7f : 1.00f;
    if (_has(label, "FUEL LEVEL"))        return 62.0f;
    if (_has(label, "TRIM"))              return 1.2f * wob;
    if (_has(label, "IGN") || _has(label, "TIMING") || _has(label, "ADVANCE"))
                                          return 14.0f + 0.5f * wob;
    if (_has(label, "INJ"))               return 2.6f;
    if (_has(label, "EGT"))               return 420.0f + 6.0f * wob;
    if (_has(label, "HUMIDITY"))          return 45.0f;
    if (_has(label, "SPEED") && !_has(label, "TURBO")) return 0.0f;
    if (_has(label, "GEAR"))              return 0.0f;
    if (_has(label, "SYNC"))              return 1.0f;
    if (_has(label, "ETHANOL") || _has(label, "FUEL COMP")) return 10.0f;
    return 0.0f;   /* pressures, g, knock, slip, counters: at rest */
}

static uint64_t _raw_for(const preconfig_item_t *it, float phys)
{
    float scale = it->scale != 0.0f ? it->scale : 1.0f;
    double r = round(((double)phys - it->value_offset) / scale);
    uint8_t len = it->bit_length;
    uint64_t mask = len >= 64 ? ~0ull : ((1ull << len) - 1);
    if (it->is_signed) {
        double lo = -(double)(1ull << (len - 1)), hi = (double)((1ull << (len - 1)) - 1);
        if (r < lo) r = lo;
        if (r > hi) r = hi;
        return (uint64_t)(int64_t)r & mask;
    }
    if (r < 0) r = 0;
    if (r > (double)mask) r = (double)mask;
    return (uint64_t)r & mask;
}

/* ── Broadcast task ────────────────────────────────────────────────────── */

static void _sim_task(void *arg)
{
    (void)arg;
    while (s_active) {
        float t = (float)(esp_timer_get_time() / 1000) / 1000.0f;
        for (int f = 0; f < s_n_frames && s_active; f++) {
            sim_frame_t *fr = &s_frames[f];
            uint8_t data[8] = {0};
            for (int i = 0; i < fr->n_rows; i++) {
                sim_row_t *row = &s_rows[fr->first_row + i];
                _put_field(data, row->map, _raw_for(row->it, _value_for(row->it, t)));
            }
            if (fr->mux_len) {
                sim_row_t mux_row;
                _map_row(&mux_row, fr->mux_start, fr->mux_len, fr->endian);
                _put_field(data, mux_row.map, fr->mux);
            }
            can_inject_rx_frame(fr->id, fr->id > 0x7FF, data, fr->dlc);
        }
        vTaskDelay(pdMS_TO_TICKS(SIM_PERIOD_MS));
    }
    s_task = NULL;
    vTaskDeleteWithCaps(NULL);
}

/* ── Public ────────────────────────────────────────────────────────────── */

static int _frame_for(uint32_t id, const preconfig_item_t *it)
{
    uint16_t mux = it->mux_bit_length ? it->mux_value : 0;
    for (int f = 0; f < s_n_frames; f++)
        if (s_frames[f].id == id && s_frames[f].mux == mux && s_frames[f].mux_len == it->mux_bit_length)
            return f;
    return -1;
}

bool can_sim_start(const char *ecu, const char *version)
{
    can_sim_stop();
    if (!ecu || !version) return false;

    /* Rows of this preset that are CAN broadcasts (not OBD2 PIDs), grouped
     * by payload so each frame is built once with all of its fields. */
    int n = 0;
    for (int i = 0; i < preconfig_items_count; i++) {
        const preconfig_item_t *it = &preconfig_items[i];
        if (it->ecu && it->version && !strcmp(it->ecu, ecu) && !strcmp(it->version, version) &&
            !it->obd2_pid && it->can_id && it->can_id[0])
            n++;
    }
    if (n == 0) return false;
    s_rows = heap_caps_calloc((size_t)n, sizeof(sim_row_t), MALLOC_CAP_SPIRAM);
    if (!s_rows) return false;

    s_n_frames = 0;
    s_n_rows = 0;
    /* Two passes: frames in first-seen order, then rows appended per frame. */
    for (int i = 0; i < preconfig_items_count && s_n_frames < SIM_MAX_FRAMES; i++) {
        const preconfig_item_t *it = &preconfig_items[i];
        if (!it->ecu || !it->version || strcmp(it->ecu, ecu) || strcmp(it->version, version) ||
            it->obd2_pid || !it->can_id || !it->can_id[0])
            continue;
        uint32_t id = (uint32_t)strtoul(it->can_id, NULL, 16);
        if (_frame_for(id, it) >= 0) continue;
        sim_frame_t *fr = &s_frames[s_n_frames++];
        memset(fr, 0, sizeof(*fr));
        fr->id = id;
        fr->mux = it->mux_bit_length ? it->mux_value : 0;
        fr->mux_start = it->mux_bit_start;
        fr->mux_len = it->mux_bit_length;
        fr->endian = it->endianess;
        fr->dlc = 8;
    }
    for (int f = 0; f < s_n_frames; f++) {
        s_frames[f].first_row = s_n_rows;
        for (int i = 0; i < preconfig_items_count; i++) {
            const preconfig_item_t *it = &preconfig_items[i];
            if (!it->ecu || !it->version || strcmp(it->ecu, ecu) || strcmp(it->version, version) ||
                it->obd2_pid || !it->can_id || !it->can_id[0])
                continue;
            if (_frame_for((uint32_t)strtoul(it->can_id, NULL, 16), it) != f) continue;
            sim_row_t *row = &s_rows[s_n_rows++];
            row->it = it;
            _map_row(row, it->bit_start, it->bit_length, it->endianess);
            s_frames[f].n_rows++;
        }
    }

    snprintf(s_ecu, sizeof(s_ecu), "%s", ecu);
    snprintf(s_version, sizeof(s_version), "%s", version);
    /* Aftermarket ECUs broadcast at 1 Mbps (Haltech, Link, MaxxECU, ECU
     * Master); the factory streams in the table are 500 k. */
    s_bitrate_idx = (!strcasecmp(ecu, "Ford") || !strcasecmp(ecu, "Toyota") ||
                     !strcasecmp(ecu, "Subaru")) ? 2 : 3;

    s_active = true;
    if (xTaskCreatePinnedToCoreWithCaps(_sim_task, "can_sim", 4096, NULL, 3, &s_task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_active = false;
        heap_caps_free(s_rows);
        s_rows = NULL;
        ESP_LOGE(TAG, "could not start the broadcast task");
        return false;
    }
    ESP_LOGI(TAG, "broadcasting %s %s: %d frames, %d channels", ecu, version, s_n_frames, s_n_rows);
    return true;
}

void can_sim_stop(void)
{
    if (!s_active && !s_task) return;
    s_active = false;
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_rows) {
        heap_caps_free(s_rows);
        s_rows = NULL;
    }
    s_n_frames = s_n_rows = 0;
    ESP_LOGI(TAG, "stopped");
}

bool can_sim_active(void) { return s_active; }
const char *can_sim_ecu(void) { return s_active ? s_ecu : ""; }
const char *can_sim_version(void) { return s_active ? s_version : ""; }

uint32_t can_sim_scan_frames(uint8_t bitrate_idx, uint32_t listen_ms,
                             uint32_t *ids, uint8_t max, uint8_t *n_ids)
{
    if (n_ids) *n_ids = 0;
    if (!s_active || bitrate_idx != s_bitrate_idx) return 0;
    uint8_t k = 0;
    for (int f = 0; f < s_n_frames; f++) {
        bool dup = false;
        for (uint8_t j = 0; j < k; j++) if (ids && ids[j] == s_frames[f].id) dup = true;
        if (dup || k >= max || !ids) continue;
        ids[k++] = s_frames[f].id;
    }
    if (n_ids) *n_ids = k;
    return (uint32_t)s_n_frames * SIM_RATE_HZ * listen_ms / 1000;
}
