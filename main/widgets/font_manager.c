/**
 * font_manager.c — Dynamic TTF font loading via lv_tiny_ttf.
 *
 * Two-level cache:
 *   - Family cache: raw TTF bytes in PSRAM, loaded from /lfs/fonts/
 *   - Instance cache: lv_font_t* at specific sizes, created on demand
 *
 * Thread safety: all public functions must be called from the LVGL task
 * (or while holding the LVGL mutex).
 */
#include "font_manager.h"
#include "widget_types.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "font_mgr";

#define LFS_FONT_DIR  "/lfs/fonts"
#define FONT_MAX_FILE_SIZE (512 * 1024)  /* 512 KB max per TTF */

/* ── Family cache (TTF data in PSRAM) ────────────────────────────────────── */

typedef struct {
	char     name[FONT_NAME_LEN];
	uint8_t *data;       /* PSRAM-allocated TTF bytes */
	size_t   data_size;
} font_family_t;

static font_family_t s_families[FONT_MAX_FAMILIES];
static uint8_t       s_family_count = 0;

/* ── Instance cache (lv_font_t at specific sizes) ────────────────────────── */

typedef struct {
	uint8_t    family_idx;  /* index into s_families */
	uint16_t   size;
	lv_font_t *font;       /* created by lv_tiny_ttf_create_data */
} font_instance_t;

static font_instance_t s_instances[FONT_MAX_INSTANCES];
static uint8_t         s_instance_count = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void _ensure_font_dir(void)
{
	struct stat st;
	if (stat(LFS_FONT_DIR, &st) != 0)
		mkdir(LFS_FONT_DIR, 0755);
}

static int _find_family(const char *name)
{
	for (uint8_t i = 0; i < s_family_count; i++) {
		if (strcasecmp(s_families[i].name, name) == 0)
			return (int)i;
	}
	return -1;
}

static bool _load_family_from_file(const char *name, const char *path)
{
	if (s_family_count >= FONT_MAX_FAMILIES) {
		ESP_LOGW(TAG, "Family cache full, skipping '%s'", name);
		return false;
	}

	FILE *f = fopen(path, "rb");
	if (!f) return false;

	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	fseek(f, 0, SEEK_SET);

	if (fsize <= 0 || fsize > FONT_MAX_FILE_SIZE) {
		ESP_LOGW(TAG, "Font '%s' size %ld out of range", name, fsize);
		fclose(f);
		return false;
	}

	uint8_t *buf = heap_caps_malloc((size_t)fsize, MALLOC_CAP_SPIRAM);
	if (!buf) {
		ESP_LOGE(TAG, "PSRAM alloc failed for font '%s' (%ld bytes)", name, fsize);
		fclose(f);
		return false;
	}

	size_t nr = fread(buf, 1, (size_t)fsize, f);
	fclose(f);

	if (nr != (size_t)fsize) {
		free(buf);
		return false;
	}

	font_family_t *fam = &s_families[s_family_count];
	safe_strncpy(fam->name, name, FONT_NAME_LEN);
	fam->data = buf;
	fam->data_size = (size_t)fsize;
	s_family_count++;

	ESP_LOGI(TAG, "Loaded font family '%s' (%u bytes)", name, (unsigned)fsize);
	return true;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void font_manager_init(void)
{
	_ensure_font_dir();

	/* Scan /lfs/fonts/ for .ttf files */
	DIR *d = opendir(LFS_FONT_DIR);
	if (!d) {
		ESP_LOGW(TAG, "No fonts directory");
		return;
	}

	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		size_t flen = strlen(de->d_name);
		if (flen <= 4 || strcasecmp(de->d_name + flen - 4, ".ttf") != 0)
			continue;

		/* Strip .ttf extension for family name */
		char fname[FONT_NAME_LEN];
		size_t copy = flen - 4;
		if (copy >= FONT_NAME_LEN) copy = FONT_NAME_LEN - 1;
		memcpy(fname, de->d_name, copy);
		fname[copy] = '\0';

		/* Skip if already loaded */
		if (_find_family(fname) >= 0) continue;

		char path[80];
		snprintf(path, sizeof(path), "%s/%s", LFS_FONT_DIR, de->d_name);
		_load_family_from_file(fname, path);

		if (s_family_count >= FONT_MAX_FAMILIES) break;
	}
	closedir(d);

	ESP_LOGI(TAG, "Font manager init: %u families loaded", s_family_count);
}

void font_manager_reset_instances(void)
{
	for (uint8_t i = 0; i < s_instance_count; i++) {
		if (s_instances[i].font) {
			lv_tiny_ttf_destroy(s_instances[i].font);
			s_instances[i].font = NULL;
		}
	}
	s_instance_count = 0;
	ESP_LOGD(TAG, "Font instances reset");
}

void font_manager_shutdown(void)
{
	font_manager_reset_instances();
	for (uint8_t i = 0; i < s_family_count; i++) {
		free(s_families[i].data);
		s_families[i].data = NULL;
		s_families[i].data_size = 0;
	}
	s_family_count = 0;
}

const lv_font_t *font_manager_get(const char *family, uint16_t size)
{
	if (!family || family[0] == '\0' || size < 8 || size > 128)
		return NULL;

	int fam_idx = _find_family(family);
	if (fam_idx < 0) return NULL;

	/* Check instance cache */
	for (uint8_t i = 0; i < s_instance_count; i++) {
		if (s_instances[i].family_idx == (uint8_t)fam_idx &&
		    s_instances[i].size == size) {
			return s_instances[i].font;
		}
	}

	/* Create new instance */
	if (s_instance_count >= FONT_MAX_INSTANCES) {
		ESP_LOGW(TAG, "Instance cache full (%u), cannot create %s:%u",
		         FONT_MAX_INSTANCES, family, size);
		return NULL;
	}

	font_family_t *fam = &s_families[fam_idx];
	lv_font_t *font = lv_tiny_ttf_create_data_ex(
		fam->data, fam->data_size, (lv_coord_t)size, 256);
	if (!font) {
		ESP_LOGE(TAG, "lv_tiny_ttf_create_data failed for '%s' size %u",
		         family, size);
		return NULL;
	}

	font_instance_t *inst = &s_instances[s_instance_count++];
	inst->family_idx = (uint8_t)fam_idx;
	inst->size = size;
	inst->font = font;

	ESP_LOGI(TAG, "Created font instance '%s:%u' (cache: %u/%u)",
	         family, size, s_instance_count, FONT_MAX_INSTANCES);

	/* Yield so the IDLE task on this core can run and feed its watchdog.
	 * Creating a new TTF instance via lv_tiny_ttf_create_data_ex can take
	 * 100–500 ms for glyph-cache preparation. During first-boot layout
	 * instantiation with many widgets requesting distinct font sizes, the
	 * cumulative time from inside widget-create calls can exceed the 5 s
	 * TWDT window on IDLE1 even with inter-widget yields. vTaskDelay(1)
	 * is 1 actual tick (2 ms at 500 Hz) — enough for IDLE to schedule. */
	vTaskDelay(1);

	return font;
}

uint8_t font_manager_family_count(void)
{
	return s_family_count;
}

const char *font_manager_family_name(uint8_t index)
{
	if (index >= s_family_count) return NULL;
	return s_families[index].name;
}

bool font_manager_add_family(const char *name, const uint8_t *data, size_t size)
{
	if (!name || !data || size == 0) return false;

	/* Check if family already exists — replace it */
	int idx = _find_family(name);
	if (idx >= 0) {
		/* Destroy any instances using this family */
		for (uint8_t i = 0; i < s_instance_count; ) {
			if (s_instances[i].family_idx == (uint8_t)idx) {
				if (s_instances[i].font)
					lv_tiny_ttf_destroy(s_instances[i].font);
				s_instances[i] = s_instances[--s_instance_count];
			} else {
				i++;
			}
		}
		free(s_families[idx].data);

		uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
		if (!buf) return false;
		memcpy(buf, data, size);
		s_families[idx].data = buf;
		s_families[idx].data_size = size;
		ESP_LOGI(TAG, "Replaced font family '%s' (%u bytes)", name, (unsigned)size);
		return true;
	}

	if (s_family_count >= FONT_MAX_FAMILIES) {
		ESP_LOGW(TAG, "Family cache full");
		return false;
	}

	uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
	if (!buf) return false;
	memcpy(buf, data, size);

	font_family_t *fam = &s_families[s_family_count];
	safe_strncpy(fam->name, name, FONT_NAME_LEN);
	fam->data = buf;
	fam->data_size = size;
	s_family_count++;

	ESP_LOGI(TAG, "Added font family '%s' (%u bytes)", name, (unsigned)size);
	return true;
}

bool font_manager_remove_family(const char *name)
{
	int idx = _find_family(name);
	if (idx < 0) return false;

	/* Destroy instances using this family */
	for (uint8_t i = 0; i < s_instance_count; ) {
		if (s_instances[i].family_idx == (uint8_t)idx) {
			if (s_instances[i].font)
				lv_tiny_ttf_destroy(s_instances[i].font);
			s_instances[i] = s_instances[--s_instance_count];
		} else {
			/* Fix up family_idx for families shifted down */
			if (s_instances[i].family_idx > (uint8_t)idx)
				s_instances[i].family_idx--;
			i++;
		}
	}

	/* Free family data */
	free(s_families[idx].data);

	/* Shift remaining families down */
	for (uint8_t i = (uint8_t)idx; i < s_family_count - 1; i++)
		s_families[i] = s_families[i + 1];
	s_family_count--;
	memset(&s_families[s_family_count], 0, sizeof(font_family_t));

	/* Delete file */
	char path[80];
	snprintf(path, sizeof(path), "%s/%s.ttf", LFS_FONT_DIR, name);
	remove(path);

	ESP_LOGI(TAG, "Removed font family '%s'", name);
	return true;
}
