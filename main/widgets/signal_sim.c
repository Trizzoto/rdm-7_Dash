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
#include "lvgl.h"
#include "esp_log.h"
#include <string.h>

#define SIM_MAX_SIGNALS 128
#define SIM_TIMER_PERIOD_MS 40      /* 25 Hz — uniform rate for every signal  */
#define SIM_CYCLE_MS 3000

static const char *TAG = "signal_sim";

typedef struct {
    float min;
    float max;
} sim_bounds_t;

static bool         s_sim_active = false;
static lv_timer_t  *s_sim_timer  = NULL;
static sim_bounds_t s_bounds[SIM_MAX_SIGNALS];
static float        s_phase[SIM_MAX_SIGNALS];
static uint16_t     s_cached_count = 0;

static void _rebuild_signal_state(uint16_t count)
{
    /* All signals start with default bounds 0-100; the widget scan below
     * overwrites with real ranges for widget types that expose min/max. */
    for (uint16_t i = 0; i < count && i < SIM_MAX_SIGNALS; i++) {
        s_bounds[i].min = 0.0f;
        s_bounds[i].max = 100.0f;
    }

    widget_t *widgets[WIDGET_REGISTRY_MAX];
    uint8_t wcount = 0;
    widget_registry_snapshot(widgets, WIDGET_REGISTRY_MAX, &wcount);

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
        default:
            /* Widget types without explicit min/max keep the 0-100 default. */
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
    /* All signals start at phase 0 so they sweep in sync */
    for (uint16_t i = 0; i < count && i < SIM_MAX_SIGNALS; i++) {
        s_phase[i] = 0.0f;
    }
}

static void _sim_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    uint16_t count = signal_get_count();
    if (count == 0) return;
    if (count > SIM_MAX_SIGNALS) count = SIM_MAX_SIGNALS;

    if (count != s_cached_count) {
        _rebuild_signal_state(count);
        _init_phases(count);
        s_cached_count = count;
    }

    const float delta = (float)SIM_TIMER_PERIOD_MS / (float)SIM_CYCLE_MS;

    for (uint16_t i = 0; i < count; i++) {
        signal_t *sig = signal_get_by_index(i);
        if (!sig) continue;

        /* Skip internal signals (can_id 0) — leave real sensor values */
        if (sig->can_id == 0) continue;

        s_phase[i] += delta;
        if (s_phase[i] >= 1.0f) s_phase[i] -= 1.0f;

        /* Triangle wave: ramp up 0-0.5, ramp down 0.5-1.0 */
        float t = s_phase[i] < 0.5f
                    ? s_phase[i] * 2.0f
                    : (1.0f - s_phase[i]) * 2.0f;

        float value = s_bounds[i].min + t * (s_bounds[i].max - s_bounds[i].min);
        signal_inject_test_value(sig->name, value);
    }
}

void signal_sim_start(void)
{
    if (s_sim_active) return;

    uint16_t count = signal_get_count();
    if (count > SIM_MAX_SIGNALS) count = SIM_MAX_SIGNALS;

    _rebuild_signal_state(count);
    _init_phases(count);
    s_cached_count = count;

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
