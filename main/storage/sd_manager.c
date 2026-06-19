#include "sd_manager.h"
#include <stdint.h>
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <sys/stat.h>

static const char *TAG = "sd_manager";

#define SD_MOSI  11
#define SD_CLK   12
#define SD_MISO  13
#define SD_CS     4

static bool s_sd_mounted = false;

/* Consecutive esp_vfs_fat_info failures seen by sd_manager_get_info — the
 * storage UI polls that endpoint, which makes it a free periodic health
 * probe. Two strikes in a row = card is gone, flip to unmounted. */
static uint8_t s_probe_failures = 0;

/* Mark the card dead WITHOUT unmounting the FATFS VFS. The loggers (and a
 * replay session) may still hold open FILE*s on /sdcard; unregistering the
 * VFS under them would turn their later fclose() into a crash. Leaving the
 * VFS registered just makes those fds return IO errors, which every caller
 * already handles. The cost: a re-inserted card needs a reboot to come back
 * (acceptable — hot-REMOVAL safety is the goal, hot-reinsert is not a
 * supported flow). Everything else gates on sd_manager_is_mounted(), so
 * flipping the flag stops new SD access (and the SPI-timeout burn) at once. */
static void _mark_card_dead(const char *why) {
	if (!s_sd_mounted) return;
	s_sd_mounted = false;
	ESP_LOGE(TAG, "SD card unresponsive (%s) — marked unmounted; "
	         "loggers fall back to internal flash, reinsert needs a reboot", why);
}

static void _ensure_dir(const char *path) {
	struct stat st;
	if (stat(path, &st) != 0)
		mkdir(path, 0755);
}

esp_err_t sd_manager_init(void) {
	ESP_LOGI(TAG, "Initializing SD card (SPI)");

	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
		.format_if_mount_failed = false,
		.max_files = 5,
		.allocation_unit_size = 16 * 1024,
	};

	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	host.max_freq_khz = 10000;

	spi_bus_config_t bus_cfg = {
		.mosi_io_num   = SD_MOSI,
		.miso_io_num   = SD_MISO,
		.sclk_io_num   = SD_CLK,
		/* Explicitly disable quad pins — zero-initialized defaults to
		 * GPIO 0 which is the LCD G3 data line, and spi_bus_free() would
		 * then reset GPIO 0 on the error path and glitch the LCD. */
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4000,
	};
	esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "SPI bus init failed: %s — running without SD card",
				 esp_err_to_name(ret));
		return ret;
	}

	sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot_config.gpio_cs = SD_CS;
	slot_config.host_id = host.slot;

	sdmmc_card_t *card = NULL;
	ret = esp_vfs_fat_sdspi_mount(SD_BASE_PATH, &host, &slot_config,
								  &mount_config, &card);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "SD card mount failed: %s — running without SD card",
				 esp_err_to_name(ret));
		spi_bus_free(host.slot);
		return ret;
	}

	s_sd_mounted = true;
	ESP_LOGI(TAG, "SD card mounted at %s", SD_BASE_PATH);
	sdmmc_card_print_info(stdout, card);

	_ensure_dir(SD_LAYOUT_DIR);
	_ensure_dir(SD_IMAGE_DIR);
	_ensure_dir(SD_FONT_DIR);

	return ESP_OK;
}

bool sd_manager_is_mounted(void) { return s_sd_mounted; }

void sd_manager_notify_io_failure(void) {
	if (!s_sd_mounted) return;
	/* A logger just saw repeated write failures. Probe the volume once: if
	 * even f_getfree can't talk to the card, it was pulled (or died). If the
	 * probe succeeds the card is fine and the failure was file-level (FS
	 * full, bad sector) — leave the mount alone so other SD features keep
	 * working. */
	uint64_t total_bytes, free_bytes;
	if (esp_vfs_fat_info(SD_BASE_PATH, &total_bytes, &free_bytes) != ESP_OK) {
		_mark_card_dead("write failures + probe failed");
	} else {
		ESP_LOGW(TAG, "logger write failures but card still responds — "
		         "filesystem full or file-level error, mount kept");
	}
}

esp_err_t sd_manager_get_info(size_t *total, size_t *used, size_t *free_out) {
	if (!s_sd_mounted) return ESP_ERR_INVALID_STATE;

	uint64_t total_bytes, free_bytes;
	esp_err_t ret = esp_vfs_fat_info(SD_BASE_PATH, &total_bytes, &free_bytes);
	if (ret != ESP_OK) {
		if (++s_probe_failures >= 2) _mark_card_dead("info probe failed twice");
		return ret;
	}
	s_probe_failures = 0;

	uint64_t used_bytes = (total_bytes > free_bytes) ? (total_bytes - free_bytes) : 0;

	if (total)    *total    = (total_bytes > SIZE_MAX) ? SIZE_MAX : (size_t)total_bytes;
	if (used)     *used     = (used_bytes  > SIZE_MAX) ? SIZE_MAX : (size_t)used_bytes;
	if (free_out) *free_out = (free_bytes  > SIZE_MAX) ? SIZE_MAX : (size_t)free_bytes;
	return ESP_OK;
}
