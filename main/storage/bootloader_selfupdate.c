/*
 * bootloader_selfupdate.c — write a new 2nd-stage bootloader from the app.
 *
 * See the header for why this exists and what it risks. The ordering below is
 * the whole safety story, so it is worth stating once:
 *
 *   1. Every check that can refuse happens BEFORE the first sector is erased.
 *      Discovering a bad embedded image after erasing would brick the dash for
 *      nothing.
 *   2. The comparison is a byte-for-byte read-back, not a hash. It is the
 *      strongest check available and it is simpler than hashing.
 *   3. ESP_OK is returned only once the read-back has passed, so a caller that
 *      sees ESP_OK may reboot. Nothing here reboots by itself.
 */
#include "storage/bootloader_selfupdate.h"

#include "esp_flash.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <string.h>

static const char *TAG = "bl_selfupd";

/* The bootloader lives at 0x0 and must not reach the partition table at
 * 0x8000. Nothing is partitioned in between, so these are raw chip offsets and
 * esp_flash_* (not esp_partition_*) is the right API. */
#define BL_OFFSET      0x0000u
#define BL_REGION_SIZE 0x8000u   /* 0x0 .. partition table at 0x8000 */
#define FLASH_SECTOR   0x1000u

/* Embedded via EMBED_FILES in main/CMakeLists.txt. */
extern const uint8_t _bl_image_start[] asm("_binary_bootloader_bin_start");
extern const uint8_t _bl_image_end[]   asm("_binary_bootloader_bin_end");

size_t bootloader_selfupdate_image_size(void) {
	return (size_t)(_bl_image_end - _bl_image_start);
}

/* Sanity-check the image we are about to commit to offset 0.
 *
 * The flash-mode and size/frequency bytes matter more than they look. esptool
 * patches those two bytes when it writes a bootloader over USB; nothing
 * patches them here, so if the committed artifact disagrees with the chip the
 * dash comes back from the reboot dead. Checking them costs nothing and is the
 * difference between a refused update and a workshop visit. */
static bool _image_is_sane(const uint8_t *img, size_t len, const char **why) {
	if (len == 0 || len > BL_REGION_SIZE) {
		*why = "embedded bootloader is empty or too big for 0x0-0x8000";
		return false;
	}
	if (img[0] != 0xE9) {
		*why = "embedded bootloader has no ESP image magic (0xE9)";
		return false;
	}
	/* 2 = DIO. The dash is built CONFIG_ESPTOOLPY_FLASHMODE_DIO. */
	if (img[2] != 2) {
		*why = "embedded bootloader flash mode is not DIO";
		return false;
	}
	/* 0x4F = 16 MB @ 80 MHz, matching CONFIG_ESPTOOLPY_FLASHSIZE_16MB /
	 * CONFIG_ESPTOOLPY_FLASHFREQ_80M. */
	if (img[3] != 0x4F) {
		*why = "embedded bootloader is not 16MB/80m";
		return false;
	}
	return true;
}

static uint8_t *_read_running_bootloader(size_t len) {
	uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
	if (!buf) buf = malloc(len);
	if (!buf) return NULL;
	if (esp_flash_read(NULL, buf, BL_OFFSET, len) != ESP_OK) {
		free(buf);
		return NULL;
	}
	return buf;
}

bl_selfupdate_state_t bootloader_selfupdate_check(void) {
	size_t len = bootloader_selfupdate_image_size();
	if (len == 0) return BL_SELFUPDATE_UNREADABLE;

	uint8_t *cur = _read_running_bootloader(len);
	if (!cur) return BL_SELFUPDATE_UNREADABLE;

	bool same = (memcmp(cur, _bl_image_start, len) == 0);
	free(cur);
	return same ? BL_SELFUPDATE_UP_TO_DATE : BL_SELFUPDATE_DIFFERENT;
}

esp_err_t bootloader_selfupdate_apply(const char **err_detail,
                                      bool *past_point_of_no_return) {
	const char *why = "";
	if (past_point_of_no_return) *past_point_of_no_return = false;

	const uint8_t *img = _bl_image_start;
	size_t len = bootloader_selfupdate_image_size();

	/* ── everything that can refuse, before anything is destroyed ─────── */
	if (!_image_is_sane(img, len, &why)) {
		ESP_LOGE(TAG, "refusing: %s", why);
		if (err_detail) *err_detail = why;
		return ESP_ERR_INVALID_ARG;
	}

	bl_selfupdate_state_t state = bootloader_selfupdate_check();
	if (state == BL_SELFUPDATE_UNREADABLE) {
		why = "could not read the running bootloader";
		ESP_LOGE(TAG, "refusing: %s", why);
		if (err_detail) *err_detail = why;
		return ESP_ERR_INVALID_STATE;
	}
	if (state == BL_SELFUPDATE_UP_TO_DATE) {
		ESP_LOGI(TAG, "bootloader already current (%u bytes) — nothing to do",
		         (unsigned)len);
		if (err_detail) *err_detail = "already up to date";
		return ESP_OK;
	}

	/* A read-back buffer allocated up front. Failing to allocate it AFTER the
	 * erase would mean writing a bootloader we then could not verify. */
	uint8_t *verify = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
	if (!verify) verify = malloc(len);
	if (!verify) {
		why = "no memory for the verification buffer";
		ESP_LOGE(TAG, "refusing: %s", why);
		if (err_detail) *err_detail = why;
		return ESP_ERR_NO_MEM;
	}

	/* ── past here the dash needs a USB reflash if anything goes wrong ── */
	ESP_LOGW(TAG, "writing bootloader: %u bytes to 0x%X — DO NOT CUT POWER",
	         (unsigned)len, BL_OFFSET);
	if (past_point_of_no_return) *past_point_of_no_return = true;

	esp_err_t err = esp_flash_erase_region(NULL, BL_OFFSET, BL_REGION_SIZE);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(err));
		free(verify);
		if (err_detail) *err_detail = "erase failed";
		return err;
	}

	err = esp_flash_write(NULL, img, BL_OFFSET, len);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "write failed: %s", esp_err_to_name(err));
		free(verify);
		if (err_detail) *err_detail = "write failed";
		return err;
	}

	/* Byte-for-byte read-back. Anything less and "verified" would mean
	 * "the write call returned OK", which is not the same claim. */
	err = esp_flash_read(NULL, verify, BL_OFFSET, len);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "read-back failed: %s", esp_err_to_name(err));
		free(verify);
		if (err_detail) *err_detail = "read-back failed";
		return err;
	}

	bool ok = (memcmp(verify, img, len) == 0);
	free(verify);
	if (!ok) {
		ESP_LOGE(TAG, "read-back MISMATCH — do not reboot this dash");
		if (err_detail) *err_detail = "read-back mismatch";
		return ESP_ERR_INVALID_CRC;
	}

	ESP_LOGW(TAG, "bootloader written and verified (%u bytes) — safe to reboot",
	         (unsigned)len);
	if (err_detail) *err_detail = "written and verified";
	return ESP_OK;
}
