/**
 * heap_monitor.c — see heap_monitor.h
 *
 * One lv_timer fires every HEAP_MONITOR_INTERVAL_MS, captures heap
 * stats, and emits a single-line log. Single-line format is intentional
 * so a serial filter ("grep heap_mon") yields a clean time-series the
 * user can paste into a spreadsheet to see trends.
 *
 * Every Nth tick (HEAP_MONITOR_TIMER_DUMP_EVERY), also dump the active
 * esp_timer list — this is the smoking gun for the PHY-leak theory.
 * If "phy-track-pll-timer" appears multiple times in successive dumps,
 * the leak is confirmed at the IDF level (the static handle in
 * phy_common.c gets overwritten without freeing the previous one).
 */
#include "heap_monitor.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <stdio.h>

static const char *TAG = "heap_mon";

static lv_timer_t *s_timer        = NULL;
static uint32_t    s_tick_count   = 0;
/* Last snapshot to compute deltas. -1 sentinels mean "no baseline yet". */
static int32_t     s_last_int_free   = -1;
static int32_t     s_last_int_largest= -1;
static int32_t     s_last_psram_free = -1;

static void _snapshot(const char *tag) {
    size_t int_free    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t psram_free  = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    /* Compute deltas if we have a baseline. */
    int32_t d_int     = (s_last_int_free    < 0) ? 0 : (int32_t)int_free    - s_last_int_free;
    int32_t d_largest = (s_last_int_largest < 0) ? 0 : (int32_t)int_largest - s_last_int_largest;
    int32_t d_psram   = (s_last_psram_free  < 0) ? 0 : (int32_t)psram_free  - s_last_psram_free;

    ESP_LOGI(TAG, "[%s] int_free=%u (d=%+d) int_largest=%u (d=%+d) psram_free=%u (d=%+d)",
             tag ? tag : "tick",
             (unsigned)int_free,    (int)d_int,
             (unsigned)int_largest, (int)d_largest,
             (unsigned)psram_free,  (int)d_psram);

    s_last_int_free    = (int32_t)int_free;
    s_last_int_largest = (int32_t)int_largest;
    s_last_psram_free  = (int32_t)psram_free;
}

static void _tick_cb(lv_timer_t *t) {
    (void)t;
    _snapshot("tick");
    s_tick_count++;

    /* Periodically dump every esp_timer in the system. esp_timer_dump
     * writes one row per timer to stdout — they appear as plain text
     * in the serial monitor (no ESP_LOG prefix because it uses fprintf
     * directly). Look for repeated "phy-track-pll-timer" rows across
     * successive dumps — that's the IDF leak signature. */
#if HEAP_MONITOR_TIMER_DUMP_EVERY > 0
    if (HEAP_MONITOR_TIMER_DUMP_EVERY > 0 &&
        (s_tick_count % HEAP_MONITOR_TIMER_DUMP_EVERY) == 0) {
        ESP_LOGI(TAG, "esp_timer_dump (look for repeated 'phy-track-pll-timer'):");
        esp_timer_dump(stdout);
    }
#endif
}

void heap_monitor_start(void) {
    if (s_timer) return;
    s_tick_count = 0;
    /* Don't reset s_last_* — they're sentinels (-1) until first snapshot. */
    s_timer = lv_timer_create(_tick_cb, HEAP_MONITOR_INTERVAL_MS, NULL);
    ESP_LOGI(TAG, "Heap monitor started — %u ms cadence, timer dump every %u ticks",
             (unsigned)HEAP_MONITOR_INTERVAL_MS,
             (unsigned)HEAP_MONITOR_TIMER_DUMP_EVERY);
    /* Prime the baseline immediately so the first tick has deltas. */
    _snapshot("boot");
}

void heap_monitor_stop(void) {
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
}

void heap_monitor_snapshot_now(const char *tag) {
    _snapshot(tag);
}
