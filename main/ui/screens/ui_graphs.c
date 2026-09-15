/*
 * ui_graphs.c — Live graphs (ADR-0076). See ui_graphs.h.
 *
 * Sampling: a 20 Hz LVGL timer reads each picked channel's latest value (in
 * its display unit) into a 60 s ring. The plot redraws at 10 Hz from the ring;
 * nothing is drawn per sample, and nothing runs while the screen is closed.
 *
 * Drawing: the plot is an RGB565 image rendered by hand (see "Drawing"
 * below) with the axis numbers as labels over it. A window holding more
 * samples than there are pixel columns is drawn as each column's low and
 * high in the order they happened, so a lean spike still shows at 60 s
 * instead of being averaged away.
 *
 * Scale: channels with the same unit share one range (lambda and its target
 * must sit on the same axis to be compared); a different unit gets its own.
 * A range lands on round numbers, grows at once and shrinks only after the
 * tighter one has held for 3 s, so the axis doesn't twitch.
 */
#include "screens/ui_graphs.h"

#include "kit/ui_kit.h"
#include "menu/main_menu.h"
#include "screens/ui_peaks.h"
#include "data/channel_manager.h"
#include "data/unit_convert.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "system/screen_config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define G_MAX       4                   /* channels at once */
#define G_HZ        20                  /* samples per second */
#define G_SECS_MAX  60
#define G_CAP       (G_SECS_MAX * G_HZ)
#define G_PAD_L     48                  /* room for the axis numbers */
#define G_PAD_R     14
#define G_PAD_T     14
#define G_PAD_B     24                  /* room for the time marks */
/* The plot card: the body (screen less bar and padding) less the legend. */
#define GRAPH_LEGEND_W 236
#define GRAPH_CARD_W   (SCREEN_W - 2 * UK_PAD - GRAPH_LEGEND_W - UK_GAP)
#define GRAPH_CARD_H   (SCREEN_H - UK_BAR_H - 2 * UK_PAD)

/* Data colours, not theme colours: a trace keeps its colour in any theme. */
static const uint32_t k_colors[G_MAX] = { 0xE8433C, 0x38BDF8, 0xF0A53A, 0x3CCF7A };
static const uint8_t  k_windows[] = { 10, 30, 60 };

static const struct { const char *name, *a, *b; } k_pairs[] = {
    { "Lambda and target",   "lambda_bank1",         "target_lambda" },
    { "Wideband and target", "wideband_1",           "target_lambda" },
    { "AFR and target",      "afr_bank1",            "target_afr" },
    { "Boost and target",    "boost_pressure",       "boost_target" },
    { "Fuel trims",          "short_term_fuel_trim", "long_term_fuel_trim" },
    { "Knock and timing",    "knock_retard",         "ignition_timing" },
    { "RPM and throttle",    "rpm",                  "throttle_position" },
};

/* The picks outlive the screen (until restart). */
/* Every static array in the menu code lives in PSRAM (EXT_RAM_BSS_ATTR):
 * internal RAM is what esp_wifi_init needs at boot, and a few KB of menu
 * tables in it was enough to make WiFi start fail with ESP_ERR_NO_MEM and
 * the dash boot-loop (ADR-0076). */
static EXT_RAM_BSS_ATTR char s_pick[G_MAX][32];
static uint8_t s_npick = 0;
static bool    s_defaults_done = false;

typedef struct {
    char      id[32];
    float    *buf;        /* G_CAP samples, NAN = no reading */
    float     lo, hi;     /* range as drawn */
    bool      has_range;
    uint8_t   shrink_frames;
    lv_obj_t *value_lbl;
    lv_obj_t *range_lbl;
} g_series_t;

static EXT_RAM_BSS_ATTR g_series_t s_ser[G_MAX];
static uint16_t    s_head = 0;        /* next write slot */
static uint16_t    s_count = 0;       /* samples held */
static uint8_t     s_win = 10;
static bool        s_paused = false;

static lv_obj_t   *s_scr = NULL;
static lv_obj_t   *s_plot = NULL;     /* the image showing s_fb */
static lv_obj_t   *s_empty = NULL;
static lv_obj_t   *s_legend = NULL;
static lv_obj_t   *s_pause_btn = NULL;
static lv_obj_t   *s_win_btn[3];
static lv_obj_t   *s_picker = NULL;
static lv_timer_t *s_timer = NULL;

static void _rebuild_legend(void);
static void _open_picker(lv_event_t *e);

/* ── Channels ─────────────────────────────────────────────────────────── */

static bool _available(const channel_t *c)
{
    return c && (c->signal_index >= 0 || c->math_enabled || c->calculate_fn);
}

static bool _id_available(const char *id)
{
    return _available(channel_manager_get(id));
}

static const char *_unit(const channel_t *c)
{
    /* λ draws now: the kit's faces fall back to a DejaVu symbol subset. */
    return c->units_display[0] ? c->units_display : c->units_native;
}

static float _read(const char *id)
{
    channel_t *c = channel_manager_get(id);
    if (!c || c->is_stale || c->last_update_ms == 0) return NAN;
    float v = c->current_value;
    if (c->units_display[0] && c->units_native[0] &&
        strcmp(c->units_display, c->units_native) != 0)
        v = unit_convert(v, c->units_native, c->units_display);
    return v;
}

static float _sample(const g_series_t *s, int ago)
{
    if (!s->buf || ago >= s_count) return NAN;
    return s->buf[(s_head + G_CAP - 1 - ago) % G_CAP];
}

static void _pick_defaults(void)
{
    if (s_defaults_done) return;
    s_defaults_done = true;
    for (size_t i = 0; i < sizeof(k_pairs) / sizeof(k_pairs[0]); i++) {
        if (_id_available(k_pairs[i].a) && _id_available(k_pairs[i].b)) {
            snprintf(s_pick[0], 32, "%s", k_pairs[i].a);
            snprintf(s_pick[1], 32, "%s", k_pairs[i].b);
            s_npick = 2;
            return;
        }
    }
    for (size_t i = 0; i < channel_manager_count(); i++) {
        channel_t *c = channel_manager_at(i);
        if (_available(c)) {
            snprintf(s_pick[0], 32, "%s", c->id);
            s_npick = 1;
            return;
        }
    }
}

/* Match series to picks, keeping the history of any channel still picked. */
static void _sync_series(void)
{
    g_series_t old[G_MAX];
    memcpy(old, s_ser, sizeof(old));
    memset(s_ser, 0, sizeof(s_ser));

    for (int i = 0; i < s_npick; i++) {
        snprintf(s_ser[i].id, sizeof(s_ser[i].id), "%s", s_pick[i]);
        for (int j = 0; j < G_MAX; j++) {
            if (old[j].buf && strcmp(old[j].id, s_pick[i]) == 0) {
                s_ser[i].buf = old[j].buf;
                s_ser[i].lo = old[j].lo;
                s_ser[i].hi = old[j].hi;
                s_ser[i].has_range = old[j].has_range;
                old[j].buf = NULL;
                break;
            }
        }
        if (!s_ser[i].buf) {
            s_ser[i].buf = heap_caps_malloc(G_CAP * sizeof(float), MALLOC_CAP_SPIRAM);
            if (s_ser[i].buf)
                for (int k = 0; k < G_CAP; k++) s_ser[i].buf[k] = NAN;
        }
    }
    for (int j = 0; j < G_MAX; j++)
        if (old[j].buf) heap_caps_free(old[j].buf);
}

/* ── Scale ────────────────────────────────────────────────────────────── */

static void _update_ranges(int n)
{
    bool done[G_MAX] = { false };
    for (int i = 0; i < s_npick; i++) {
        if (done[i]) continue;
        channel_t *ci = channel_manager_get(s_ser[i].id);
        const char *unit = ci ? _unit(ci) : "";
        uint8_t dec = ci ? ci->decimals : 1;

        /* Everything in this unit group, over the visible window. */
        float lo = INFINITY, hi = -INFINITY;
        int members[G_MAX], nm = 0;
        for (int j = i; j < s_npick; j++) {
            channel_t *cj = channel_manager_get(s_ser[j].id);
            if (j != i && (!cj || strcmp(_unit(cj), unit) != 0)) continue;
            members[nm++] = j;
            done[j] = true;
            for (int k = 0; k < n; k++) {
                float v = _sample(&s_ser[j], k);
                if (isnan(v)) continue;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
        }
        if (lo > hi) continue;   /* nothing yet: keep whatever it had */

        float min_span = powf(10.0f, -(float)dec) * 4.0f;
        bool non_negative = lo >= 0.0f;
        float span = hi - lo;
        if (span < min_span) {
            float mid = (lo + hi) * 0.5f;
            lo = mid - min_span * 0.5f;
            hi = mid + min_span * 0.5f;
            span = min_span;
        }
        lo -= span * 0.06f;
        hi += span * 0.06f;
        if (non_negative && lo < 0.0f) lo = 0.0f;

        /* Round to four equal steps of 1, 2, 2.5 or 5 x 10^n, so the axis
         * reads 0 / 2000 / 4000 and not 518 / 2443 / 4368. */
        float raw = (hi - lo) / 4.0f;
        float mag = powf(10.0f, floorf(log10f(raw)));
        float step = mag;
        if (raw > mag * 5.0f) step = mag * 10.0f;
        else if (raw > mag * 2.5f) step = mag * 5.0f;
        else if (raw > mag * 2.0f) step = mag * 2.5f;
        else if (raw > mag) step = mag * 2.0f;
        float nlo = floorf(lo / step) * step;
        float nhi = nlo + 4.0f * step;
        for (int guard = 0; nhi < hi && guard < 8; guard++) {
            /* Rounding lo down pushed the top below the data: next step up. */
            float m = powf(10.0f, floorf(log10f(step)));
            float f = step / m;
            step = (f < 1.5f ? 2.0f : f < 2.2f ? 2.5f : f < 3.0f ? 5.0f : 10.0f) * m;
            nlo = floorf(lo / step) * step;
            nhi = nlo + 4.0f * step;
        }

        for (int m = 0; m < nm; m++) {
            g_series_t *s = &s_ser[members[m]];
            bool grow = !s->has_range || nlo < s->lo || nhi > s->hi;
            if (grow) {
                s->lo = nlo; s->hi = nhi; s->has_range = true;
                s->shrink_frames = 0;
            } else if (nlo != s->lo || nhi != s->hi) {
                /* A tighter scale must hold for 3 s before the axis moves in,
                 * so a trace that briefly calms down doesn't make it jump. */
                if (++s->shrink_frames > 30) {
                    s->lo = nlo; s->hi = nhi;
                    s->shrink_frames = 0;
                }
            } else {
                s->shrink_frames = 0;
            }
        }
    }
}

/* ── Drawing ──────────────────────────────────────────────────────────── */
/*
 * The plot is one RGB565 image drawn by hand, not LVGL line primitives: LVGL's
 * anti-aliased lines go through a mask per segment, and a 60 s trace is
 * hundreds of segments — that cost ~60 % CPU at 10 Hz. Here every pixel
 * column of a trace is one vertical span with fractional coverage at its two
 * ends, so each column is written once (joins neither gap nor double up) and
 * a redraw is a few milliseconds.
 *
 * The buffer lives in PSRAM, where writing every pixel is slow (clearing the
 * whole plot took ~77 ms). So it is cleared once, and each frame erases only
 * the columns and rows the traces touched last time, putting the grid back
 * under them.
 */

#define PLOT_W  (GRAPH_CARD_W - 2)
#define PLOT_H  (GRAPH_CARD_H - 2)

static uint16_t     *s_fb = NULL;
static EXT_RAM_BSS_ATTR lv_img_dsc_t s_fb_dsc;
static EXT_RAM_BSS_ATTR lv_obj_t *s_ylabels[5];
static EXT_RAM_BSS_ATTR lv_obj_t *s_tlabels[6];

static EXT_RAM_BSS_ATTR int16_t s_dirty_top[PLOT_W];   /* rows written per column, last frame */
static EXT_RAM_BSS_ATTR int16_t s_dirty_bot[PLOT_W];   /* top > bot = clean */
static bool    s_fb_clean = false;    /* false = clear it all next frame */

/* What a frame changed, per vertical strip of the plot. Only these bands are
 * invalidated: copying the whole 528 x 402 plot through PSRAM every frame was
 * most of the graph's CPU, and a trace usually spans a small part of each
 * strip's height. 12 strips stays well inside LVGL's 32 dirty areas. */
#define STRIPS   12
#define STRIP_W  ((PLOT_W + STRIPS - 1) / STRIPS)
static EXT_RAM_BSS_ATTR int16_t s_strip_top[STRIPS];
static EXT_RAM_BSS_ATTR int16_t s_strip_bot[STRIPS];
static bool    s_full_invalidate = true;

static inline void _strip_mark(int x, int top, int bot)
{
    int k = x / STRIP_W;
    if (k < 0 || k >= STRIPS) return;
    if (top < s_strip_top[k]) s_strip_top[k] = (int16_t)top;
    if (bot > s_strip_bot[k]) s_strip_bot[k] = (int16_t)bot;
}

typedef struct { float x, y; bool brk; } g_pt_t;
static g_pt_t *s_pts = NULL;           /* scratch: one trace's points */
#define PTS_MAX (PLOT_W * 2 + 8)

static inline uint16_t _mix(uint16_t bg, uint16_t fg, uint32_t a)
{
    if (a >= 255) return fg;
    uint32_t r = ((fg >> 11) * a + (bg >> 11) * (255 - a) + 127) / 255;
    uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * (255 - a) + 127) / 255;
    uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * (255 - a) + 127) / 255;
    return (uint16_t)(r << 11 | g << 5 | b);
}

/* One column's span [top, bot] (pixels, fractional) at coverage @p cov. */
static void _span(int x, float top, float bot, uint16_t color, float cov)
{
    if (x < 0 || x >= PLOT_W) return;
    if (top < 0) top = 0;
    if (bot > PLOT_H) bot = PLOT_H;
    if (bot <= top) return;
    int r0 = (int)top, r1 = (int)ceilf(bot);
    if (r0 < s_dirty_top[x]) s_dirty_top[x] = (int16_t)r0;
    if (r1 - 1 > s_dirty_bot[x]) s_dirty_bot[x] = (int16_t)(r1 - 1);
    for (int r = r0; r < r1 && r < PLOT_H; r++) {
        float o = (bot < r + 1 ? bot : r + 1) - (top > r ? top : r);
        if (o <= 0) continue;
        uint32_t a = (uint32_t)(o * cov * 255.0f + 0.5f);
        uint16_t *p = &s_fb[r * PLOT_W + x];
        *p = _mix(*p, color, a);
    }
}

/* A polyline of points (x in pixels, y in pixels), thickness 2.2 px. Each
 * integer column takes the line's y extent across that column. */
static void _trace(const g_pt_t *pts, int n, uint16_t color, bool dashed)
{
    const float HW = 1.1f;
    for (int i = 1; i < n; i++) {
        const g_pt_t *a = &pts[i - 1], *b = &pts[i];
        if (b->brk) continue;
        float x0 = a->x, x1 = b->x;
        if (x1 - x0 < 0.001f) {             /* same column: a vertical stroke */
            int c = (int)x0;
            if (dashed && ((c / 9) % 2)) continue;
            float lo = a->y < b->y ? a->y : b->y, hi = a->y < b->y ? b->y : a->y;
            _span(c, lo - HW, hi + HW, color, 1.0f);
            if (hi - lo > 1.5f) {
                _span(c - 1, lo, hi, color, 0.45f);
                _span(c + 1, lo, hi, color, 0.45f);
            }
            continue;
        }
        int c0 = (int)floorf(x0), c1 = (int)ceilf(x1);
        for (int c = c0; c < c1; c++) {
            if (dashed && ((c / 9) % 2)) continue;
            float cx0 = c > x0 ? (float)c : x0;
            float cx1 = (c + 1) < x1 ? (float)(c + 1) : x1;
            float w = cx1 - cx0;                  /* how much of the column */
            if (w <= 0) continue;
            float ya = a->y + (b->y - a->y) * (cx0 - x0) / (x1 - x0);
            float yb = a->y + (b->y - a->y) * (cx1 - x0) / (x1 - x0);
            float lo = ya < yb ? ya : yb, hi = ya < yb ? yb : ya;
            _span(c, lo - HW, hi + HW, color, w);
            /* A column only widens a line vertically; a steep stroke would
             * be 1 px wide next to 2 px flat ones. Lend it to both sides. */
            float m = fabsf((b->y - a->y) / (x1 - x0));
            if (m > 1.0f) {
                float side = (m > 3.0f ? 0.55f : 0.55f * (m - 1.0f) / 2.0f) * w;
                _span(c - 1, lo, hi, color, side);
                _span(c + 1, lo, hi, color, side);
            }
        }
    }
}

static void _dot(float cx, float cy, float rad, uint16_t color)
{
    for (int y = (int)(cy - rad - 1); y <= (int)(cy + rad + 1); y++) {
        if (y < 0 || y >= PLOT_H) continue;
        for (int x = (int)(cx - rad - 1); x <= (int)(cx + rad + 1); x++) {
            if (x < 0 || x >= PLOT_W) continue;
            float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
            float a = rad + 0.5f - d;
            if (a <= 0) continue;
            if (y < s_dirty_top[x]) s_dirty_top[x] = (int16_t)y;
            if (y > s_dirty_bot[x]) s_dirty_bot[x] = (int16_t)y;
            s_fb[y * PLOT_W + x] = _mix(s_fb[y * PLOT_W + x], color, (uint32_t)((a > 1 ? 1 : a) * 255));
        }
    }
}

static float _y_px(const g_series_t *s, float v, float y0, float h)
{
    float t = (v - s->lo) / (s->hi - s->lo);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return y0 + h - t * h;
}

static void _set_labels(void)
{
    for (int k = 0; k <= 5; k++) {
        if (!s_tlabels[k]) continue;
        int secs = s_win - s_win * k / 5;
        if (secs == 0) lv_label_set_text(s_tlabels[k], "now");
        else lv_label_set_text_fmt(s_tlabels[k], "-%ds", secs);
    }

    int axis = -1;
    bool one_scale = true;
    for (int i = 0; i < s_npick; i++) {
        if (!s_ser[i].has_range) continue;
        if (axis < 0) axis = i;
        else if (s_ser[i].lo != s_ser[axis].lo || s_ser[i].hi != s_ser[axis].hi) one_scale = false;
    }
    for (int k = 0; k <= 4; k++) {
        if (!s_ylabels[k]) continue;
        if (axis < 0) { lv_label_set_text(s_ylabels[k], ""); continue; }
        channel_t *c = channel_manager_get(s_ser[axis].id);
        float v = s_ser[axis].hi - (s_ser[axis].hi - s_ser[axis].lo) * k / 4;
        char t[16];
        snprintf(t, sizeof(t), "%.*f", c ? c->decimals : 1, (double)v);
        if (strcmp(lv_label_get_text(s_ylabels[k]), t) != 0) lv_label_set_text(s_ylabels[k], t);
        /* In the trace's colour when two scales share the plot, so it is
         * clear whose numbers they are. */
        lv_obj_set_style_text_color(s_ylabels[k],
            one_scale ? ui_pal->text_muted : lv_color_hex(k_colors[axis]), 0);
    }
}

static void _render(void)
{
    if (!s_fb) return;
    const float x0 = G_PAD_L, x1 = PLOT_W - G_PAD_R;
    const float y0 = G_PAD_T, y1 = PLOT_H - G_PAD_B;
    const float w = x1 - x0, h = y1 - y0;

    uint16_t bg = ui_pal->card.full, grid = ui_pal->line.full;
    int gy[5], gx[6];
    for (int k = 0; k <= 4; k++) gy[k] = (int)(y0 + h * k / 4);
    for (int k = 0; k <= 5; k++) gx[k] = (int)(x0 + w * k / 5);

    for (int k = 0; k < STRIPS; k++) { s_strip_top[k] = PLOT_H; s_strip_bot[k] = -1; }

    if (!s_fb_clean) {
        s_full_invalidate = true;
        /* Whole plot, row by row: one prepared row copied down is far
         * cheaper on PSRAM than writing pixels one at a time. */
        uint16_t blank[PLOT_W], row[PLOT_W], line[PLOT_W];
        for (int x = 0; x < PLOT_W; x++) { blank[x] = bg; row[x] = bg; line[x] = bg; }
        for (int k = 0; k <= 5; k++) row[gx[k]] = grid;
        for (int x = (int)x0; x <= (int)x1; x++) line[x] = grid;
        for (int y = 0; y < PLOT_H; y++) {
            const uint16_t *src = blank;
            if (y >= (int)y0 && y <= (int)y1) {
                src = row;
                for (int k = 0; k <= 4; k++) if (y == gy[k]) src = line;
            }
            memcpy(&s_fb[y * PLOT_W], src, sizeof(row));
        }
        for (int x = 0; x < PLOT_W; x++) { s_dirty_top[x] = PLOT_H; s_dirty_bot[x] = -1; }
        s_fb_clean = true;
    } else {
        /* Only what the traces wrote last frame. */
        for (int x = 0; x < PLOT_W; x++) {
            if (s_dirty_top[x] > s_dirty_bot[x]) continue;
            _strip_mark(x, s_dirty_top[x], s_dirty_bot[x]);
            bool vline = false;
            for (int k = 0; k <= 5; k++) if (x == gx[k]) vline = true;
            for (int y = s_dirty_top[x]; y <= s_dirty_bot[x]; y++) {
                bool in_y = y >= (int)y0 && y <= (int)y1;
                bool hline = false;
                for (int k = 0; k <= 4; k++) if (y == gy[k]) hline = true;
                bool in_x = x >= (int)x0 && x <= (int)x1;
                s_fb[y * PLOT_W + x] = ((hline && in_x) || (vline && in_y)) ? grid : bg;
            }
            s_dirty_top[x] = PLOT_H;
            s_dirty_bot[x] = -1;
        }
    }

    int n_win = s_win * G_HZ;
    int n = s_count < n_win ? s_count : n_win;
    _update_ranges(n);
    int cols = (int)w;

    for (int i = 0; i < s_npick; i++) {
        g_series_t *s = &s_ser[i];
        if (!s->buf || !s->has_range || !s_pts) continue;
        int np = 0;
        bool gap = true;

        if (n_win <= cols) {
            for (int j = n_win - n; j < n_win; j++) {
                float v = _sample(s, n_win - 1 - j);
                if (isnan(v)) { gap = true; continue; }
                s_pts[np].x = x0 + w * j / (n_win - 1);
                s_pts[np].y = _y_px(s, v, y0, h);
                s_pts[np].brk = gap;
                gap = false;
                if (np < PTS_MAX - 1) np++;
            }
        } else {
            /* Each column's low and high, in the order they happened, so a
             * spike survives the squeeze into a 60 s window. */
            for (int b = 0; b < cols; b++) {
                int ja = b * n_win / cols, jb = (b + 1) * n_win / cols;
                if (jb <= ja) jb = ja + 1;
                float lo = INFINITY, hi = -INFINITY;
                int jlo = -1, jhi = -1;
                for (int j = ja; j < jb; j++) {
                    if (j < n_win - n) continue;
                    float v = _sample(s, n_win - 1 - j);
                    if (isnan(v)) continue;
                    if (v < lo) { lo = v; jlo = j; }
                    if (v > hi) { hi = v; jhi = j; }
                }
                if (jlo < 0) { gap = true; continue; }
                float x = x0 + b + 0.5f;
                float first = jlo <= jhi ? lo : hi, second = jlo <= jhi ? hi : lo;
                s_pts[np].x = x; s_pts[np].y = _y_px(s, first, y0, h); s_pts[np].brk = gap;
                gap = false;
                if (np < PTS_MAX - 1) np++;
                if (hi > lo) {
                    s_pts[np].x = x; s_pts[np].y = _y_px(s, second, y0, h); s_pts[np].brk = false;
                    if (np < PTS_MAX - 1) np++;
                }
            }
        }

        uint16_t col = lv_color_hex(k_colors[i]).full;
        _trace(s_pts, np, col, strstr(s->id, "target") != NULL);
        if (np > 0 && n > 0 && !isnan(_sample(s, 0)))
            _dot(s_pts[np - 1].x, s_pts[np - 1].y, 4.0f, col);
    }

    /* This frame's writes, for the strips (s_dirty now holds them). */
    for (int x = 0; x < PLOT_W; x++)
        if (s_dirty_top[x] <= s_dirty_bot[x]) _strip_mark(x, s_dirty_top[x], s_dirty_bot[x]);

    lv_img_cache_invalidate_src(&s_fb_dsc);
    if (s_plot) {
        if (s_full_invalidate) {
            lv_obj_invalidate(s_plot);
            s_full_invalidate = false;
        } else {
            lv_area_t c;
            lv_obj_get_coords(s_plot, &c);
            for (int k = 0; k < STRIPS; k++) {
                if (s_strip_top[k] > s_strip_bot[k]) continue;
                lv_area_t a = {
                    c.x1 + k * STRIP_W, c.y1 + s_strip_top[k],
                    c.x1 + (k + 1) * STRIP_W - 1, c.y1 + s_strip_bot[k],
                };
                if (a.x2 > c.x2) a.x2 = c.x2;
                lv_obj_invalidate_area(s_plot, &a);
            }
        }
    }
    _set_labels();
}

/* ── Legend ───────────────────────────────────────────────────────────── */

static void _fmt(const channel_t *c, float v, char *buf, size_t n)
{
    if (isnan(v)) { snprintf(buf, n, "-"); return; }
    snprintf(buf, n, "%.*f", c ? c->decimals : 1, (double)v);
}

static void _refresh_legend_values(void)
{
    int n_win = s_win * G_HZ;
    int n = s_count < n_win ? s_count : n_win;
    for (int i = 0; i < s_npick; i++) {
        g_series_t *s = &s_ser[i];
        if (!s->value_lbl || !lv_obj_is_valid(s->value_lbl)) continue;
        channel_t *c = channel_manager_get(s->id);
        char v[24], lo_s[16], hi_s[16], txt[48];
        _fmt(c, _sample(s, 0), v, sizeof(v));
        lv_label_set_text(s->value_lbl, v);

        float lo = INFINITY, hi = -INFINITY;
        for (int k = 0; k < n; k++) {
            float x = _sample(s, k);
            if (isnan(x)) continue;
            if (x < lo) lo = x;
            if (x > hi) hi = x;
        }
        if (lo > hi) {
            lv_label_set_text(s->range_lbl, c ? "No reading" : "Channel removed");
        } else {
            _fmt(c, lo, lo_s, sizeof(lo_s));
            _fmt(c, hi, hi_s, sizeof(hi_s));
            snprintf(txt, sizeof(txt), "Min %s   Max %s", lo_s, hi_s);
            lv_label_set_text(s->range_lbl, txt);
        }
    }
}

static void _rebuild_legend_async(void *arg)
{
    (void)arg;
    _rebuild_legend();
}

static void _remove_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_npick) return;
    for (int i = idx; i < s_npick - 1; i++) memcpy(s_pick[i], s_pick[i + 1], 32);
    s_npick--;
    _sync_series();
    /* The tapped row is inside the legend being rebuilt. */
    lv_async_call(_rebuild_legend_async, NULL);
}

static void _peaks_cb(lv_event_t *e)
{
    (void)e;
    peaks_ui_show();
}

static void _rebuild_legend(void)
{
    if (!s_legend || !lv_obj_is_valid(s_legend)) return;
    lv_obj_clean(s_legend);
    for (int i = 0; i < G_MAX; i++) { s_ser[i].value_lbl = NULL; s_ser[i].range_lbl = NULL; }

    for (int i = 0; i < s_npick; i++) {
        channel_t *c = channel_manager_get(s_ser[i].id);

        lv_obj_t *row = uk_card(s_legend);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 2, 0);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_style_pad_left(row, 16, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(row, ui_pal->raised, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, _remove_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *bar = lv_obj_create(row);
        lv_obj_remove_style_all(bar);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(bar, 4, lv_pct(100));
        lv_obj_align(bar, LV_ALIGN_LEFT_MID, -10, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(k_colors[i]), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 2, 0);

        lv_obj_t *name = uk_label(row, c ? c->label : s_ser[i].id, UK_FONT_LABEL, UK_TONE_MUTED);
        lv_obj_set_width(name, lv_pct(100));
        lv_obj_set_style_pad_right(name, 22, 0);   /* clear of the X */
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

        lv_obj_t *x = uk_icon(row, UK_ICON_CLOSE, UK_ICON_MD, UK_TONE_HINT);
        lv_obj_add_flag(x, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 4, -4);

        lv_obj_t *line = lv_obj_create(row);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(line, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_column(line, 6, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s_ser[i].value_lbl = uk_label(line, "-", UK_FONT_TITLE, UK_TONE_TEXT);
        lv_obj_set_style_text_color(s_ser[i].value_lbl, lv_color_hex(k_colors[i]), 0);
        if (c && _unit(c)[0]) {
            lv_obj_t *u = uk_label(line, _unit(c), UK_FONT_LABEL, UK_TONE_MUTED);
            lv_obj_set_style_pad_bottom(u, 3, 0);
        }

        s_ser[i].range_lbl = uk_label(row, "", UK_FONT_SMALL, UK_TONE_HINT);
    }

    if (s_npick < G_MAX) {
        lv_obj_t *add = uk_btn(s_legend, UK_ICON_PLUS, "Add a channel", UK_BTN_NEUTRAL, _open_picker, NULL);
        lv_obj_set_width(add, lv_pct(100));
    }

    lv_obj_t *spacer = lv_obj_create(s_legend);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_width(spacer, 1);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t *pk = uk_btn(s_legend, UK_ICON_LIST, "Min / max table", UK_BTN_GHOST, _peaks_cb, NULL);
    lv_obj_set_width(pk, lv_pct(100));

    _refresh_legend_values();
    if (s_empty) {
        if (s_npick) lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    }
    _render();
}

/* ── Picker ───────────────────────────────────────────────────────────── */

static void _picker_close(lv_event_t *e)
{
    (void)e;
    if (s_picker && lv_obj_is_valid(s_picker)) lv_obj_del(s_picker);
    s_picker = NULL;
}

static bool _picked(const char *id)
{
    for (int i = 0; i < s_npick; i++) if (strcmp(s_pick[i], id) == 0) return true;
    return false;
}

static void _apply_async(void *arg)
{
    (void)arg;
    _picker_close(NULL);
    _sync_series();
    _rebuild_legend();
}

static void _pair_cb(lv_event_t *e)
{
    int p = (int)(intptr_t)lv_event_get_user_data(e);
    snprintf(s_pick[0], 32, "%s", k_pairs[p].a);
    snprintf(s_pick[1], 32, "%s", k_pairs[p].b);
    s_npick = 2;
    lv_async_call(_apply_async, NULL);
}

static void _channel_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    channel_t *c = channel_manager_at((size_t)idx);
    if (!c || s_npick >= G_MAX || _picked(c->id)) return;
    snprintf(s_pick[s_npick++], 32, "%s", c->id);
    lv_async_call(_apply_async, NULL);
}

static void _open_picker(lv_event_t *e)
{
    (void)e;
    if (s_picker && lv_obj_is_valid(s_picker)) return;
    s_picker = uk_popup(700, 440, "Add a channel", _picker_close);

    lv_obj_t *col = lv_obj_create(s_picker);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, lv_pct(100), 440 - 36 - UK_POPUP_BODY_Y);
    lv_obj_align(col, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    /* Pairs that belong on one graph, offered only when this car has both. */
    lv_obj_t *pairs = lv_obj_create(col);
    lv_obj_remove_style_all(pairs);
    lv_obj_set_size(pairs, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pairs, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(pairs, 8, 0);
    lv_obj_set_style_pad_column(pairs, 8, 0);
    lv_obj_clear_flag(pairs, LV_OBJ_FLAG_SCROLLABLE);
    int npairs = 0;
    for (size_t i = 0; i < sizeof(k_pairs) / sizeof(k_pairs[0]); i++) {
        if (!_id_available(k_pairs[i].a) || !_id_available(k_pairs[i].b)) continue;
        if (npairs == 0) lv_obj_move_to_index(uk_section(col, "Compare, for tuning"), 0);
        uk_btn(pairs, UK_ICON_CHART, k_pairs[i].name, UK_BTN_NEUTRAL, _pair_cb, (void *)(intptr_t)i);
        npairs++;
    }
    if (npairs == 0) lv_obj_add_flag(pairs, LV_OBJ_FLAG_HIDDEN);

    char head[48];
    if (s_npick >= G_MAX) snprintf(head, sizeof(head), "Channels - %d at most, remove one first", G_MAX);
    else snprintf(head, sizeof(head), "Channels");
    uk_section(col, head);

    lv_obj_t *list = uk_scroll(col);
    lv_obj_set_height(list, 0);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(list, 6, 0);

    for (size_t i = 0; i < channel_manager_count(); i++) {
        channel_t *c = channel_manager_at(i);
        if (!_available(c)) continue;
        bool on = _picked(c->id);
        lv_obj_t *b = uk_btn(list, on ? UK_ICON_CHECK : UK_ICON_NONE, c->label,
                             on ? UK_BTN_ON : UK_BTN_NEUTRAL, _channel_cb, (void *)(intptr_t)i);
        lv_obj_set_width(b, 205);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(b, 12, 0);
        lv_obj_t *l = uk_btn_label(b);
        lv_obj_set_flex_grow(l, 1);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        if (on || s_npick >= G_MAX) lv_obj_add_state(b, LV_STATE_DISABLED);
    }
}

/* ── Timer, bar ───────────────────────────────────────────────────────── */

/* Sampling runs by the clock, not by timer calls: LVGL skips timer runs it
 * missed instead of catching up, so a long frame would otherwise drop samples
 * and stretch the time axis. Every 50 ms that passed gets its sample (a
 * missed one repeats the value read now).
 *
 * Redraws are paced by the window: a pixel is 0.4 samples at 10 s but 2.5 at
 * 60 s, so the wide windows redraw less often without moving any less
 * smoothly. */
static uint32_t s_next_sample_ms = 0;
static uint32_t s_last_render_ms = 0;
static uint32_t s_last_legend_ms = 0;

static void _timer_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    const uint32_t period = 1000 / G_HZ;
    if (s_paused) {
        s_next_sample_ms = now;
    } else {
        if (s_next_sample_ms == 0 || now - s_next_sample_ms > 1000) s_next_sample_ms = now;
        float v[G_MAX];
        bool read = false;
        while ((int32_t)(now - s_next_sample_ms) >= 0) {
            if (!read) {
                for (int i = 0; i < s_npick; i++) v[i] = _read(s_ser[i].id);
                read = true;
            }
            for (int i = 0; i < s_npick; i++)
                if (s_ser[i].buf) s_ser[i].buf[s_head] = v[i];
            s_head = (s_head + 1) % G_CAP;
            if (s_count < G_CAP) s_count++;
            s_next_sample_ms += period;
        }
        uint32_t render_every = s_win <= 10 ? 100 : s_win <= 30 ? 200 : 250;
        if (now - s_last_render_ms >= render_every) {
            s_last_render_ms = now;
            _render();
        }
    }
    if (now - s_last_legend_ms >= 250) {
        s_last_legend_ms = now;
        _refresh_legend_values();
    }
}

static void _win_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    s_win = k_windows[k];
    for (int i = 0; i < 3; i++) uk_btn_set_kind(s_win_btn[i], i == k ? UK_BTN_ON : UK_BTN_NEUTRAL);
    for (int i = 0; i < s_npick; i++) s_ser[i].has_range = false;
    _refresh_legend_values();
    _render();
}

static void _pause_cb(lv_event_t *e)
{
    (void)e;
    s_paused = !s_paused;
    uk_btn_set_text(s_pause_btn, s_paused ? "Resume" : "Pause");
    uk_btn_set_kind(s_pause_btn, s_paused ? UK_BTN_ON : UK_BTN_NEUTRAL);
    _render();
}

static lv_obj_t *_bar_btn(lv_obj_t *bar, uk_icon_t icon, const char *text,
                          uk_btn_kind_t kind, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = uk_btn(lv_obj_get_child(bar, 2), icon, text, kind, cb, ud);
    lv_obj_set_height(b, 36);
    return b;
}

static void _screen_deleted_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) != s_scr) return;
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    _picker_close(NULL);
    for (int i = 0; i < G_MAX; i++) {
        if (s_ser[i].buf) heap_caps_free(s_ser[i].buf);
    }
    memset(s_ser, 0, sizeof(s_ser));
    if (s_fb) { heap_caps_free(s_fb); s_fb = NULL; }
    s_fb_clean = false;
    s_full_invalidate = true;
    if (s_pts) { heap_caps_free(s_pts); s_pts = NULL; }
    memset(s_ylabels, 0, sizeof(s_ylabels));
    memset(s_tlabels, 0, sizeof(s_tlabels));
    s_scr = s_plot = s_legend = s_pause_btn = s_empty = NULL;
    s_count = 0;
    s_head = 0;
    s_paused = false;
}

void graphs_ui_show(void)
{
    if (s_scr) return;
    uk_init();
    _pick_defaults();

    s_scr = uk_screen();
    lv_obj_add_event_cb(s_scr, _screen_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_t *bar = uk_bar(s_scr, "Live graphs", UK_BAR_BACK, main_menu_back_cb, NULL);
    lv_obj_set_style_pad_column(lv_obj_get_child(bar, 2), 8, 0);
    for (int i = 0; i < 3; i++) {
        char t[8];
        snprintf(t, sizeof(t), "%d s", k_windows[i]);
        s_win_btn[i] = _bar_btn(bar, UK_ICON_NONE, t, k_windows[i] == s_win ? UK_BTN_ON : UK_BTN_NEUTRAL,
                                _win_cb, (void *)(intptr_t)i);
        lv_obj_set_width(s_win_btn[i], 60);
    }
    s_pause_btn = _bar_btn(bar, UK_ICON_NONE, "Pause", UK_BTN_NEUTRAL, _pause_cb, NULL);
    lv_obj_set_width(s_pause_btn, 96);
    lv_obj_set_style_pad_left(s_pause_btn, 0, 0);

    lv_obj_t *body = uk_body(s_scr);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, UK_GAP, 0);

    lv_obj_t *card = uk_card(body);
    lv_obj_set_size(card, GRAPH_CARD_W, GRAPH_CARD_H);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_fb = heap_caps_malloc((size_t)PLOT_W * PLOT_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_pts = heap_caps_malloc(PTS_MAX * sizeof(g_pt_t), MALLOC_CAP_SPIRAM);
    memset(&s_fb_dsc, 0, sizeof(s_fb_dsc));
    s_fb_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_fb_dsc.header.w = PLOT_W;
    s_fb_dsc.header.h = PLOT_H;
    s_fb_dsc.data_size = (uint32_t)PLOT_W * PLOT_H * 2;
    s_fb_dsc.data = (const uint8_t *)s_fb;
    s_plot = lv_img_create(card);
    if (s_fb) lv_img_set_src(s_plot, &s_fb_dsc);
    lv_obj_set_pos(s_plot, 0, 0);

    /* Axis numbers and time marks sit over the image's margins. */
    for (int k = 0; k <= 4; k++) {
        s_ylabels[k] = uk_label(card, "", UK_FONT_SMALL, UK_TONE_MUTED);
        lv_obj_set_width(s_ylabels[k], G_PAD_L - 8);
        lv_obj_set_style_text_align(s_ylabels[k], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(s_ylabels[k], 0, G_PAD_T + (PLOT_H - G_PAD_T - G_PAD_B) * k / 4 - 8);
    }
    for (int k = 0; k <= 5; k++) {
        s_tlabels[k] = uk_label(card, "", UK_FONT_SMALL, UK_TONE_HINT);
        lv_obj_set_width(s_tlabels[k], 60);
        lv_obj_set_style_text_align(s_tlabels[k], k == 5 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER, 0);
        lv_coord_t x = G_PAD_L + (PLOT_W - G_PAD_L - G_PAD_R) * k / 5;
        lv_obj_set_pos(s_tlabels[k], k == 5 ? x - 60 : x - 30, PLOT_H - G_PAD_B + 5);
    }
    s_empty = uk_label(card, "Add a channel to draw it here.", UK_FONT_BODY, UK_TONE_MUTED);
    lv_obj_center(s_empty);
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);

    s_legend = lv_obj_create(body);
    lv_obj_remove_style_all(s_legend);
    lv_obj_set_size(s_legend, GRAPH_LEGEND_W, lv_pct(100));
    lv_obj_set_flex_flow(s_legend, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_legend, 8, 0);
    lv_obj_clear_flag(s_legend, LV_OBJ_FLAG_SCROLLABLE);

    s_head = 0;
    s_count = 0;
    _sync_series();
    _rebuild_legend();

    s_timer = lv_timer_create(_timer_cb, 1000 / G_HZ, NULL);
    main_menu_swap_to(s_scr);
}
