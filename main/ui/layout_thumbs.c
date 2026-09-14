/*
 * layout_thumbs.c — see layout_thumbs.h (ADR-0076).
 */
#include "layout_thumbs.h"

#include "system/display_capture.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "thumbs";

#define THUMB_DIR     "/lfs/thumbs"
#define MAX_THUMBS    12
#define RECAPTURE_MS  30000    /* a fresh picture at most every 30 s */
#define SETTLE_MS     3000     /* let a new layout draw its first values */
#define DATA_BYTES    (LAYOUT_THUMB_W * LAYOUT_THUMB_H * 2)

typedef struct {
    char          name[32];
    lv_img_dsc_t  dsc;
    uint8_t      *data;
    uint32_t      taken_ms;
    bool          need_save;
    bool          used;
} thumb_t;

static thumb_t    s_thumbs[MAX_THUMBS];
static uint32_t   s_loaded_ms = 0;
static lv_timer_t *s_save_timer = NULL;

static thumb_t *_find(const char *name)
{
    for (int i = 0; i < MAX_THUMBS; i++)
        if (s_thumbs[i].used && strcmp(s_thumbs[i].name, name) == 0) return &s_thumbs[i];
    return NULL;
}

/* Find, or take a free slot, or evict the least recently captured one. */
static thumb_t *_slot(const char *name)
{
    thumb_t *t = _find(name);
    if (t) return t;
    thumb_t *victim = NULL;
    for (int i = 0; i < MAX_THUMBS; i++) {
        if (!s_thumbs[i].used) { victim = &s_thumbs[i]; break; }
        if (!victim || s_thumbs[i].taken_ms < victim->taken_ms) victim = &s_thumbs[i];
    }
    if (victim->used && victim->data) {
        lv_img_cache_invalidate_src(&victim->dsc);
        heap_caps_free(victim->data);
    }
    memset(victim, 0, sizeof(*victim));
    snprintf(victim->name, sizeof(victim->name), "%s", name);
    victim->used = true;
    return victim;
}

static bool _ensure_data(thumb_t *t)
{
    if (t->data) return true;
    t->data = heap_caps_malloc(DATA_BYTES, MALLOC_CAP_SPIRAM);
    if (!t->data) return false;
    t->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    t->dsc.header.always_zero = 0;
    t->dsc.header.w = LAYOUT_THUMB_W;
    t->dsc.header.h = LAYOUT_THUMB_H;
    t->dsc.data_size = DATA_BYTES;
    t->dsc.data = t->data;
    return true;
}

static void _path(const char *name, char *buf, size_t n)
{
    snprintf(buf, n, THUMB_DIR "/%s.thm", name);
}

/* ── Flash ────────────────────────────────────────────────────────────── */

typedef struct { char magic[4]; uint16_t w, h; } thm_header_t;

static void _save(thumb_t *t)
{
    struct stat st;
    if (stat(THUMB_DIR, &st) != 0) mkdir(THUMB_DIR, 0755);
    char path[80];
    _path(t->name, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "cannot write %s", path); return; }
    thm_header_t h = { {'R', 'D', 'M', 'T'}, LAYOUT_THUMB_W, LAYOUT_THUMB_H };
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(t->data, 1, DATA_BYTES, f) == DATA_BYTES;
    fclose(f);
    if (!ok) unlink(path);
    else ESP_LOGI(TAG, "saved %s", path);
}

/* Writing ~60 KB to LittleFS takes a moment; do it a little after the tap,
 * once the dock has faded in, not in the middle of the tap itself. */
static void _save_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_save_timer = NULL;
    for (int i = 0; i < MAX_THUMBS; i++)
        if (s_thumbs[i].used && s_thumbs[i].need_save && s_thumbs[i].data && s_thumbs[i].taken_ms) {
            s_thumbs[i].need_save = false;
            _save(&s_thumbs[i]);
        }
}

static bool _load(thumb_t *t)
{
    char path[80];
    _path(t->name, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    thm_header_t h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && memcmp(h.magic, "RDMT", 4) == 0 &&
              h.w == LAYOUT_THUMB_W && h.h == LAYOUT_THUMB_H && _ensure_data(t) &&
              fread(t->data, 1, DATA_BYTES, f) == DATA_BYTES;
    fclose(f);
    return ok;
}

/* ── Public ───────────────────────────────────────────────────────────── */

void layout_thumbs_mark_loaded(const char *name)
{
    if (!name || !name[0]) return;
    s_loaded_ms = lv_tick_get();
    thumb_t *t = _slot(name);
    t->need_save = true;
    t->taken_ms = 0;        /* due at the first chance */
}

void layout_thumbs_capture_if_due(const char *name)
{
    if (!name || !name[0] || !display_capture_shadow_ready()) return;
    uint32_t now = lv_tick_get();
    if (now - s_loaded_ms < SETTLE_MS) return;
    thumb_t *t = _slot(name);
    if (t->taken_ms && now - t->taken_ms < RECAPTURE_MS) return;
    const uint16_t *fb = display_capture_shadow_fb();
    if (!fb || !_ensure_data(t)) return;

    /* Box filter: every panel pixel lands in exactly one thumbnail pixel. */
    const int SW = CAPTURE_WIDTH, SH = CAPTURE_HEIGHT;
    uint16_t *out = (uint16_t *)t->data;
    for (int oy = 0; oy < LAYOUT_THUMB_H; oy++) {
        int sy0 = oy * SH / LAYOUT_THUMB_H, sy1 = (oy + 1) * SH / LAYOUT_THUMB_H;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int ox = 0; ox < LAYOUT_THUMB_W; ox++) {
            int sx0 = ox * SW / LAYOUT_THUMB_W, sx1 = (ox + 1) * SW / LAYOUT_THUMB_W;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const uint16_t *row = fb + sy * SW;
                for (int sx = sx0; sx < sx1; sx++) {
                    uint16_t p = row[sx];
                    r += p >> 11; g += (p >> 5) & 0x3F; b += p & 0x1F;
                    n++;
                }
            }
            out[oy * LAYOUT_THUMB_W + ox] =
                (uint16_t)(((r + n / 2) / n) << 11 | ((g + n / 2) / n) << 5 | ((b + n / 2) / n));
        }
    }
    t->taken_ms = now ? now : 1;
    lv_img_cache_invalidate_src(&t->dsc);

    if (t->need_save && !s_save_timer) {
        s_save_timer = lv_timer_create(_save_timer_cb, 1500, NULL);
        lv_timer_set_repeat_count(s_save_timer, 1);
    }
}

const lv_img_dsc_t *layout_thumbs_get(const char *name)
{
    if (!name || !name[0]) return NULL;
    thumb_t *t = _find(name);
    if (t && t->data && (t->taken_ms || !t->need_save)) return &t->dsc;
    /* Not captured this boot: the last one saved, if any. A slot marked
     * loaded but not yet captured still shows the saved picture meanwhile. */
    bool fresh_slot = (t == NULL);
    if (!t) t = _slot(name);
    if (t->data || _load(t)) return &t->dsc;
    if (fresh_slot) { t->used = false; }
    return NULL;
}

void layout_thumbs_forget(const char *name)
{
    if (!name || !name[0]) return;
    thumb_t *t = _find(name);
    if (t) {
        if (t->data) {
            lv_img_cache_invalidate_src(&t->dsc);
            heap_caps_free(t->data);
        }
        memset(t, 0, sizeof(*t));
    }
    char path[80];
    _path(name, path, sizeof(path));
    unlink(path);
}
