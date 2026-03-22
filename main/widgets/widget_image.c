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
	if (hdr.width == 0 || hdr.height == 0 || hdr.width > 800 || hdr.height > 480) {
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

	/* Try to load the image from LittleFS */
	if (id->image_name[0] != '\0') {
		id->img_dsc = rdm_image_load(id->image_name);
		if (id->img_dsc) {
			id->img_obj = lv_img_create(cont);
			lv_img_set_src(id->img_obj, id->img_dsc);
			lv_obj_set_align(id->img_obj, LV_ALIGN_CENTER);
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

	w->root = cont;
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
	if (id->recolor_opa != 0)
		cJSON_AddNumberToObject(cfg, "recolor_opa", id->recolor_opa);
	if (id->recolor.full != lv_color_black().full)
		cJSON_AddNumberToObject(cfg, "recolor", (int)id->recolor.full);
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
	item = cJSON_GetObjectItemCaseSensitive(cfg, "recolor_opa");
	if (cJSON_IsNumber(item)) id->recolor_opa = (uint8_t)item->valueint;
	item = cJSON_GetObjectItemCaseSensitive(cfg, "recolor");
	if (cJSON_IsNumber(item)) id->recolor.full = (uint32_t)item->valueint;
}

static void _image_destroy(widget_t *w) {
	if (!w) return;
	widget_rules_free(w);
	if (w->root && lv_obj_is_valid(w->root))
		lv_obj_del(w->root);
	w->root = NULL;
	image_data_t *id = (image_data_t *)w->type_data;
	if (id) {
		rdm_image_free(id->img_dsc);
		free(id);
	}
	free(w);
}

widget_t *widget_image_create_instance(uint8_t slot) {
	(void)slot; /* images don't use slots */

	widget_t *w = calloc(1, sizeof(widget_t));
	if (!w) return NULL;

	image_data_t *id = heap_caps_calloc(1, sizeof(image_data_t), MALLOC_CAP_SPIRAM);
	if (!id) id = calloc(1, sizeof(image_data_t));
	if (!id) { free(w); return NULL; }

	id->opacity = 255;
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

	return w;
}
