/*
 * widget_track_map.c — the circuit outline, and the car's place on it.
 *
 * Deliberately NOT modelled on widget_pathbar's band renderer. Pathbar draws a
 * thick mitered ribbon out of quads, and its own source carries the reason that
 * is dangerous here: lv_draw_polygon's scanline edge-walk never terminates on a
 * self-intersecting polygon, hanging the LVGL task until the watchdog reboots
 * the dash. A tacho bar is authored by hand and rarely crosses itself. A racing
 * circuit crosses itself constantly — every hairpin projected into a few
 * hundred pixels produces near-degenerate quads, and a bridge-over-straight
 * layout (Bathurst, Suzuka) crosses for real.
 *
 * So the outline is drawn with lv_draw_line, one segment at a time, with round
 * joins. No polygons, no miters, nothing that can be made non-convex by an
 * unlucky projection. It costs more draw calls and buys immunity from a class
 * of bug that presents as "the dash rebooted mid-session".
 */
#include "widgets/widget_track_map.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "cJSON.h"
#include "lvgl.h"

#include "widgets/track_map_geo.h"
#include "widgets/widget_rules.h"
#include "data/channel_manager.h"

static const char *TAG = "track_map";

#define LFS_TRACK_DIR   TRACK_MAP_LFS_DIR
#define TM_NAME_LEN     32
#define TM_CHAN_LEN     32

#define DEF_W           320
#define DEF_H           240
#define DEF_LINE_COLOR  0x5A6472   /* cool grey — the tarmac, not the subject  */
#define DEF_DOT_COLOR   0xFF453A   /* the car. The only red on the widget.     */
#define DEF_SF_COLOR    0xF5F7FA
#define DEF_NAME_COLOR  0x8A93A0

typedef struct {
    char      asset[TM_NAME_LEN];      /* the .rdmtrk to load (no extension) */

    /* Geometry, as loaded. Raw file bytes are kept because track_map_file_t
     * borrows into them — freeing the buffer would dangle f.points. */
    uint8_t          *blob;
    size_t            blob_len;
    track_map_file_t  file;
    bool              loaded;

    /* Projected pixels, rebuilt on load and on every resize. */
    lv_point_t *pts;
    uint16_t    n_pts;
    track_map_proj_t proj;

    /* Appearance */
    lv_color_t line_color, dot_color, sf_color, name_color;
    uint8_t    line_width, dot_radius;
    bool       show_name, show_start_finish, show_dot;
    int16_t    rotation;               /* degrees, clockwise on screen */

    /* Live position (Phase 3). Channel-owned, per ADR-0005 — the widget does
     * not decode CAN, it reads the same named channels everything else does. */
    char      lat_chan[TM_CHAN_LEN], lon_chan[TM_CHAN_LEN];
    channel_t *ch_lat, *ch_lon;
    bool      have_fix;                /* a position has arrived at least once */
    double    car_lat, car_lon;
} track_map_data_t;

/* ── helpers ─────────────────────────────────────────────────────────────── */

static lv_color_t _u32_to_color(uint32_t v) { return lv_color_hex(v); }

static void _tm_free_geo(track_map_data_t *td) {
    if (!td) return;
    if (td->pts)  { free(td->pts);  td->pts = NULL; }
    if (td->blob) { free(td->blob); td->blob = NULL; }
    td->blob_len = 0;
    td->n_pts = 0;
    td->loaded = false;
    memset(&td->file, 0, sizeof(td->file));
}

/* Project every geographic point into widget-local pixels and drop the ones
 * that land on a pixel we already have.
 *
 * The de-dup is not an optimisation. A 400-point outline squeezed into a
 * 320 px box puts many neighbours on the SAME integer pixel, and a zero-length
 * segment has no direction — the round join degenerates and LVGL draws a blob.
 * Pathbar de-dups its authored JSON for the same reason; here it has to happen
 * AFTER projection, because that is where the collisions are created. */
static void _tm_project(track_map_data_t *td, uint16_t w, uint16_t h) {
    if (!td || !td->loaded) return;
    if (td->pts) { free(td->pts); td->pts = NULL; }
    td->n_pts = 0;

    uint16_t n = td->file.n_points;
    if (n < 2) return;

    lv_point_t *out = heap_caps_calloc(n, sizeof(lv_point_t), MALLOC_CAP_SPIRAM);
    if (!out) out = calloc(n, sizeof(lv_point_t));
    if (!out) { ESP_LOGE(TAG, "no memory for %u projected points", n); return; }

    int margin = (int)td->line_width + (int)td->dot_radius + 2;
    track_map_fit(&td->file, (int)w, (int)h, margin, (double)td->rotation, &td->proj);

    uint16_t k = 0;
    for (uint16_t i = 0; i < n; i++) {
        int32_t la, lo;
        if (!track_map_point(&td->file, i, &la, &lo)) break;
        double px, py;
        track_map_project(&td->proj, la, lo, &px, &py);
        lv_coord_t X = (lv_coord_t)lroundf((float)px);
        lv_coord_t Y = (lv_coord_t)lroundf((float)py);
        if (k == 0 || X != out[k - 1].x || Y != out[k - 1].y) {
            out[k].x = X; out[k].y = Y; k++;
        }
    }
    if (k < 2) { free(out); return; }
    td->pts = out;
    td->n_pts = k;
}

/* Read the asset. Kept resident for the widget's life rather than cached by
 * name: widget_image's refcounted cache has no invalidation on re-upload (its
 * own comment says a replaced image is only picked up "on the next layout
 * reload"), and "I pushed a new track and nothing changed" is exactly the bug
 * this feature must not ship with. One track_map per layout makes the cache
 * pointless anyway. */
static void _tm_load(track_map_data_t *td) {
    _tm_free_geo(td);
    if (!td->asset[0]) return;

    char path[128];
    snprintf(path, sizeof(path), LFS_TRACK_DIR "/%s" TRACK_MAP_EXT, td->asset);
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGW(TAG, "no such track asset: %s", path); return; }

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long sz = ftell(f);
    rewind(f);
    long max = (long)TRACK_MAP_HEADER + (long)TRACK_MAP_MAX_POINTS * 8;
    if (sz < (long)TRACK_MAP_HEADER || sz > max) {
        ESP_LOGW(TAG, "%s is %ld bytes, outside [%d, %ld]", path, sz, TRACK_MAP_HEADER, max);
        fclose(f);
        return;
    }

    uint8_t *buf = heap_caps_malloc((size_t)sz, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)sz);
    if (!buf) { fclose(f); ESP_LOGE(TAG, "no memory for %ld bytes", sz); return; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); ESP_LOGW(TAG, "short read on %s", path); return; }

    track_map_err_t err = track_map_parse(buf, got, &td->file);
    if (err != TRACK_MAP_OK) {
        ESP_LOGW(TAG, "%s rejected (err %d)", path, (int)err);
        free(buf);
        memset(&td->file, 0, sizeof(td->file));
        return;
    }
    td->blob = buf;
    td->blob_len = got;
    td->loaded = true;
    ESP_LOGI(TAG, "loaded %s: \"%s\", %u points", path, td->file.name, td->file.n_points);
}

/* ── drawing ─────────────────────────────────────────────────────────────── */

static void _tm_draw_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN_END) return;
    widget_t *w = (widget_t *)lv_event_get_user_data(e);
    if (!w) return;
    track_map_data_t *td = (track_map_data_t *)w->type_data;
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    if (!td || !ctx || !obj) return;

    lv_opa_t master = lv_obj_get_style_opa_recursive(obj, LV_PART_MAIN);
    if (master <= LV_OPA_MIN) return;

    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    const lv_coord_t ox = coords.x1, oy = coords.y1;

    if (!td->loaded || td->n_pts < 2) {
        /* Same convention as a missing image: name what is missing rather than
         * drawing an empty box that looks like a working widget with a track
         * that happens to be invisible. */
        return;
    }

    /* The outline: independent segments with round caps. Round caps ARE the
     * join — abutting segments each contribute a half-disc at the shared point,
     * which is what a round join is, without any polygon maths that could fail
     * to terminate. */
    lv_draw_line_dsc_t ld;
    lv_draw_line_dsc_init(&ld);
    ld.color = td->line_color;
    ld.width = td->line_width;
    ld.opa   = master;
    ld.round_start = ld.round_end = 1;

    for (uint16_t i = 1; i < td->n_pts; i++) {
        lv_point_t a = { (lv_coord_t)(ox + td->pts[i - 1].x), (lv_coord_t)(oy + td->pts[i - 1].y) };
        lv_point_t b = { (lv_coord_t)(ox + td->pts[i].x),     (lv_coord_t)(oy + td->pts[i].y) };
        lv_draw_line(ctx, &ld, &a, &b);
    }
    /* Close the loop only if the file says it is one. A hillclimb joined end to
     * start would draw a straight line across the map that no car ever drove. */
    if ((td->file.flags & TRACK_MAP_FLAG_CLOSED) && td->n_pts > 2) {
        lv_point_t a = { (lv_coord_t)(ox + td->pts[td->n_pts - 1].x),
                         (lv_coord_t)(oy + td->pts[td->n_pts - 1].y) };
        lv_point_t b = { (lv_coord_t)(ox + td->pts[0].x), (lv_coord_t)(oy + td->pts[0].y) };
        lv_draw_line(ctx, &ld, &a, &b);
    }

    /* Start/finish: a tick ACROSS the track at the first point, perpendicular
     * to the direction of travel there. */
    if (td->show_start_finish && td->n_pts >= 2) {
        float dx = (float)(td->pts[1].x - td->pts[0].x);
        float dy = (float)(td->pts[1].y - td->pts[0].y);
        float len = sqrtf(dx * dx + dy * dy);
        if (len > 0.01f) {
            float nx = -dy / len, ny = dx / len;
            float half = (float)td->line_width * 2.0f + 3.0f;
            lv_draw_line_dsc_t sd;
            lv_draw_line_dsc_init(&sd);
            sd.color = td->sf_color;
            sd.width = (lv_coord_t)(td->line_width + 1);
            sd.opa   = master;
            sd.round_start = sd.round_end = 0;
            lv_point_t a = { (lv_coord_t)(ox + td->pts[0].x - nx * half),
                             (lv_coord_t)(oy + td->pts[0].y - ny * half) };
            lv_point_t b = { (lv_coord_t)(ox + td->pts[0].x + nx * half),
                             (lv_coord_t)(oy + td->pts[0].y + ny * half) };
            lv_draw_line(ctx, &sd, &a, &b);
        }
    }

    /* The car. Drawn last so it is never hidden by the track, and only when a
     * position has actually arrived — an unknown position must not default to
     * the origin, which would park the car on the start line forever and look
     * entirely plausible. Staleness dims it rather than hiding it: a frozen
     * marker that is visibly dimmed says "this was where you were", where a
     * vanished one says nothing at all. */
    if (td->show_dot && td->have_fix) {
        bool stale = (td->ch_lat && td->ch_lat->is_stale) ||
                     (td->ch_lon && td->ch_lon->is_stale);
        double px, py;
        track_map_project(&td->proj,
                          (int32_t)lround(td->car_lat * 1e7),
                          (int32_t)lround(td->car_lon * 1e7), &px, &py);
        /* Clamp into the widget: a fix from a different circuit projects to a
         * pixel far outside, and LVGL would happily draw it over whatever the
         * neighbouring widget is. */
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        if (px > w->w) px = w->w;
        if (py > w->h) py = w->h;

        lv_draw_rect_dsc_t dd;
        lv_draw_rect_dsc_init(&dd);
        dd.bg_color = td->dot_color;
        dd.bg_opa   = stale ? (lv_opa_t)(master / 3) : master;
        dd.radius   = LV_RADIUS_CIRCLE;
        dd.border_width = 0;
        lv_coord_t r = (lv_coord_t)td->dot_radius;
        lv_area_t a = {
            .x1 = (lv_coord_t)(ox + px - r), .y1 = (lv_coord_t)(oy + py - r),
            .x2 = (lv_coord_t)(ox + px + r), .y2 = (lv_coord_t)(oy + py + r)
        };
        lv_draw_rect(ctx, &dd, &a);
    }
}

/* ── live position ───────────────────────────────────────────────────────── */

static void _tm_on_channel(channel_t *c, void *user) {
    (void)c;
    widget_t *w = (widget_t *)user;
    if (!w) return;
    track_map_data_t *td = (track_map_data_t *)w->type_data;
    if (!td || !td->ch_lat || !td->ch_lon) return;

    /* Both halves come from the SAME 8-byte frame, so a callback on either one
     * means both are current. Reading them together avoids drawing a position
     * built from one new coordinate and one old one — which on a fast corner
     * puts the car off the track for a frame. */
    td->car_lat = td->ch_lat->current_value;
    td->car_lon = td->ch_lon->current_value;
    td->have_fix = true;
    if (w->root && lv_obj_is_valid(w->root)) lv_obj_invalidate(w->root);
}

static void _tm_bind_channels(widget_t *w) {
    track_map_data_t *td = (track_map_data_t *)w->type_data;
    if (!td) return;
    if (!td->lat_chan[0]) snprintf(td->lat_chan, sizeof(td->lat_chan), "gps_latitude");
    if (!td->lon_chan[0]) snprintf(td->lon_chan, sizeof(td->lon_chan), "gps_longitude");
    td->ch_lat = channel_manager_get(td->lat_chan);
    td->ch_lon = channel_manager_get(td->lon_chan);
    if (td->ch_lat) channel_manager_subscribe(td->ch_lat, _tm_on_channel, w);
    if (td->ch_lon) channel_manager_subscribe(td->ch_lon, _tm_on_channel, w);
    if (!td->ch_lat || !td->ch_lon)
        ESP_LOGW(TAG, "position channels not found (%s / %s) — the dot stays off",
                 td->lat_chan, td->lon_chan);
}

/* ── vtable ──────────────────────────────────────────────────────────────── */

static void _tm_create(widget_t *w, lv_obj_t *parent) {
    track_map_data_t *td = (track_map_data_t *)w->type_data;
    if (!td) return;

    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w->w, w->h);
    lv_obj_set_align(obj, LV_ALIGN_CENTER);
    lv_obj_set_pos(obj, w->x, w->y);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(obj, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(obj, _tm_draw_cb, LV_EVENT_DRAW_MAIN_END, w);
    w->root = obj;

    _tm_load(td);
    _tm_project(td, w->w, w->h);

    /* The name is a real label rather than drawn text: LVGL owns font fallback
     * and UTF-8 shaping, and the track name is the one string here that can
     * legitimately contain an umlaut. */
    if (td->show_name) {
        const char *nm = td->loaded && td->file.name[0] ? td->file.name
                       : (td->asset[0] ? td->asset : "no track");
        lv_obj_t *lbl = lv_label_create(obj);
        lv_label_set_text(lbl, nm);
        lv_obj_set_align(lbl, LV_ALIGN_BOTTOM_MID);
        lv_obj_set_style_text_color(lbl, td->name_color, LV_PART_MAIN);
    }
    if (!td->loaded) {
        lv_obj_t *lbl = lv_label_create(obj);
        lv_label_set_text(lbl, td->asset[0] ? td->asset : "no track");
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), LV_PART_MAIN);
    }

    _tm_bind_channels(w);
}

static void _tm_resize(widget_t *w, uint16_t nw, uint16_t nh) {
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_set_size(w->root, nw, nh);
    w->w = nw;
    w->h = nh;
    /* Re-project rather than re-scale: storing geography is exactly what makes
     * a resize free here, where a pixel-baked path would have to be
     * re-authored. */
    track_map_data_t *td = (track_map_data_t *)w->type_data;
    if (td) _tm_project(td, nw, nh);
    if (w->root && lv_obj_is_valid(w->root)) lv_obj_invalidate(w->root);
}

static void _tm_open_settings(widget_t *w) { (void)w; }

static void _tm_to_json(widget_t *w, cJSON *out) {
    track_map_data_t *td = (track_map_data_t *)w->type_data;
    if (!td || !out) return;
    widget_base_to_json(w, out);        /* type, id, x, y, w, h, group */
    cJSON *cfg = cJSON_AddObjectToObject(out, "config");
    if (!cfg) return;
    cJSON_AddStringToObject(cfg, "track_asset", td->asset);
    cJSON_AddNumberToObject(cfg, "line_color", lv_color_to32(td->line_color) & 0xFFFFFF);
    cJSON_AddNumberToObject(cfg, "dot_color", lv_color_to32(td->dot_color) & 0xFFFFFF);
    cJSON_AddNumberToObject(cfg, "sf_color", lv_color_to32(td->sf_color) & 0xFFFFFF);
    cJSON_AddNumberToObject(cfg, "name_color", lv_color_to32(td->name_color) & 0xFFFFFF);
    cJSON_AddNumberToObject(cfg, "line_width", td->line_width);
    cJSON_AddNumberToObject(cfg, "dot_radius", td->dot_radius);
    cJSON_AddNumberToObject(cfg, "rotation", td->rotation);
    cJSON_AddBoolToObject(cfg, "show_name", td->show_name);
    cJSON_AddBoolToObject(cfg, "show_start_finish", td->show_start_finish);
    cJSON_AddBoolToObject(cfg, "show_dot", td->show_dot);
    cJSON_AddStringToObject(cfg, "lat_channel", td->lat_chan);
    cJSON_AddStringToObject(cfg, "lon_channel", td->lon_chan);
}

static void _tm_from_json(widget_t *w, cJSON *in) {
    track_map_data_t *td = (track_map_data_t *)w->type_data;
    if (!td || !in) return;
    /* Position and size live at the TOP level, not in config — and this must
     * run before the early return below, or a track_map with no config block
     * silently lands at (0,0) 320x240 no matter where the editor put it.
     * Every other widget calls this first; this one did not, and the dash
     * proved it by drawing the circuit in the top-left corner. */
    widget_base_from_json(w, in);

    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
    if (!cJSON_IsObject(cfg)) return;
    cJSON *it;

    it = cJSON_GetObjectItemCaseSensitive(cfg, "track_asset");
    if (cJSON_IsString(it) && it->valuestring) {
        strncpy(td->asset, it->valuestring, sizeof(td->asset) - 1);
        td->asset[sizeof(td->asset) - 1] = '\0';
    }
    it = cJSON_GetObjectItemCaseSensitive(cfg, "line_color");
    if (cJSON_IsNumber(it)) td->line_color = _u32_to_color((uint32_t)it->valuedouble);
    it = cJSON_GetObjectItemCaseSensitive(cfg, "dot_color");
    if (cJSON_IsNumber(it)) td->dot_color = _u32_to_color((uint32_t)it->valuedouble);
    it = cJSON_GetObjectItemCaseSensitive(cfg, "sf_color");
    if (cJSON_IsNumber(it)) td->sf_color = _u32_to_color((uint32_t)it->valuedouble);
    it = cJSON_GetObjectItemCaseSensitive(cfg, "name_color");
    if (cJSON_IsNumber(it)) td->name_color = _u32_to_color((uint32_t)it->valuedouble);
    it = cJSON_GetObjectItemCaseSensitive(cfg, "line_width");
    if (cJSON_IsNumber(it)) td->line_width = (uint8_t)LV_CLAMP(1, it->valueint, 20);
    it = cJSON_GetObjectItemCaseSensitive(cfg, "dot_radius");
    if (cJSON_IsNumber(it)) td->dot_radius = (uint8_t)LV_CLAMP(2, it->valueint, 24);
    it = cJSON_GetObjectItemCaseSensitive(cfg, "rotation");
    if (cJSON_IsNumber(it)) td->rotation = (int16_t)(((it->valueint % 360) + 360) % 360);
    it = cJSON_GetObjectItemCaseSensitive(cfg, "show_name");
    if (cJSON_IsBool(it)) td->show_name = cJSON_IsTrue(it);
    it = cJSON_GetObjectItemCaseSensitive(cfg, "show_start_finish");
    if (cJSON_IsBool(it)) td->show_start_finish = cJSON_IsTrue(it);
    it = cJSON_GetObjectItemCaseSensitive(cfg, "show_dot");
    if (cJSON_IsBool(it)) td->show_dot = cJSON_IsTrue(it);
    it = cJSON_GetObjectItemCaseSensitive(cfg, "lat_channel");
    if (cJSON_IsString(it) && it->valuestring) {
        strncpy(td->lat_chan, it->valuestring, sizeof(td->lat_chan) - 1);
        td->lat_chan[sizeof(td->lat_chan) - 1] = '\0';
    }
    it = cJSON_GetObjectItemCaseSensitive(cfg, "lon_channel");
    if (cJSON_IsString(it) && it->valuestring) {
        strncpy(td->lon_chan, it->valuestring, sizeof(td->lon_chan) - 1);
        td->lon_chan[sizeof(td->lon_chan) - 1] = '\0';
    }

    /* from_json can run AFTER create (a live layout edit), in which case the
     * asset just named has to be fetched now — otherwise the widget keeps
     * drawing the previous track and the edit appears to have done nothing. */
    if (w->root && lv_obj_is_valid(w->root)) {
        _tm_load(td);
        _tm_project(td, w->w, w->h);
        lv_obj_invalidate(w->root);
    }
}

static void _tm_destroy(widget_t *w) {
    if (!w) return;
    track_map_data_t *td = (track_map_data_t *)w->type_data;
    if (td) {
        if (td->ch_lat) channel_manager_unsubscribe(td->ch_lat, _tm_on_channel, w);
        if (td->ch_lon) channel_manager_unsubscribe(td->ch_lon, _tm_on_channel, w);
        _tm_free_geo(td);
    }
    if (w->root && lv_obj_is_valid(w->root))
        lv_obj_del(w->root);
    w->root = NULL;
    /* Same reason pathbar does this with no apply_overrides: a hand-authored
     * layout can still carry a "rules" array that widget_rules_from_json
     * allocated and subscribed, and without this the subscription dangles into
     * freed memory. */
    widget_rules_free(w);
    if (w->type_data) free(w->type_data);
    free(w);
}

/* ── factory ─────────────────────────────────────────────────────────────── */

widget_t *widget_track_map_create_instance(uint8_t slot) {
    widget_t *w = calloc(1, sizeof(widget_t));
    if (!w) { ESP_LOGE(TAG, "alloc widget_t failed"); return NULL; }

    track_map_data_t *td = heap_caps_calloc(1, sizeof(track_map_data_t), MALLOC_CAP_SPIRAM);
    if (!td) td = calloc(1, sizeof(track_map_data_t));
    if (!td) { free(w); return NULL; }

    td->line_color = _u32_to_color(DEF_LINE_COLOR);
    td->dot_color  = _u32_to_color(DEF_DOT_COLOR);
    td->sf_color   = _u32_to_color(DEF_SF_COLOR);
    td->name_color = _u32_to_color(DEF_NAME_COLOR);
    td->line_width = 3;
    td->dot_radius = 5;
    td->rotation   = 0;
    td->show_name  = true;
    td->show_start_finish = true;
    td->show_dot   = true;
    snprintf(td->lat_chan, sizeof(td->lat_chan), "gps_latitude");
    snprintf(td->lon_chan, sizeof(td->lon_chan), "gps_longitude");

    w->type      = WIDGET_TRACK_MAP;
    w->slot      = slot;
    w->x         = 0;
    w->y         = 0;
    w->w         = DEF_W;
    w->h         = DEF_H;
    w->type_data = td;
    snprintf(w->id, sizeof(w->id), "trackmap_%u", slot);

    w->create        = _tm_create;
    w->resize        = _tm_resize;
    w->open_settings = _tm_open_settings;
    w->to_json       = _tm_to_json;
    w->from_json     = _tm_from_json;
    w->destroy       = _tm_destroy;

    ESP_LOGI(TAG, "Created track_map instance slot=%u", slot);
    return w;
}
