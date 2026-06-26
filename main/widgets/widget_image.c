/*
 * widget_image.c — Image widget that loads RDMIMG binary files from LittleFS.
 *
 * Images are stored at /lfs/images/<name>.rdmimg in a custom binary format:
 *   - 12-byte header (magic "RDMI", width, height, color format, reserved)
 *   - Followed by width * height * 3 bytes of RGB565+alpha pixel data
 *
 * The browser converts PNG/JPG to this format before uploading.
 */
#include "widget_image.h"
#include "widget_rules.h"
#include "screen_config.h"
#include "system/night_mode.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "widget_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_image";

#define IMAGE_DEFAULT_W 100
#define IMAGE_DEFAULT_H 100
#define LFS_IMAGE_DIR "/lfs/images"

/* Compute fit-contain zoom (LVGL units, 256 = 100%) to fill a cw×ch container
 * while preserving the aspect ratio of an iw×ih image. */
static uint16_t _fit_zoom(uint16_t cw, uint16_t ch, uint16_t iw, uint16_t ih) {
	if (iw == 0 || ih == 0) return 256;
	uint32_t zx = (uint32_t)cw * 256 / iw;
	uint32_t zy = (uint32_t)ch * 256 / ih;
	uint16_t z  = (uint16_t)(zx < zy ? zx : zy);
	if (z < 1)    z = 1;
	if (z > 4096) z = 4096; /* LVGL v8 maximum zoom */
	return z;
}

/* ── Reference-counted image cache ───────────────────────────────────────────
 * rdm_image_load() used to allocate a fresh PSRAM copy of the pixel data on
 * EVERY call, so the same image placed on N widgets cost N× the memory — and
 * once PSRAM ran out the extra copies silently failed to load (the image only
 * appeared on one widget). Cache decoded descriptors by name with a refcount so
 * every widget referencing an image shares ONE buffer; freed when the last user
 * releases it. All access is on the LVGL task (layout load / widget create /
 * destroy), so no locking is needed. Updated image files are picked up on the
 * next layout reload, when all widgets free their refs and the entry drops. */
#define IMG_CACHE_MAX 24
typedef struct {
	char          name[40];
	lv_img_dsc_t *dsc;
	uint16_t      refs;
} img_cache_entry_t;
static img_cache_entry_t s_img_cache[IMG_CACHE_MAX];

static lv_img_dsc_t *_img_cache_acquire(const char *name) {
	for (int i = 0; i < IMG_CACHE_MAX; i++) {
		if (s_img_cache[i].dsc && strcmp(s_img_cache[i].name, name) == 0) {
			s_img_cache[i].refs++;
			ESP_LOGD(TAG, "Shared image '%s' (refs=%u)", name, s_img_cache[i].refs);
			return s_img_cache[i].dsc;
		}
	}
	return NULL;
}

static void _img_cache_store(const char *name, lv_img_dsc_t *dsc) {
	for (int i = 0; i < IMG_CACHE_MAX; i++) {
		if (!s_img_cache[i].dsc) {
			strncpy(s_img_cache[i].name, name, sizeof(s_img_cache[i].name) - 1);
			s_img_cache[i].name[sizeof(s_img_cache[i].name) - 1] = '\0';
			s_img_cache[i].dsc  = dsc;
			s_img_cache[i].refs = 1;
			return;
		}
	}
	/* Cache full (>24 distinct images): leave this one uncached — still valid,
	 * just unshared; rdm_image_free() frees it directly when released. */
	ESP_LOGW(TAG, "Image cache full; '%s' loaded unshared", name);
}

/* ── Slice cache: meter-sized opaque crops of a larger image ─────────────────
 * Reading a sub-rectangle of a big full-screen background is cache-hostile
 * (each scanline jumps the full image width), so redrawing it under a moving
 * needle is slow. A small meter-SIZED crop fits the data cache → ~2x faster.
 * rdm_image_slice() samples a source rect into a contiguous opaque (TRUE_COLOR)
 * face, cached + refcounted by (src, rect, dest-size) so re-applying a layout
 * only re-slices the meters whose geometry actually changed. */
#define SLICE_CACHE_MAX 24
typedef struct {
	const lv_img_dsc_t *src;
	int16_t  sx, sy, sw, sh, dw, dh;
	lv_img_dsc_t *dsc;
	uint16_t refs;
} slice_cache_entry_t;
static slice_cache_entry_t s_slice_cache[SLICE_CACHE_MAX];

static lv_img_dsc_t *_slice_cache_acquire(const lv_img_dsc_t *src,
		int sx, int sy, int sw, int sh, int dw, int dh) {
	for (int i = 0; i < SLICE_CACHE_MAX; i++) {
		slice_cache_entry_t *e = &s_slice_cache[i];
		if (e->dsc && e->src == src && e->sx == sx && e->sy == sy &&
		    e->sw == sw && e->sh == sh && e->dw == dw && e->dh == dh) {
			e->refs++;
			return e->dsc;
		}
	}
	return NULL;
}

static void _slice_cache_store(const lv_img_dsc_t *src,
		int sx, int sy, int sw, int sh, int dw, int dh, lv_img_dsc_t *dsc) {
	for (int i = 0; i < SLICE_CACHE_MAX; i++) {
		if (!s_slice_cache[i].dsc) {
			s_slice_cache[i].src = src;
			s_slice_cache[i].sx = (int16_t)sx; s_slice_cache[i].sy = (int16_t)sy;
			s_slice_cache[i].sw = (int16_t)sw; s_slice_cache[i].sh = (int16_t)sh;
			s_slice_cache[i].dw = (int16_t)dw; s_slice_cache[i].dh = (int16_t)dh;
			s_slice_cache[i].dsc = dsc; s_slice_cache[i].refs = 1;
			return;
		}
	}
}

lv_img_dsc_t *rdm_image_slice(const lv_img_dsc_t *src,
		int sx, int sy, int sw, int sh, int dw, int dh) {
	if (!src || !src->data || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return NULL;
	lv_img_dsc_t *cached = _slice_cache_acquire(src, sx, sy, sw, sh, dw, dh);
	if (cached) return cached;

	int snw = src->header.w, snh = src->header.h;
	int bpp = (src->header.cf == LV_IMG_CF_TRUE_COLOR_ALPHA) ? 3 : 2;  /* read RGB565, skip alpha */
	size_t out_sz = (size_t)dw * dh * 2;
	uint8_t *out = heap_caps_malloc(out_sz, MALLOC_CAP_SPIRAM);
	if (!out) return NULL;
	const uint8_t *sd = (const uint8_t *)src->data;
	for (int dy = 0; dy < dh; dy++) {
		int syp = sy + (int)((int64_t)dy * sh / dh);
		if (syp < 0) syp = 0; else if (syp >= snh) syp = snh - 1;
		const uint8_t *srow = sd + (size_t)syp * snw * bpp;
		uint8_t *orow = out + (size_t)dy * dw * 2;
		for (int dx = 0; dx < dw; dx++) {
			int sxp = sx + (int)((int64_t)dx * sw / dw);
			if (sxp < 0) sxp = 0; else if (sxp >= snw) sxp = snw - 1;
			const uint8_t *sp = srow + (size_t)sxp * bpp;
			orow[dx * 2] = sp[0]; orow[dx * 2 + 1] = sp[1];
		}
	}
	lv_img_dsc_t *dsc = calloc(1, sizeof(lv_img_dsc_t));
	if (!dsc) { heap_caps_free(out); return NULL; }
	dsc->header.always_zero = 0;
	dsc->header.w = dw; dsc->header.h = dh;
	dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
	dsc->data_size = out_sz; dsc->data = out;
	_slice_cache_store(src, sx, sy, sw, sh, dw, dh, dsc);
	return dsc;
}

lv_img_dsc_t *rdm_image_load(const char *name) {
	if (!name || name[0] == '\0') return NULL;

	lv_img_dsc_t *shared = _img_cache_acquire(name);
	if (shared) return shared;

	char path[80];
	snprintf(path, sizeof(path), "%s/%s.rdmimg", LFS_IMAGE_DIR, name);

	FILE *f = fopen(path, "rb");
	if (!f) {
		ESP_LOGW(TAG, "Cannot open image file: %s", path);
		return NULL;
	}

	/* Read header */
	rdm_image_header_t hdr;
	if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
		ESP_LOGE(TAG, "Failed to read image header from %s", path);
		fclose(f);
		return NULL;
	}

	/* Validate magic */
	if (memcmp(hdr.magic, "RDMI", 4) != 0) {
		ESP_LOGE(TAG, "Invalid magic in %s", path);
		fclose(f);
		return NULL;
	}

	/* Validate dimensions */
	if (hdr.width == 0 || hdr.height == 0 || hdr.width > SCREEN_W || hdr.height > SCREEN_H) {
		ESP_LOGE(TAG, "Invalid dimensions %ux%u in %s", hdr.width, hdr.height, path);
		fclose(f);
		return NULL;
	}

	/* Calculate pixel data size: LV_IMG_CF_TRUE_COLOR_ALPHA = 3 bytes/pixel (RGB565 + alpha) */
	size_t px_size = (size_t)hdr.width * hdr.height * LV_IMG_PX_SIZE_ALPHA_BYTE;

	/* Allocate pixel buffer in PSRAM */
	uint8_t *px_data = heap_caps_malloc(px_size, MALLOC_CAP_SPIRAM);
	if (!px_data) {
		ESP_LOGE(TAG, "Failed to allocate %u bytes for image %s", (unsigned)px_size, name);
		fclose(f);
		return NULL;
	}

	size_t nread = fread(px_data, 1, px_size, f);
	fclose(f);

	if (nread != px_size) {
		ESP_LOGE(TAG, "Short read for %s: got %u, expected %u", name, (unsigned)nread, (unsigned)px_size);
		heap_caps_free(px_data);
		return NULL;
	}

	/* Opaque fast-path: RDMIMG always stores TRUE_COLOR_ALPHA (3 B/px), which
	 * forces LVGL down the per-pixel alpha-blend draw path — expensive when a
	 * large background is re-composited from PSRAM under every moving needle.
	 * If the alpha channel is fully opaque (no pixel < 255), repack to
	 * TRUE_COLOR (2 B/px) so LVGL uses a straight opaque blit instead. The
	 * colour bytes are byte-identical to the alpha format, so the image renders
	 * the same. Compaction is done IN PLACE (2 B/px < 3 B/px, write index always
	 * trails read index) to avoid a transient second large PSRAM allocation. */
	lv_img_cf_t cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
	bool opaque = true;
	for (size_t i = 2; i < px_size; i += LV_IMG_PX_SIZE_ALPHA_BYTE) {
		if (px_data[i] != 0xFF) { opaque = false; break; }
	}
	if (opaque) {
		size_t j = 0;
		for (size_t i = 0; i < px_size; i += LV_IMG_PX_SIZE_ALPHA_BYTE) {
			px_data[j++] = px_data[i];
			px_data[j++] = px_data[i + 1];
		}
		size_t tc_size = (size_t)hdr.width * hdr.height * sizeof(lv_color_t);
		uint8_t *shrunk = heap_caps_realloc(px_data, tc_size, MALLOC_CAP_SPIRAM);
		if (shrunk) px_data = shrunk;   /* shrink can't fail in practice; keep old ptr if it does */
		px_size = tc_size;
		cf = LV_IMG_CF_TRUE_COLOR;
	}

	/* Yield to let the idle tasks run after a potentially-long fread().
	 * A 1+ MB read from LittleFS can take several hundred ms, especially
	 * with flash wear-leveling lookups; chaining multiple large images
	 * during layout load can cumulatively starve IDLE1 past the TWDT
	 * window. vTaskDelay(1) is 1 actual tick (2 ms at 500 Hz tick rate),
	 * which is enough for IDLE1 to schedule and reset the watchdog. */
	vTaskDelay(1);

	/* Allocate and fill the LVGL image descriptor */
	lv_img_dsc_t *dsc = calloc(1, sizeof(lv_img_dsc_t));
	if (!dsc) {
		heap_caps_free(px_data);
		return NULL;
	}

	dsc->header.always_zero = 0;
	dsc->header.w = hdr.width;
	dsc->header.h = hdr.height;
	dsc->header.cf = cf;
	dsc->data_size = px_size;
	dsc->data = px_data;

	ESP_LOGI(TAG, "Loaded image '%s' (%ux%u, %u bytes, %s)", name, hdr.width, hdr.height,
	         (unsigned)px_size, cf == LV_IMG_CF_TRUE_COLOR ? "opaque" : "alpha");
	_img_cache_store(name, dsc);
	return dsc;
}

void rdm_image_free(lv_img_dsc_t *dsc) {
	if (!dsc) return;
	for (int i = 0; i < IMG_CACHE_MAX; i++) {
		if (s_img_cache[i].dsc == dsc) {
			if (s_img_cache[i].refs > 0) s_img_cache[i].refs--;
			if (s_img_cache[i].refs == 0) {
				heap_caps_free((void *)dsc->data);
				free(dsc);
				s_img_cache[i].dsc = NULL;
				s_img_cache[i].name[0] = '\0';
			}
			return;
		}
	}
	/* Slice cache (auto-faces cut from a larger image). */
	for (int i = 0; i < SLICE_CACHE_MAX; i++) {
		if (s_slice_cache[i].dsc == dsc) {
			if (s_slice_cache[i].refs > 0) s_slice_cache[i].refs--;
			if (s_slice_cache[i].refs == 0) {
				heap_caps_free((void *)dsc->data);
				free(dsc);
				s_slice_cache[i].dsc = NULL;
				s_slice_cache[i].src = NULL;
			}
			return;
		}
	}
	/* Not in any cache — free directly. */
	heap_caps_free((void *)dsc->data);
	free(dsc);
}

/* Return the bottom screen-background image's descriptor + the rect it actually
 * occupies on screen (after auto_size zoom) + its native size. Used by the
 * meter auto-slice pass to cut each gauge's face from the shared background. */
bool widget_image_get_bg_geometry(widget_t *w, lv_img_dsc_t **dsc,
                                  lv_area_t *disp, uint16_t *nw, uint16_t *nh) {
	if (!w || w->type != WIDGET_IMAGE || !dsc || !disp) return false;
	image_data_t *id = (image_data_t *)w->type_data;
	if (!id || !id->img_dsc) return false;
	uint16_t native_w = id->native_w ? id->native_w : id->img_dsc->header.w;
	uint16_t native_h = id->native_h ? id->native_h : id->img_dsc->header.h;
	uint16_t zoom = id->auto_size ? _fit_zoom(w->w, w->h, native_w, native_h)
	                              : (id->image_scale ? id->image_scale : 256);
	int disp_w = (int)native_w * zoom / 256;
	int disp_h = (int)native_h * zoom / 256;
	int cx = SCREEN_ORIGIN_X + w->x;   /* image root is center-aligned */
	int cy = SCREEN_ORIGIN_Y + w->y;
	disp->x1 = cx - disp_w / 2; disp->y1 = cy - disp_h / 2;
	disp->x2 = disp->x1 + disp_w - 1; disp->y2 = disp->y1 + disp_h - 1;
	*dsc = id->img_dsc; *nw = native_w; *nh = native_h;
	return true;
}

/* Forward declarations — used by _image_create / _image_destroy below. */
static void _image_apply_night_mode(widget_t *w, bool active);
static void _image_night_cb(bool active, void *user_data);

static void _image_create(widget_t *w, lv_obj_t *parent) {
	image_data_t *id = (image_data_t *)w->type_data;
	if (!id) return;

	/* Create a container for the image. Images are pure decoration: pointer
	 * events must pass through so a user can still long-press a widget that
	 * sits behind an image overlay. Clearing CLICKABLE here complements the
	 * same gate in dashboard.c's event registration. */
	lv_obj_t *cont = lv_obj_create(parent);
	lv_obj_set_size(cont, w->w, w->h);
	lv_obj_set_align(cont, LV_ALIGN_CENTER);
	lv_obj_set_pos(cont, w->x, w->y);
	lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_opa(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Try to load the day image from LittleFS */
	if (id->image_name[0] != '\0') {
		id->img_dsc = rdm_image_load(id->image_name);
		if (id->img_dsc) {
			/* Track the name backing the loaded descriptor for the
			 * decode-cost guard in apply_overrides. */
			safe_strncpy(id->cur_image_name, id->image_name, sizeof(id->cur_image_name));
			id->native_w = id->img_dsc->header.w;
			id->native_h = id->img_dsc->header.h;
			id->img_obj = lv_img_create(cont);
			lv_img_set_src(id->img_obj, id->img_dsc);
			lv_obj_set_align(id->img_obj, LV_ALIGN_CENTER);
			uint16_t zoom = id->auto_size
				? _fit_zoom(w->w, w->h, id->native_w, id->native_h)
				: id->image_scale;
			if (zoom != 256)
				lv_img_set_zoom(id->img_obj, zoom);
			lv_obj_set_style_img_opa(id->img_obj, id->opacity,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
			if (id->recolor_opa > 0) {
				lv_obj_set_style_img_recolor(id->img_obj, id->recolor,
											  LV_PART_MAIN | LV_STATE_DEFAULT);
				lv_obj_set_style_img_recolor_opa(id->img_obj, id->recolor_opa,
												  LV_PART_MAIN | LV_STATE_DEFAULT);
			}
		} else {
			/* Show placeholder text if image not found */
			lv_obj_t *lbl = lv_label_create(cont);
			lv_label_set_text(lbl, id->image_name);
			lv_obj_set_align(lbl, LV_ALIGN_CENTER);
			lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888),
										LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	}

	/* If a night-mode image override is set and differs from the day image,
	 * load it now and create a hidden night lv_img. apply_night_mode() will
	 * toggle visibility instead of re-loading the descriptor. */
#if !NIGHT_MODE_DISABLED
	if (id->night.has_image_name &&
		id->night.image_name[0] != '\0' &&
		strncmp(id->night.image_name, id->image_name, sizeof(id->night.image_name)) != 0) {
		id->night_img_dsc = rdm_image_load(id->night.image_name);
		if (id->night_img_dsc) {
			id->night_img_obj = lv_img_create(cont);
			lv_img_set_src(id->night_img_obj, id->night_img_dsc);
			lv_obj_set_align(id->night_img_obj, LV_ALIGN_CENTER);
			uint16_t nzoom = id->auto_size
				? _fit_zoom(w->w, w->h, id->native_w, id->native_h)
				: id->image_scale;
			if (nzoom != 256)
				lv_img_set_zoom(id->night_img_obj, nzoom);
			lv_obj_set_style_img_opa(id->night_img_obj, id->opacity,
									  LV_PART_MAIN | LV_STATE_DEFAULT);
			/* Apply night recolor immediately if set, so first reveal looks right. */
			if (id->night.has_recolor) {
				lv_obj_set_style_img_recolor(id->night_img_obj, id->night.recolor,
											  LV_PART_MAIN | LV_STATE_DEFAULT);
				lv_obj_set_style_img_recolor_opa(id->night_img_obj, LV_OPA_COVER,
												  LV_PART_MAIN | LV_STATE_DEFAULT);
			} else if (id->recolor_opa > 0) {
				lv_obj_set_style_img_recolor(id->night_img_obj, id->recolor,
											  LV_PART_MAIN | LV_STATE_DEFAULT);
				lv_obj_set_style_img_recolor_opa(id->night_img_obj, id->recolor_opa,
												  LV_PART_MAIN | LV_STATE_DEFAULT);
			}
			lv_obj_add_flag(id->night_img_obj, LV_OBJ_FLAG_HIDDEN);
		}
	}
#endif /* !NIGHT_MODE_DISABLED */

	w->root = cont;

	/* Subscribe to night-mode changes if any night override is set. */
	if (id->night.has_recolor || id->night.has_image_name) {
		night_mode_subscribe(_image_night_cb, w);
		_image_apply_night_mode(w, night_mode_is_active());
	}
}

static void _image_resize(widget_t *w, uint16_t nw, uint16_t nh) {
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_set_size(w->root, nw, nh);
	w->w = nw;
	w->h = nh;
	image_data_t *id = (image_data_t *)w->type_data;
	if (!id || !id->auto_size || id->native_w == 0 || id->native_h == 0) return;
	uint16_t zoom = _fit_zoom(nw, nh, id->native_w, id->native_h);
	if (id->img_obj && lv_obj_is_valid(id->img_obj))
		lv_img_set_zoom(id->img_obj, zoom);
	if (id->night_img_obj && lv_obj_is_valid(id->night_img_obj))
		lv_img_set_zoom(id->night_img_obj, zoom);
}

static void _image_open_settings(widget_t *w) { (void)w; }

static void _image_to_json(widget_t *w, cJSON *out) {
	image_data_t *id = (image_data_t *)w->type_data;
	widget_base_to_json(w, out);
	if (!id) return;
	cJSON *cfg = cJSON_AddObjectToObject(out, "config");
	if (!cfg) return;
	if (id->image_name[0] != '\0')
		cJSON_AddStringToObject(cfg, "image_name", id->image_name);
	if (id->opacity != 255)
		cJSON_AddNumberToObject(cfg, "opacity", id->opacity);
	if (id->auto_size)
		cJSON_AddBoolToObject(cfg, "auto_size", true);
	else if (id->image_scale != 256)
		cJSON_AddNumberToObject(cfg, "image_scale", id->image_scale);
	if (id->recolor_opa != 0)
		cJSON_AddNumberToObject(cfg, "recolor_opa", id->recolor_opa);
	if (id->recolor.full != lv_color_black().full)
		cJSON_AddNumberToObject(cfg, "recolor", (int)id->recolor.full);
	/* Night-mode overrides — emit only fields that have an override set */
	{
		cJSON *n = cJSON_CreateObject();
		NIGHT_SERIALIZE_COLOR(n, id->night, recolor);
		NIGHT_SERIALIZE_IMAGE(n, id->night, image_name);
		if (cJSON_GetArraySize(n) > 0) cJSON_AddItemToObject(cfg, "night", n);
		else cJSON_Delete(n);
	}
}

static void _image_from_json(widget_t *w, cJSON *in) {
	image_data_t *id = (image_data_t *)w->type_data;
	widget_base_from_json(w, in);
	if (!id) return;
	cJSON *cfg = cJSON_GetObjectItemCaseSensitive(in, "config");
	if (!cfg) return;

	cJSON *item;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "image_name");
	if (cJSON_IsString(item) && item->valuestring) {
		safe_strncpy(id->image_name, item->valuestring, sizeof(id->image_name));
	}
	item = cJSON_GetObjectItemCaseSensitive(cfg, "opacity");
	if (cJSON_IsNumber(item)) id->opacity = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "auto_size");
	if (cJSON_IsBool(item)) id->auto_size = cJSON_IsTrue(item);
	item = cJSON_GetObjectItemCaseSensitive(cfg, "image_scale");
	if (cJSON_IsNumber(item)) id->image_scale = (uint16_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "recolor_opa");
	if (cJSON_IsNumber(item)) id->recolor_opa = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "recolor");
	if (cJSON_IsNumber(item)) id->recolor.full = (uint32_t)item->valueint;

	/* Night-mode overrides */
	cJSON *night = cJSON_GetObjectItemCaseSensitive(cfg, "night");
	if (cJSON_IsObject(night)) {
		NIGHT_PARSE_COLOR(night, id->night, recolor);
		NIGHT_PARSE_IMAGE(night, id->night, image_name);
	}

	/* Seed the CURRENT (shown) style state from the configured base so the
	 * first apply_overrides has the correct base to overlay onto / revert to.
	 * cur_image_name is set when the descriptor is actually loaded (in create
	 * and in apply_overrides). */
	id->cur_opacity     = id->opacity;
	id->cur_recolor     = id->recolor;
	id->cur_recolor_opa = id->recolor_opa;
}

/* ── Rule-driven overrides ─────────────────────────────────────────────────
 *
 * Lets the rules engine swap an image, dim it, or tint it based on a CAN
 * signal. The image_name path is the headline use case: e.g. show a green
 * battery icon while the battery is healthy and a red one when the
 * voltage falls below a threshold, without having to stack two image
 * widgets with mutually-exclusive visibility rules.
 *
 * On image_name override we free the old descriptor and load the new one
 * into the same lv_img object — re-using the day/night sibling pair to
 * keep night-mode swaps working. recolor / opacity / recolor_opa are
 * style-only and apply in-place to whichever object is currently
 * visible. */
static void _image_apply_overrides(widget_t *w, const rule_override_t *ov, uint8_t count) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	image_data_t *id = (image_data_t *)w->type_data;
	if (!id) return;

	/* Start from the configured BASE every call, then overlay the active
	 * overrides onto LOCALS. The base struct fields (image_name/opacity/
	 * recolor/recolor_opa) are never mutated, so to_json keeps emitting the
	 * configured values and a deactivation (count==0) fully reverts. */
	char       t_name[32];
	uint8_t    t_opacity     = id->opacity;
	lv_color_t t_recolor     = id->recolor;
	uint8_t    t_recolor_opa = id->recolor_opa;
	safe_strncpy(t_name, id->image_name, sizeof(t_name));

	for (uint8_t i = 0; i < count; i++) {
		const rule_override_t *o = &ov[i];
		if (strcmp(o->field_name, "image_name") == 0 && o->value_type == RULE_VAL_STRING) {
			safe_strncpy(t_name, o->value.str, sizeof(t_name));
		} else if (strcmp(o->field_name, "opacity") == 0 && o->value_type == RULE_VAL_NUMBER) {
			int v = (int)o->value.num; if (v < 0) v = 0; if (v > 255) v = 255;
			t_opacity = (uint8_t)v;
		} else if (strcmp(o->field_name, "recolor") == 0 && o->value_type == RULE_VAL_COLOR) {
			t_recolor.full = (uint16_t)o->value.color;
		} else if (strcmp(o->field_name, "recolor_opa") == 0 && o->value_type == RULE_VAL_NUMBER) {
			int v = (int)o->value.num; if (v < 0) v = 0; if (v > 255) v = 255;
			t_recolor_opa = (uint8_t)v;
		}
	}

	/* ── Apply once. ─────────────────────────────────────────────────────── */

	/* Image source: only free+reload the descriptor when the resolved target
	 * name differs from the one currently loaded (decode-cost guard). This is
	 * the single apply point, so a base-restore (count==0) correctly reloads
	 * the original image when an override had swapped it. */
	if (strncmp(t_name, id->cur_image_name, sizeof(id->cur_image_name)) != 0) {
		rdm_image_free(id->img_dsc);
		id->img_dsc = NULL;
		if (t_name[0] != '\0')
			id->img_dsc = rdm_image_load(t_name);
		safe_strncpy(id->cur_image_name, t_name, sizeof(id->cur_image_name));
		if (id->img_obj && lv_obj_is_valid(id->img_obj)) {
			/* Setting NULL when the new image fails to load clears the
			 * previous tile rather than leaving stale pixels. */
			lv_img_set_src(id->img_obj, id->img_dsc);
			if (id->img_dsc) {
				id->native_w = id->img_dsc->header.w;
				id->native_h = id->img_dsc->header.h;
				if (id->auto_size) {
					uint16_t zoom = _fit_zoom(w->w, w->h, id->native_w, id->native_h);
					if (zoom != 256) lv_img_set_zoom(id->img_obj, zoom);
				}
			}
		}
	}

	/* Opacity: applies to whichever object(s) exist. */
	if (id->img_obj && lv_obj_is_valid(id->img_obj))
		lv_obj_set_style_img_opa(id->img_obj, t_opacity,
			LV_PART_MAIN | LV_STATE_DEFAULT);
	if (id->night_img_obj && lv_obj_is_valid(id->night_img_obj))
		lv_obj_set_style_img_opa(id->night_img_obj, t_opacity,
			LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Recolor: always write both color + opa so a revert to recolor_opa==0
	 * actually disables a previously-applied tint. When night mode is active and
	 * a night recolor is set (day-only recolor case), the night tint takes
	 * precedence over the base so we don't fight _image_apply_night_mode. If a
	 * separate night IMAGE exists it carries its own baked colors on the night
	 * sibling, so we only ever recolor the day object here. */
	if (id->img_obj && lv_obj_is_valid(id->img_obj)) {
		lv_color_t rc  = t_recolor;
		uint8_t    rco = t_recolor_opa;
#if !NIGHT_MODE_DISABLED
		bool night_active = night_mode_is_active();
		bool has_night_img = id->night_img_obj && lv_obj_is_valid(id->night_img_obj);
		if (night_active && id->night.has_recolor && !has_night_img) {
			rc  = id->night.recolor;
			rco = LV_OPA_COVER;
		}
#endif
		lv_obj_set_style_img_recolor(id->img_obj, rc,
			LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_img_recolor_opa(id->img_obj, rco,
			LV_PART_MAIN | LV_STATE_DEFAULT);
	}

	/* Remember the shown style state (mirrors base when count==0). */
	id->cur_opacity     = t_opacity;
	id->cur_recolor     = t_recolor;
	id->cur_recolor_opa = t_recolor_opa;
}

static void _image_destroy(widget_t *w) {
	if (!w) return;
	night_mode_unsubscribe(_image_night_cb, w);
	widget_rules_free(w);
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	image_data_t *id = (image_data_t *)w->type_data;
	if (id) {
		rdm_image_free(id->img_dsc);
		rdm_image_free(id->night_img_dsc);
		free(id);
	}
	free(w);
}

/* Toggle day/night visibility (dual-object pattern) and apply recolor.
 * - If a night image was loaded, swap visibility between day and night objects.
 * - For the visible object, apply the appropriate recolor based on night state.
 * This avoids LVGL v8's single-source limitation for live image swap. */
static void _image_apply_night_mode(widget_t *w, bool active) {
	if (!w || !w->root || !lv_obj_is_valid(w->root)) return;
	image_data_t *id = (image_data_t *)w->type_data;
	if (!id) return;

	bool day_valid = id->img_obj && lv_obj_is_valid(id->img_obj);
	bool night_valid = id->night_img_obj && lv_obj_is_valid(id->night_img_obj);

	/* Visibility swap when both objects exist */
	if (day_valid && night_valid) {
		if (active) {
			lv_obj_add_flag(id->img_obj, LV_OBJ_FLAG_HIDDEN);
			lv_obj_clear_flag(id->night_img_obj, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_clear_flag(id->img_obj, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(id->night_img_obj, LV_OBJ_FLAG_HIDDEN);
		}
	}

	/* Recolor on whichever object is currently visible. When the night object
	 * exists it already has its colors baked in at create time, so we only
	 * need to handle the day object's recolor for the case where ONLY a
	 * recolor override exists (no separate night image). */
	if (day_valid && !night_valid) {
		/* Day fallback is the CURRENT (override-effective) state, not the raw
		 * base, so an active rule recolor survives a night→day toggle. The base
		 * fields stay untouched (to_json safe). */
		lv_color_t rc = NIGHT_PICK_COLOR(active, id->night, recolor, id->cur_recolor);
		if (id->cur_recolor_opa > 0 || (active && id->night.has_recolor)) {
			lv_obj_set_style_img_recolor(id->img_obj, rc,
				LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_img_recolor_opa(id->img_obj,
				(active && id->night.has_recolor) ? LV_OPA_COVER : id->cur_recolor_opa,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	}
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _image_night_cb(bool active, void *user_data) {
	_image_apply_night_mode((widget_t *)user_data, active);
}

/* ── Inspector get / set ───────────────────────────────────────────────────
 *
 * Note: schema "image_scale" is expressed as a 1..200 percent (default 100),
 * but the runtime stores the raw LVGL zoom value (256 = 100%). The hooks
 * here convert between the two so the inspector slider reads / writes the
 * expected percent values. image_name writes through to type_data — the
 * lv_img source swap needs a rebuild to honour the auto_size fit logic. */

static bool _image_inspector_get(const widget_t *w, const char *name,
                                 widget_field_value_t *out) {
	if (!w || w->type != WIDGET_IMAGE || !w->type_data || !name || !out) return false;
	const image_data_t *id = (const image_data_t *)w->type_data;

	if (strcmp(name, "image_name") == 0)  { out->str = id->image_name; return true; }
	if (strcmp(name, "image_scale") == 0) { out->i = (id->image_scale * 100 + 128) / 256; return true; }
	if (strcmp(name, "opacity") == 0)     { out->i = id->opacity;     return true; }
	if (strcmp(name, "recolor_opa") == 0) { out->i = id->recolor_opa; return true; }
	if (strcmp(name, "recolor") == 0)     { out->color = lv_color_to32(id->recolor) & 0xFFFFFF; return true; }
	return false;
}

static bool _image_inspector_set(widget_t *w, const char *name,
                                 const widget_field_value_t *in) {
	if (!w || w->type != WIDGET_IMAGE || !w->type_data || !name || !in) return false;
	image_data_t *id = (image_data_t *)w->type_data;

	if (strcmp(name, "image_name") == 0 && in->str) {
		safe_strncpy(id->image_name, in->str, sizeof(id->image_name));
		return true;   /* needs rebuild — auto_size fit + descriptor load on _image_create */
	}
	if (strcmp(name, "image_scale") == 0) {
		int pct = in->i; if (pct < 10) pct = 10; if (pct > 200) pct = 200;
		id->image_scale = (uint16_t)((pct * 256 + 50) / 100);
		id->auto_size = false;   /* explicit scale wins over auto-fit */
		if (id->img_obj && lv_obj_is_valid(id->img_obj))
			lv_img_set_zoom(id->img_obj, id->image_scale);
		return true;
	}
	if (strcmp(name, "opacity") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 255) v = 255;
		id->opacity = (uint8_t)v;
		id->cur_opacity = id->opacity;   /* keep effective in sync (no rule active) */
		if (id->img_obj && lv_obj_is_valid(id->img_obj))
			lv_obj_set_style_img_opa(id->img_obj, id->opacity,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		if (id->night_img_obj && lv_obj_is_valid(id->night_img_obj))
			lv_obj_set_style_img_opa(id->night_img_obj, id->opacity,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		return true;
	}
	if (strcmp(name, "recolor_opa") == 0) {
		int v = in->i; if (v < 0) v = 0; if (v > 255) v = 255;
		id->recolor_opa = (uint8_t)v;
		id->cur_recolor_opa = id->recolor_opa;   /* keep effective in sync */
		if (id->img_obj && lv_obj_is_valid(id->img_obj)) {
			lv_obj_set_style_img_recolor(id->img_obj, id->recolor,
				LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_img_recolor_opa(id->img_obj, id->recolor_opa,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		return true;
	}
	if (strcmp(name, "recolor") == 0) {
		id->recolor = lv_color_hex(in->color);
		id->cur_recolor = id->recolor;   /* keep effective in sync */
		if (id->img_obj && lv_obj_is_valid(id->img_obj) && id->recolor_opa > 0) {
			lv_obj_set_style_img_recolor(id->img_obj, id->recolor,
				LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_img_recolor_opa(id->img_obj, id->recolor_opa,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
		return true;
	}
	return false;
}

widget_t *widget_image_create_instance(uint8_t slot) {
	(void)slot; /* images don't use slots */

	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w) return NULL;

	image_data_t *id = heap_caps_calloc(1, sizeof(image_data_t), MALLOC_CAP_SPIRAM);
	if (!id) id = calloc(1, sizeof(image_data_t));
	if (!id) { free(w); return NULL; }

	id->opacity = 255;
	id->image_scale = 256;  /* 256 = 100% in LVGL zoom */
	id->recolor = lv_color_black();
	id->recolor_opa = 0;
	/* Seed CURRENT (shown) state from the configured base so a rule that fires
	 * before any config still reverts to a sane base on deactivation. */
	id->cur_image_name[0] = '\0';   /* nothing loaded yet */
	id->cur_opacity = id->opacity;
	id->cur_recolor = id->recolor;
	id->cur_recolor_opa = id->recolor_opa;

	w->type = WIDGET_IMAGE;
	w->slot = 0;
	w->x = 0;
	w->y = 0;
	w->w = IMAGE_DEFAULT_W;
	w->h = IMAGE_DEFAULT_H;
	w->type_data = id;
	snprintf(w->id, sizeof(w->id), "image_0");

	w->create = _image_create;
	w->resize = _image_resize;
	w->open_settings = _image_open_settings;
	w->to_json = _image_to_json;
	w->from_json = _image_from_json;
	w->destroy = _image_destroy;
	w->apply_overrides = _image_apply_overrides;
	w->apply_night_mode = _image_apply_night_mode;
	w->inspector_get = _image_inspector_get;
	w->inspector_set = _image_inspector_set;

	return w;
}
