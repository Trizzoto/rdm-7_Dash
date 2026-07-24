/*
 * widget_anim.c — EXPERIMENTAL signal-driven image-sequence widget.
 *
 * Displays one of N pre-loaded RDMIMG frames based on a signal value:
 *   - mode 0 "scrub":  frame index follows the value across [range_min,
 *     range_max] like a needle — frame 0 at range_min, last frame at
 *     range_max. No timer; redraws only happen on an actual frame change.
 *   - mode 1 "trigger": frame 0 (idle) below threshold; at/above threshold
 *     all frames loop at loop_fps until the value drops back out.
 *
 * All frames are loaded into PSRAM at create() via the shared refcounted
 * rdm_image_load() cache and stay resident, so a frame change is just
 * lv_img_set_src() between resident descriptors — one invalidate, no
 * decode, no allocation. This is deliberately NOT lv_gif: GIF decode is
 * per-frame CPU work and time-driven; this widget is value-driven.
 *
 * The whole module is compiled out by RDM_WIDGET_ANIM_ENABLED=0 (see
 * widget_anim.h and tools/WIDGET_ANIM_README.md).
 */
#include "widget_anim.h"

#if RDM_WIDGET_ANIM_ENABLED

#include "widget_image.h"   /* rdm_image_load / rdm_image_free */
#include "widget_rules.h"
#include "signal.h"
#include "channel_manager.h"
#include "screen_config.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_anim";

#define ANIM_DEFAULT_W          200
#define ANIM_DEFAULT_H          200
#define ANIM_DEFAULT_FRAMES     8
#define ANIM_DEFAULT_RANGE_MAX  8000.0f
#define ANIM_DEFAULT_THRESHOLD  6000.0f
#define ANIM_DEFAULT_FPS        12
#define ANIM_DEFAULT_HYST_PCT   2

/* Fit-contain zoom (LVGL units, 256 = 100%) — same maths as widget_image. */
static uint16_t _anim_fit_zoom(uint16_t cw, uint16_t ch, uint16_t iw, uint16_t ih) {
	if (iw == 0 || ih == 0) return 256;
	uint32_t zx = (uint32_t)cw * 256 / iw;
	uint32_t zy = (uint32_t)ch * 256 / ih;
	uint16_t z  = (uint16_t)(zx < zy ? zx : zy);
	if (z < 1)    z = 1;
	if (z > 4096) z = 4096; /* LVGL v8 maximum zoom */
	return z;
}

/* Swap the visible frame. Descriptors stay resident, so this is a pointer
 * swap + one invalidate of the widget's area. */
static void _anim_set_frame(anim_data_t *ad, uint8_t idx) {
	if (!ad || ad->loaded_count == 0) return;
	if (idx >= ad->loaded_count) idx = ad->loaded_count - 1;
	if (idx == ad->cur_frame) return;
	ad->cur_frame = idx;
	if (ad->img_obj && lv_obj_is_valid(ad->img_obj)) {
		lv_img_set_src(ad->img_obj, ad->frames[idx]);
		/* Re-fit to THIS frame's dimensions. A frame set with mixed sizes would
		 * otherwise inherit frame 0's zoom and render mis-scaled from frame 1 on. */
		lv_obj_t *parent = lv_obj_get_parent(ad->img_obj);
		if (parent) {
			uint16_t zoom = _anim_fit_zoom((uint16_t)lv_obj_get_width(parent),
			                               (uint16_t)lv_obj_get_height(parent),
			                               ad->frames[idx]->header.w,
			                               ad->frames[idx]->header.h);
			lv_img_set_zoom(ad->img_obj, zoom);
		}
	}
}

/* Trigger-mode loop tick — advances one frame per period while active. */
static void _anim_loop_cb(lv_timer_t *t) {
	widget_t *w = (widget_t *)t->user_data;
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	anim_data_t *ad = (anim_data_t *)w->type_data;
	if (!ad || ad->loaded_count == 0 || !ad->trigger_active) return;
	ad->loop_pos = (uint8_t)((ad->loop_pos + 1) % ad->loaded_count);
	_anim_set_frame(ad, ad->loop_pos);
}

static void _anim_on_signal(float value, bool is_stale, void *user_data) {
	widget_t *w = (widget_t *)user_data;
	anim_data_t *ad = (anim_data_t *)w->type_data;
	if (!ad || !w->root || !lv_obj_is_valid(w->root) || ad->loaded_count == 0)
		return;

	if (is_stale) {
		if (ad->trigger_active) {
			ad->trigger_active = false;
			if (ad->loop_timer) lv_timer_pause(ad->loop_timer);
		}
		_anim_set_frame(ad, 0);
		return;
	}

	if (ad->mode == 1) {
		/* Trigger loop with hysteresis: activate at threshold, release a
		 * little below it so a value hovering on the line doesn't strobe
		 * the loop on/off. */
		float hyst = fabsf(ad->threshold) * (float)ad->hyst_pct / 100.0f;
		if (!ad->trigger_active && value >= ad->threshold) {
			ad->trigger_active = true;
			ad->loop_pos = 0;
			_anim_set_frame(ad, 0);
			if (ad->loop_timer) lv_timer_resume(ad->loop_timer);
		} else if (ad->trigger_active && value < ad->threshold - hyst) {
			ad->trigger_active = false;
			if (ad->loop_timer) lv_timer_pause(ad->loop_timer);
			_anim_set_frame(ad, 0);
		}
		return;
	}

	/* Scrub: map value onto continuous frame position 0..N-1, then only
	 * commit a frame change once the position moves more than half a frame
	 * PLUS the hysteresis band past the current frame — kills boundary
	 * chatter when the signal sits right on a frame edge. */
	float range = ad->range_max - ad->range_min;
	if (range <= 0.0f) range = 1.0f;
	float pos = (value - ad->range_min) / range * (float)(ad->loaded_count - 1);
	float max_pos = (float)(ad->loaded_count - 1);
	if (pos <= 0.0f)      { _anim_set_frame(ad, 0); return; }
	if (pos >= max_pos)   { _anim_set_frame(ad, ad->loaded_count - 1); return; }

	float hyst_frames = (float)ad->hyst_pct / 100.0f * max_pos;
	if (fabsf(pos - (float)ad->cur_frame) >= 0.5f + hyst_frames)
		_anim_set_frame(ad, (uint8_t)lroundf(pos));
}

/* ── Frame loading / release ─────────────────────────────────────────────── */

static void _anim_free_frames(anim_data_t *ad) {
	for (int i = 0; i < ANIM_MAX_FRAMES; i++) {
		if (ad->frames[i]) {
			rdm_image_free(ad->frames[i]);
			ad->frames[i] = NULL;
		}
	}
	ad->loaded_count = 0;
}

static void _anim_load_frames(anim_data_t *ad) {
	if (ad->frame_prefix[0] == '\0' || ad->frame_count == 0) return;
	uint8_t want = ad->frame_count;
	if (want > ANIM_MAX_FRAMES) want = ANIM_MAX_FRAMES;
	for (uint8_t i = 0; i < want; i++) {
		char name[40];
		snprintf(name, sizeof(name), "%s_%u", ad->frame_prefix, (unsigned)i);
		lv_img_dsc_t *dsc = rdm_image_load(name);
		if (!dsc) {
			ESP_LOGW(TAG, "Frame %u of '%s' missing — using %u frames",
			         (unsigned)i, ad->frame_prefix, (unsigned)i);
			break;
		}
		ad->frames[ad->loaded_count++] = dsc;
	}
	ESP_LOGI(TAG, "Loaded %u/%u frames for '%s'",
	         (unsigned)ad->loaded_count, (unsigned)want, ad->frame_prefix);
}

/* ── vtable ──────────────────────────────────────────────────────────────── */

static void _anim_create(widget_t *w, lv_obj_t *parent) {
	anim_data_t *ad = (anim_data_t *)w->type_data;
	if (!ad) return;

	/* Pure-decoration container like widget_image: clicks pass through to
	 * whatever the animation overlays (complemented by the is_decoration
	 * gate in dashboard.c). */
	lv_obj_t *cont = lv_obj_create(parent);
	lv_obj_set_size(cont, w->w, w->h);
	lv_obj_set_align(cont, LV_ALIGN_CENTER);
	lv_obj_set_pos(cont, w->x, w->y);
	lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_opa(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

	_anim_load_frames(ad);

	if (ad->loaded_count > 0) {
		ad->img_obj = lv_img_create(cont);
		lv_img_set_src(ad->img_obj, ad->frames[0]);
		lv_obj_set_align(ad->img_obj, LV_ALIGN_CENTER);
		uint16_t zoom = _anim_fit_zoom(w->w, w->h,
		                               ad->frames[0]->header.w,
		                               ad->frames[0]->header.h);
		if (zoom != 256)
			lv_img_set_zoom(ad->img_obj, zoom);
		ad->cur_frame = 0;
	} else if (ad->frame_prefix[0] != '\0') {
		/* Frames named but not on flash — placeholder like widget_image. */
		lv_obj_t *lbl = lv_label_create(cont);
		lv_label_set_text(lbl, ad->frame_prefix);
		lv_obj_set_align(lbl, LV_ALIGN_CENTER);
		lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888),
		                            LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	w->root = cont;

	/* Trigger-mode loop timer: created paused, resumed by the signal
	 * callback. Period from loop_fps (clamped 1..30). */
	if (ad->mode == 1 && ad->loaded_count > 1) {
		uint8_t fps = ad->loop_fps;
		if (fps < 1)  fps = 1;
		if (fps > 30) fps = 30;
		ad->loop_timer = lv_timer_create(_anim_loop_cb, 1000 / fps, w);
		if (ad->loop_timer) {
			lv_timer_set_repeat_count(ad->loop_timer, -1);
			lv_timer_pause(ad->loop_timer);
		}
	}

	/* Subscribe AFTER w->root is set (callbacks bail on invalid root). */
	if (ad->signal_index >= 0) {
		signal_subscribe(ad->signal_index, _anim_on_signal, w);
		/* Prime from the CURRENT value. Subscribers are only notified when a
		 * value arrives/changes, so a signal that is simply sitting still —
		 * a bench/internal signal pinned at a constant, a car idling, or any
		 * gauge between CAN frames — never called us at all. The widget then
		 * sat on frame 0 with its loop timer still paused: the animation
		 * "never played" until something happened to move the signal. Evaluate
		 * once here so the widget is correct from its very first paint. */
		signal_t *sig = signal_get_by_index((uint16_t)ad->signal_index);
		if (sig)
			_anim_on_signal(sig->current_value, sig->is_stale, w);
	}
}

static void _anim_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	w->w = nw;
	w->h = nh;
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_set_size(w->root, nw, nh);
	anim_data_t *ad = (anim_data_t *)w->type_data;
	if (!ad || ad->loaded_count == 0) return;
	if (ad->img_obj && lv_obj_is_valid(ad->img_obj)) {
		uint8_t cf = ad->cur_frame < ad->loaded_count ? ad->cur_frame : 0;
		uint16_t zoom = _anim_fit_zoom(nw, nh,
		                               ad->frames[cf]->header.w,
		                               ad->frames[cf]->header.h);
		lv_img_set_zoom(ad->img_obj, zoom);
	}
}

static void _anim_open_settings(widget_t *w) { (void)w; }

static void _anim_to_json(widget_t *w, cJSON *out) {
	anim_data_t *ad = (anim_data_t *)w->type_data;
	widget_base_to_json(w, out);
	if (!ad) return;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (!cfg) return;
	/* Defaults-only (32 KB layout budget) — keep in lockstep with the
	 * factory defaults below AND the schema defaults. */
	if (ad->frame_prefix[0] != '\0')
		cJSON_AddStringToObject(cfg, "frame_prefix", ad->frame_prefix);
	if (ad->frame_count != ANIM_DEFAULT_FRAMES)
		cJSON_AddNumberToObject(cfg, "frame_count", ad->frame_count);
	if (ad->mode != 0)
		cJSON_AddNumberToObject(cfg, "mode", ad->mode);
	if (ad->range_min != 0.0f)
		cJSON_AddNumberToObject(cfg, "range_min", ad->range_min);
	if (ad->range_max != ANIM_DEFAULT_RANGE_MAX)
		cJSON_AddNumberToObject(cfg, "range_max", ad->range_max);
	if (ad->threshold != ANIM_DEFAULT_THRESHOLD)
		cJSON_AddNumberToObject(cfg, "threshold", ad->threshold);
	if (ad->loop_fps != ANIM_DEFAULT_FPS)
		cJSON_AddNumberToObject(cfg, "loop_fps", ad->loop_fps);
	if (ad->hyst_pct != ANIM_DEFAULT_HYST_PCT)
		cJSON_AddNumberToObject(cfg, "hyst_pct", ad->hyst_pct);
	if (ad->signal_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "signal_name", ad->signal_name);
	if (ad->channel_id[0] != '\0')
		cJSON_AddStringToObject(cfg, "channel", ad->channel_id);
}

static void _anim_from_json(widget_t *w, cJSON *in) {
	anim_data_t *ad = (anim_data_t *)w->type_data;
	widget_base_from_json(w, in);
	if (!ad) return;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg) return;

	cJSON *item;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "frame_prefix");
	if (cJSON_IsString(item) && item->valuestring)
		safe_strncpy(ad->frame_prefix, item->valuestring, sizeof(ad->frame_prefix));
	item = cJSON_GetObjectItemCaseSensitive(cfg, "frame_count");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v < 1) v = 1;
		if (v > ANIM_MAX_FRAMES) v = ANIM_MAX_FRAMES;
		ad->frame_count = (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "mode");
	if (cJSON_IsNumber(item)) ad->mode = (item->valueint == 1) ? 1 : 0;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "range_min");
	if (cJSON_IsNumber(item)) ad->range_min = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "range_max");
	if (cJSON_IsNumber(item)) ad->range_max = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "threshold");
	if (cJSON_IsNumber(item)) ad->threshold = (float)item->valuedouble;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "loop_fps");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v < 1) v = 1;
		if (v > 30) v = 30;
		ad->loop_fps = (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "hyst_pct");
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		if (v < 0) v = 0;
		if (v > 10) v = 10;
		ad->hyst_pct = (uint8_t)v;
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "signal_name");
	if (cJSON_IsString(item) && item->valuestring)
		safe_strncpy(ad->signal_name, item->valuestring, sizeof(ad->signal_name));

	/* Resolve signal name → index */
	if (ad->signal_name[0] != '\0')
		ad->signal_index = signal_find_by_name(ad->signal_name);

	/* Channel binding (same v14 idiom as widget_bar: channel wins over a
	 * raw signal_name when it resolves; empty-signal channels fall back). */
	cJSON *ch_item = cJSON_GetObjectItemCaseSensitive(cfg, "channel");
	if (cJSON_IsString(ch_item) && ch_item->valuestring && ch_item->valuestring[0] != '\0')
		safe_strncpy(ad->channel_id, ch_item->valuestring, sizeof(ad->channel_id));
	channel_t *bound_c = ad->channel_id[0] ? channel_manager_get(ad->channel_id) : NULL;
	if (bound_c && bound_c->signal_index >= 0) {
		safe_strncpy(ad->signal_name, bound_c->signal_name, sizeof(ad->signal_name));
		ad->signal_index = bound_c->signal_index;
	}
}

static void _anim_destroy(widget_t *w) {
	if (!w) return;
	anim_data_t *ad = (anim_data_t *)w->type_data;
	if (ad) {
		if (ad->signal_index >= 0)
			signal_unsubscribe(ad->signal_index, _anim_on_signal, w);
		if (ad->loop_timer) {
			lv_timer_del(ad->loop_timer);
			ad->loop_timer = NULL;
		}
	}
	widget_rules_free(w);
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	if (ad) {
		ad->img_obj = NULL;      /* child of root — already deleted */
		_anim_free_frames(ad);
		free(ad);
	}
	free(w);
}

/* ── Inspector get / set (on-device editing) ─────────────────────────────── */

static bool _anim_inspector_get(const widget_t *w, const char *name,
                                widget_field_value_t *out) {
	if (!w || w->type != WIDGET_ANIM || !w->type_data || !name || !out) return false;
	const anim_data_t *ad = (const anim_data_t *)w->type_data;

	if (strcmp(name, "frame_prefix") == 0) { out->str = ad->frame_prefix; return true; }
	if (strcmp(name, "frame_count") == 0)  { out->i = ad->frame_count;    return true; }
	if (strcmp(name, "mode") == 0)         { out->i = ad->mode;           return true; }
	if (strcmp(name, "range_min") == 0)    { out->f = ad->range_min;      return true; }
	if (strcmp(name, "range_max") == 0)    { out->f = ad->range_max;      return true; }
	if (strcmp(name, "threshold") == 0)    { out->f = ad->threshold;      return true; }
	if (strcmp(name, "loop_fps") == 0)     { out->i = ad->loop_fps;       return true; }
	if (strcmp(name, "hyst_pct") == 0)     { out->i = ad->hyst_pct;       return true; }
	return false;
}

static bool _anim_inspector_set(widget_t *w, const char *name,
                                const widget_field_value_t *in) {
	if (!w || w->type != WIDGET_ANIM || !w->type_data || !name || !in) return false;
	anim_data_t *ad = (anim_data_t *)w->type_data;

	/* These three take effect only when _anim_create re-runs (frames load, the
	 * loop timer is wired per mode). Return FALSE so the caller falls back to a
	 * full rebuild — returning true tells the web editor the change was applied
	 * live, so it skips the rebuild and the new frames/mode never take. (Unlike
	 * widget_image, which the editor special-cases client-side; the false return
	 * makes this correct for every client, including RDM Studio.) The write to
	 * type_data still happens so an on-device inspector keeps the value. */
	if (strcmp(name, "frame_prefix") == 0 && in->str) {
		safe_strncpy(ad->frame_prefix, in->str, sizeof(ad->frame_prefix));
		return false;
	}
	if (strcmp(name, "frame_count") == 0) {
		int v = in->i;
		if (v < 1) v = 1;
		if (v > ANIM_MAX_FRAMES) v = ANIM_MAX_FRAMES;
		ad->frame_count = (uint8_t)v;
		return false;
	}
	if (strcmp(name, "mode") == 0) {
		ad->mode = (in->i == 1) ? 1 : 0;
		return false;
	}
	if (strcmp(name, "range_min") == 0) { ad->range_min = in->f; return true; }
	if (strcmp(name, "range_max") == 0) { ad->range_max = in->f; return true; }
	if (strcmp(name, "threshold") == 0) { ad->threshold = in->f; return true; }
	if (strcmp(name, "loop_fps") == 0) {
		int v = in->i;
		if (v < 1) v = 1;
		if (v > 30) v = 30;
		ad->loop_fps = (uint8_t)v;
		if (ad->loop_timer)
			lv_timer_set_period(ad->loop_timer, 1000 / ad->loop_fps);
		return true;
	}
	if (strcmp(name, "hyst_pct") == 0) {
		int v = in->i;
		if (v < 0) v = 0;
		if (v > 10) v = 10;
		ad->hyst_pct = (uint8_t)v;
		return true;
	}
	return false;
}

/* ── Factory ─────────────────────────────────────────────────────────────── */

widget_t *widget_anim_create_instance(uint8_t slot) {
	(void)slot; /* anim widgets don't use slots */

	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w) return NULL;

	anim_data_t *ad = heap_caps_calloc(1, sizeof(anim_data_t), MALLOC_CAP_SPIRAM);
	if (!ad) ad = calloc(1, sizeof(anim_data_t));
	if (!ad) { free(w); return NULL; }

	ad->frame_count  = ANIM_DEFAULT_FRAMES;
	ad->mode         = 0;
	ad->range_min    = 0.0f;
	ad->range_max    = ANIM_DEFAULT_RANGE_MAX;
	ad->threshold    = ANIM_DEFAULT_THRESHOLD;
	ad->loop_fps     = ANIM_DEFAULT_FPS;
	ad->hyst_pct     = ANIM_DEFAULT_HYST_PCT;
	ad->signal_index = -1;

	w->type = WIDGET_ANIM;
	w->slot = 0;
	w->x = 0;
	w->y = 0;
	w->w = ANIM_DEFAULT_W;
	w->h = ANIM_DEFAULT_H;
	w->type_data = ad;
	snprintf(w->id, sizeof(w->id), "anim_0");

	w->create        = _anim_create;
	w->resize        = _anim_resize;
	w->open_settings = _anim_open_settings;
	w->to_json       = _anim_to_json;
	w->from_json     = _anim_from_json;
	w->destroy       = _anim_destroy;
	w->apply_overrides  = NULL; /* rules not supported (editor hides the UI) */
	w->apply_night_mode = NULL; /* frames are user content; no night variant in v1 */
	w->inspector_get = _anim_inspector_get;
	w->inspector_set = _anim_inspector_set;

	return w;
}

#endif /* RDM_WIDGET_ANIM_ENABLED */
