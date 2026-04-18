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
#include "lvgl.h"
#include "widget_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "widget_image";

#define IMAGE_DEFAULT_W 100
#define IMAGE_DEFAULT_H 100
#define LFS_IMAGE_DIR "/lfs/images"

lv_img_dsc_t *rdm_image_load(const char *name) {
	if (!name || name[0] == '\0') return NULL;

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

	/* Allocate and fill the LVGL image descriptor */
	lv_img_dsc_t *dsc = calloc(1, sizeof(lv_img_dsc_t));
	if (!dsc) {
		heap_caps_free(px_data);
		return NULL;
	}

	dsc->header.always_zero = 0;
	dsc->header.w = hdr.width;
	dsc->header.h = hdr.height;
	dsc->header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
	dsc->data_size = px_size;
	dsc->data = px_data;

	ESP_LOGI(TAG, "Loaded image '%s' (%ux%u, %u bytes)", name, hdr.width, hdr.height, (unsigned)px_size);
	return dsc;
}

void rdm_image_free(lv_img_dsc_t *dsc) {
	if (!dsc) return;
	heap_caps_free((void *)dsc->data);
	free(dsc);
}

/* Forward declarations — used by _image_create / _image_destroy below. */
static void _image_apply_night_mode(widget_t *w, bool active);
static void _image_night_cb(bool active, void *user_data);

static void _image_create(widget_t *w, lv_obj_t *parent) {
	image_data_t *id = (image_data_t *)w->type_data;
	if (!id) return;

	/* Create a container for the image */
	lv_obj_t *cont = lv_obj_create(parent);
	lv_obj_set_size(cont, w->w, w->h);
	lv_obj_set_align(cont, LV_ALIGN_CENTER);
	lv_obj_set_pos(cont, w->x, w->y);
	lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_opa(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_border_width(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_pad_all(cont, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

	/* Try to load the day image from LittleFS */
	if (id->image_name[0] != '\0') {
		id->img_dsc = rdm_image_load(id->image_name);
		if (id->img_dsc) {
			id->img_obj = lv_img_create(cont);
			lv_img_set_src(id->img_obj, id->img_dsc);
			lv_obj_set_align(id->img_obj, LV_ALIGN_CENTER);
			if (id->image_scale != 256)
				lv_img_set_zoom(id->img_obj, id->image_scale);
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
	if (id->night.has_image_name &&
		id->night.image_name[0] != '\0' &&
		strncmp(id->night.image_name, id->image_name, sizeof(id->night.image_name)) != 0) {
		id->night_img_dsc = rdm_image_load(id->night.image_name);
		if (id->night_img_dsc) {
			id->night_img_obj = lv_img_create(cont);
			lv_img_set_src(id->night_img_obj, id->night_img_dsc);
			lv_obj_set_align(id->night_img_obj, LV_ALIGN_CENTER);
			if (id->image_scale != 256)
				lv_img_set_zoom(id->night_img_obj, id->image_scale);
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
	if (id->image_scale != 256)
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
		lv_color_t rc = NIGHT_PICK_COLOR(active, id->night, recolor, id->recolor);
		if (id->recolor_opa > 0 || (active && id->night.has_recolor)) {
			lv_obj_set_style_img_recolor(id->img_obj, rc,
				LV_PART_MAIN | LV_STATE_DEFAULT);
			lv_obj_set_style_img_recolor_opa(id->img_obj,
				(active && id->night.has_recolor) ? LV_OPA_COVER : id->recolor_opa,
				LV_PART_MAIN | LV_STATE_DEFAULT);
		}
	}
}

/* night_mode_subscribe callback shim — extracts widget_t* from user_data. */
static void _image_night_cb(bool active, void *user_data) {
	_image_apply_night_mode((widget_t *)user_data, active);
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
	w->apply_night_mode = _image_apply_night_mode;

	return w;
}
