/* signal_sim.c - build fake CAN frames and dispatch them through the real
 * signal_dispatch_frame path. This is MUCH closer to how actual CAN traffic
 * hits the dashboard: bursts are coalesced per-frame (multiple signals on
 * one CAN ID move together), the change-detection gate in signal_dispatch_frame
 * runs as normal, and widget callbacks see the same code path they would with
 * a real ECU connected. FPS behaviour should match real-CAN behaviour because
 * it IS the real-CAN code path.
 *
 * Previous implementation used signal_inject_test_value() which bypassed the
 * decode layer entirely and caused inconsistent perf vs. real CAN.
 */

#include "signal_sim.h"
#include "signal.h"
#include "can_decode.h"
#include "widget_types.h"
#include "widget_registry.h"
#include "widget_meter.h"
#include "widget_bar.h"
#include "widget_rpm_bar.h"
#include "widget_warning.h"
#include "widget_panel.h"
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>

#define SIM_MAX_SIGNALS 128
#define SIM_MAX_FRAMES  64    /* unique CAN IDs per layout */
/* Tick rate tuned to match a real car's fast broadcast (25ms = 40 Hz per
 * frame, similar to what Haltech/MaxxECU put on a loaded bus). Cycle =
 * 3s for each signal's full sweep - brisk enough for demos and mirrors
 * what a car at wide-open throttle would generate. */
#define SIM_TIMER_PERIOD_MS 25
#define SIM_CYCLE_MS 3000

static const char *TAG = "signal_sim";

typedef struct {
    float min;
    float max;
} sim_bounds_t;

typedef struct {
    uint32_t can_id;        /* 0 = unused slot */
    uint8_t  max_end_byte;  /* Highest byte touched by any signal (for DLC) */
} sim_frame_t;

static bool          s_sim_active = false;
static lv_timer_t   *s_sim_timer  = NULL;
static sim_bounds_t  s_bounds[SIM_MAX_SIGNALS];
static float         s_phase[SIM_MAX_SIGNALS];
static uint16_t      s_cached_count = 0;
static sim_frame_t   s_frames[SIM_MAX_FRAMES];
static uint16_t      s_frame_count = 0;

/* ── Bounds ------------------------------------------------------------- */

static void _build_bounds(uint16_t count)
{
    /* Default all signals to 0-100 */
    for (uint16_t i = 0; i < count && i < SIM_MAX_SIGNALS; i++) {
        s_bounds[i].min = 0.0f;
        s_bounds[i].max = 100.0f;
    }

    /* Scan widget registry for meaningful bounds */
    widget_t *widgets[32];
    uint8_t wcount = 0;
    widget_registry_snapshot(widgets, 32, &wcount);

    for (uint8_t wi = 0; wi < wcount; wi++) {
        widget_t *w = widgets[wi];
        if (!w || !w->type_data) continue;

        const char *sig_name = NULL;
        float mn = 0.0f;
        float mx = 100.0f;

        switch (w->type) {
        case WIDGET_METER: {
            meter_data_t *md = (meter_data_t *)w->type_data;
            sig_name = md->signal_name;
            mn = md->min;
            mx = md->max;
            break;
        }
        case WIDGET_BAR: {
            bar_data_t *bd = (bar_data_t *)w->type_data;
            sig_name = bd->signal_name;
            mn = bd->bar_min;
            mx = bd->bar_max;
            break;
        }
        case WIDGET_RPM_BAR: {
            rpm_bar_data_t *rd = (rpm_bar_data_t *)w->type_data;
            sig_name = rd->signal_name;
            mn = 0.0f;
            mx = rd->gauge_max;
            break;
        }
        case WIDGET_WARNING: {
            warning_data_t *wd = (warning_data_t *)w->type_data;
            sig_name = wd->signal_name;
            mn = 0.0f;
            mx = 1.0f;
            break;
        }
        default:
            continue;
        }

        if (!sig_name || sig_name[0] == '\0') continue;

        int16_t idx = signal_find_by_name(sig_name);
        if (idx < 0 || idx >= SIM_MAX_SIGNALS) continue;

        s_bounds[idx].min = mn;
        s_bounds[idx].max = mx;
    }
}

static void _init_phases(uint16_t count)
{
    /* Stagger phases slightly so frames for different CAN IDs don't all
     * peak at the same moment - looks more like real bursty CAN traffic. */
    for (uint16_t i = 0; i < count && i < SIM_MAX_SIGNALS; i++) {
        s_phase[i] = ((float)(i * 37) / 256.0f);
        while (s_phase[i] >= 1.0f) s_phase[i] -= 1.0f;
    }
}

/* ── Build the list of unique CAN IDs to dispatch each tick ------------- */

static void _build_frame_list(uint16_t count)
{
    s_frame_count = 0;
    for (uint16_t i = 0; i < count && i < SIM_MAX_SIGNALS; i++) {
        signal_t *sig = signal_get_by_index(i);
        if (!sig || sig->can_id == 0) continue;  /* skip internal signals */

        /* Find or create a frame slot for this CAN ID */
        int frame_idx = -1;
        for (uint16_t f = 0; f < s_frame_count; f++) {
            if (s_frames[f].can_id == sig->can_id) {
                frame_idx = (int)f;
                break;
            }
        }
        if (frame_idx < 0) {
            if (s_frame_count >= SIM_MAX_FRAMES) continue;
            frame_idx = s_frame_count++;
            s_frames[frame_idx].can_id = sig->can_id;
            s_frames[frame_idx].max_end_byte = 0;
        }

        uint8_t end_byte = (uint8_t)((sig->bit_start + sig->bit_length - 1) / 8);
        if (end_byte > s_frames[frame_idx].max_end_byte) {
            s_frames[frame_idx].max_end_byte = end_byte;
        }
    }
    ESP_LOGI(TAG, "Built frame list: %u unique CAN IDs from %u signals",
             s_frame_count, count);
}

/* ── Inverse decode: compute the raw bit field for a target engineering
 *    value. can_pack_bits takes an unsigned raw, so we clamp negative raws
 *    into the bit_length's 2's-complement range if the signal is signed. */

static uint32_t _value_to_raw(const signal_t *sig, float value)
{
    float scale = sig->scale;
    if (scale == 0.0f) scale = 1.0f;
    float raw_f = (value - sig->offset) / scale;

    int32_t raw_i = (int32_t)raw_f;

    uint32_t mask = sig->bit_length >= 32 ? 0xFFFFFFFFu
                                          : ((1u << sig->bit_length) - 1);
    if (sig->is_signed) {
        /* Clamp to 2's-complement range for bit_length */
        int32_t max_pos = (int32_t)(mask >> 1);
        int32_t min_neg = -max_pos - 1;
        if (raw_i > max_pos) raw_i = max_pos;
        if (raw_i < min_neg) raw_i = min_neg;
    } else {
        if (raw_i < 0) raw_i = 0;
        if ((uint32_t)raw_i > mask) raw_i = (int32_t)mask;
    }
    return ((uint32_t)raw_i) & mask;
}

/* ── Per-tick: build each frame from the signals that share its CAN ID,
 *    then dispatch through the real CAN path. This is the key change that
 *    makes sim behave identically to real CAN traffic. */

static void _sim_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    uint16_t count = signal_get_count();
    if (count == 0) return;
    if (count > SIM_MAX_SIGNALS) count = SIM_MAX_SIGNALS;

    if (count != s_cached_count) {
        _build_bounds(count);
        _init_phases(count);
        _build_frame_list(count);
        s_cached_count = count;
    }

    const float delta = (float)SIM_TIMER_PERIOD_MS / (float)SIM_CYCLE_MS;

    /* Advance phase for every bound signal */
    for (uint16_t i = 0; i < count; i++) {
        signal_t *sig = signal_get_by_index(i);
        if (!sig || sig->can_id == 0) continue;
        s_phase[i] += delta;
        if (s_phase[i] >= 1.0f) s_phase[i] -= 1.0f;
    }

    /* For each unique CAN ID, build an 8-byte frame with all its signals
     * packed in, then hand it to signal_dispatch_frame as if it had just
     * arrived on the bus. */
    for (uint16_t f = 0; f < s_frame_count; f++) {
        uint32_t can_id = s_frames[f].can_id;
        uint8_t  frame[8] = {0};

        for (uint16_t i = 0; i < count; i++) {
            signal_t *sig = signal_get_by_index(i);
            if (!sig || sig->can_id != can_id) continue;

            /* Triangle wave: 0 -> max -> 0 */
            float t = s_phase[i] < 0.5f
                        ? s_phase[i] * 2.0f
                        : (1.0f - s_phase[i]) * 2.0f;
            float value = s_bounds[i].min + t * (s_bounds[i].max - s_bounds[i].min);

            uint32_t raw = _value_to_raw(sig, value);
            can_pack_bits(frame, sig->bit_start, sig->bit_length, raw, sig->endian);
        }

        uint8_t dlc = (uint8_t)(s_frames[f].max_end_byte + 1);
        if (dlc < 1) dlc = 1;
        if (dlc > 8) dlc = 8;
        signal_dispatch_frame(can_id, frame, dlc);
    }
}

/* ── Public API --------------------------------------------------------- */

void signal_sim_start(void)
{
    if (s_sim_active) return;

    uint16_t count = signal_get_count();
    if (count > SIM_MAX_SIGNALS) count = SIM_MAX_SIGNALS;

    _build_bounds(count);
    _init_phases(count);
    _build_frame_list(count);
    s_cached_count = count;

    s_sim_active = true;
    s_sim_timer = lv_timer_create(_sim_timer_cb, SIM_TIMER_PERIOD_MS, NULL);

    ESP_LOGI(TAG, "Signal simulator started (%u signals, %u frames)",
             count, s_frame_count);
}

void signal_sim_stop(void)
{
    if (!s_sim_active) return;

    if (s_sim_timer) {
        lv_timer_del(s_sim_timer);
        s_sim_timer = NULL;
    }

    s_sim_active = false;
    s_cached_count = 0;

    /* Let the 2s signal timeout naturally mark signals stale */
    ESP_LOGI(TAG, "Signal simulator stopped");
}

bool signal_sim_is_active(void)
{
    return s_sim_active;
}
