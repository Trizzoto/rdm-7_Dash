#include "signal_sim.h"
#include "signal.h"
#include "widget_types.h"
#include "widget_registry.h"
#include "widget_meter.h"
#include "widget_bar.h"
#include "widget_rpm_bar.h"
#include "widget_warning.h"
#include "widget_arc.h"
#include "widget_shift_light.h"
#include "widget_pathbar.h"
#include "widget_text.h"
#include "widget_panel.h"
#include "widget_anim.h"
#include "data/canonical_channels.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <math.h>

#define SIM_MAX_SIGNALS     128
/* Aligned to the 16 ms LVGL refresh window. The dt_ms-based phase math in
 * _sim_timer_cb makes the animation speed independent of the timer rate, so
 * a slower tick doesn't slow the sweep — it just stops doing redundant work
 * (multiple injects between two frames the user never sees). */
#define SIM_TIMER_PERIOD_MS  16
#define SIM_CYCLE_MS        12000   /* full min->max->min sweep period; ~realistic
                                     * gentle motion (was 3000 = fast teleport that
                                     * spiked redraw load and looked unnatural) */
/* Batch size is chosen adaptively in _sim_timer_cb based on signal count. */

static const char *TAG = "signal_sim";

typedef struct {
    float min;
    float max;
} sim_bounds_t;

static bool         s_sim_active   = false;
static lv_timer_t  *s_sim_timer    = NULL;
static sim_bounds_t s_bounds[SIM_MAX_SIGNALS];
/* Which signals the active layout actually displays — only these get driven.
 * Lets the sim animate placeholder (can_id==0) signals on a fresh/bench device
 * while NOT touching internal signals (gear calc, fuel sender, DTC count, …)
 * that no widget shows. Rebuilt by _rebuild_signal_state on every (re)start. */
static bool         s_driven[SIM_MAX_SIGNALS];
static float        s_phase[SIM_MAX_SIGNALS];
/* Emit threshold state: last value actually injected per signal + a validity
 * flag. We only inject when the triangle-wave value has moved by at least
 * ~one needle-pixel of the signal's range, so a meter bound to a slow sweep
 * doesn't get a fresh value (and a repaint) every single timer tick. This
 * equalises sim vs real CAN, which only emits on a decoded value change. */
static float        s_last_emit[SIM_MAX_SIGNALS];
static bool         s_emit_valid[SIM_MAX_SIGNALS];
static uint16_t     s_cached_count = 0;
static uint16_t     s_sim_cursor   = 0;   /* round-robin position */
static uint64_t     s_last_tick_us = 0;   /* wall-clock time of previous tick */

static void _rebuild_signal_state(uint16_t count)
{
    /* All signals start with default bounds 0-100; the widget scan below
     * overwrites with real ranges for widget types that expose min/max. */
    for (uint16_t i = 0; i < count && i < SIM_MAX_SIGNALS; i++) {
        s_bounds[i].min = 0.0f;
        s_bounds[i].max = 100.0f;
        s_driven[i] = false;
    }

    widget_t *widgets[WIDGET_REGISTRY_MAX];
    uint8_t wcount = 0;
    widget_registry_snapshot(widgets, WIDGET_REGISTRY_MAX, &wcount);

    /* Pass 1 — gauges with an explicit min/max define the sweep precisely. */
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
        case WIDGET_ARC: {
            arc_data_t *ad = (arc_data_t *)w->type_data;
            sig_name = ad->signal_name;
            mn = ad->signal_min;
            mx = ad->signal_max;
            break;
        }
        case WIDGET_PATHBAR: {
            pathbar_data_t *pd = (pathbar_data_t *)w->type_data;
            sig_name = pd->signal_name;
            mn = pd->val_min;
            mx = pd->val_max;
            break;
        }
        case WIDGET_SHIFT_LIGHT: {
            shift_light_data_t *sd = (shift_light_data_t *)w->type_data;
            sig_name = sd->signal_name;
            /* Sweep from just below first-LED threshold to just past flash
             * threshold so we exercise off / progression / flash visuals. */
            mn = 0.0f;
            float top = sd->flash_threshold > sd->range_max
                            ? sd->flash_threshold
                            : sd->range_max;
            mx = top > 0.0f ? top * 1.05f : 100.0f;
            break;
        }
#if RDM_WIDGET_ANIM_ENABLED
        case WIDGET_ANIM: {
            /* Drive the animation across its active range so it scrubs / loops
             * on the bench instead of sitting dead on frame 0. */
            anim_data_t *ad = (anim_data_t *)w->type_data;
            sig_name = ad->signal_name;
            if (ad->mode == 1) {      /* trigger: sweep across the threshold */
                mn = 0.0f;
                mx = ad->threshold > 0.0f ? ad->threshold * 1.1f : 100.0f;
            } else {                  /* scrub: the configured value range */
                mn = ad->range_min;
                mx = ad->range_max;
            }
            break;
        }
#endif
        default:
            /* Widget types without explicit min/max keep the 0-100 default. */
            continue;
        }

        if (!sig_name || sig_name[0] == '\0') continue;

        int16_t idx = signal_find_by_name(sig_name);
        if (idx < 0 || idx >= SIM_MAX_SIGNALS) continue;

        s_bounds[idx].min = mn;
        s_bounds[idx].max = mx;
        s_driven[idx]     = true;
    }

    /* Pass 2 — number-only readouts (text / panel) have no range of their own.
     * Drive them too so digit-only layouts animate on the bench, using the
     * canonical display range for the signal (fallback 0-100). A gauge from
     * pass 1 already covering the same signal keeps its precise bounds. */
    for (uint8_t wi = 0; wi < wcount; wi++) {
        widget_t *w = widgets[wi];
        if (!w || !w->type_data) continue;

        const char *sig_name = NULL;
        switch (w->type) {
        case WIDGET_TEXT:  sig_name = ((text_data_t *)w->type_data)->signal_name;  break;
        case WIDGET_PANEL: sig_name = ((panel_data_t *)w->type_data)->signal_name; break;
        default: continue;
        }
        if (!sig_name || sig_name[0] == '\0') continue;

        int16_t idx = signal_find_by_name(sig_name);
        if (idx < 0 || idx >= SIM_MAX_SIGNALS) continue;
        if (s_driven[idx]) continue;   /* a gauge already owns the bounds */

        const canonical_channel_def_t *cc = canonical_channel_find_ci(sig_name);
        if (cc && cc->max_default > cc->min_default) {
            s_bounds[idx].min = cc->min_default;
            s_bounds[idx].max = cc->max_default;
        }
        s_driven[idx] = true;
    }
}

static void _init_phases(uint16_t count)
{
    /* All signals start at phase 0 so they sweep in sync. Also clear the
     * emit-threshold memo so the first tick of a (re)started sim — or a
     * signal-count change rebuild — always injects, repainting every widget
     * from a known state instead of being gated against a stale last-emit. */
    for (uint16_t i = 0; i < count && i < SIM_MAX_SIGNALS; i++) {
        s_phase[i] = 0.0f;
        s_last_emit[i]  = 0.0f;
        s_emit_valid[i] = false;
    }
}

static void _sim_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Use actual elapsed wall-clock time for phase advancement so the
     * animation speed stays correct regardless of how often this callback
     * fires (the LVGL task rate varies under load). */
    uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (s_last_tick_us == 0) s_last_tick_us = now_us;
    float dt_ms = (float)(now_us - s_last_tick_us) / 1000.0f;
    s_last_tick_us = now_us;

    uint16_t count = signal_get_count();
    if (count == 0) return;
    if (count > SIM_MAX_SIGNALS) count = SIM_MAX_SIGNALS;

    if (count != s_cached_count) {
        _rebuild_signal_state(count);
        _init_phases(count);
        s_cached_count = count;
        s_sim_cursor   = 0;
    }

    /* Cap dt so a heavy render tick doesn't cause an outsized jump. */
    if (dt_ms > 30.0f) dt_ms = 30.0f;

    /* Adaptive batch: keeps per-tick widget invalidations manageable.
     *   ≤12 signals → divisor 1 → inject every tick  (~71 Hz, smooth)
     *   13–24       → divisor 2 → inject every 2nd   (~35 Hz)
     *   25+         → divisor 4 → inject every 4th   (~18 Hz) */
    uint16_t divisor = (count <= 12) ? 1 : (count <= 24) ? 2 : 4;
    uint16_t batch   = (count + divisor - 1) / divisor;

    /* Phase advances ONLY inside the inject loop, by divisor ticks' worth of
     * time.  This guarantees each inject is exactly one step forward — no
     * accumulated skipping from phases advancing during non-inject ticks. */
    const float delta = (float)divisor * dt_ms / (float)SIM_CYCLE_MS;

    for (uint16_t j = 0; j < batch; j++) {
        uint16_t i = (s_sim_cursor + j) % count;
        signal_t *sig = signal_get_by_index(i);
        /* Drive only what the active layout displays — regardless of whether
         * the signal has a CAN decode yet. This makes a fresh/bench device
         * (placeholder signals, can_id==0) animate, while leaving undisplayed
         * internal signals (gear calc, fuel sender, DTC count) untouched. */
        if (!sig || i >= SIM_MAX_SIGNALS || !s_driven[i]) continue;

        s_phase[i] += delta;
        if (s_phase[i] >= 1.0f) s_phase[i] -= 1.0f;

        float t = s_phase[i] < 0.5f
                    ? s_phase[i] * 2.0f
                    : (1.0f - s_phase[i]) * 2.0f;

        float value = s_bounds[i].min + t * (s_bounds[i].max - s_bounds[i].min);

        /* Threshold the emission: only inject when the value moved by at least
         * ~1 needle-pixel of the signal's range (range/200). Below that the
         * receiving widget would round to the same integer and repaint for
         * nothing — the exact cost that made sim laggier than CAN. Phase has
         * already advanced above and the cursor advances after the loop, so
         * skipping the inject doesn't perturb the round-robin or phase math. */
        float step = (s_bounds[i].max - s_bounds[i].min) / 200.0f;
        if (step <= 0.0f) step = 0.5f;
        if (s_emit_valid[i] && fabsf(value - s_last_emit[i]) < step) {
            continue;
        }
        s_last_emit[i]  = value;
        s_emit_valid[i] = true;
        signal_inject_test_value(sig->name, value);
    }

    s_sim_cursor = (s_sim_cursor + batch) % count;
}

void signal_sim_start(void)
{
    if (s_sim_active) return;

    uint16_t count = signal_get_count();
    if (count > SIM_MAX_SIGNALS) count = SIM_MAX_SIGNALS;

    _rebuild_signal_state(count);
    _init_phases(count);
    s_cached_count = count;
    s_sim_cursor   = 0;
    s_last_tick_us = 0;

    s_sim_active = true;
    s_sim_timer = lv_timer_create(_sim_timer_cb, SIM_TIMER_PERIOD_MS, NULL);

    ESP_LOGI(TAG, "Signal simulator started (%u signals)", count);
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
