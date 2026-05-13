#include "data_logger.h"
#include "widgets/signal.h"
#include "storage/sd_manager.h"
#include "storage/config_store.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "data_logger";

#define LOG_DEFAULT_RATE_HZ  10
#define LOG_DIR_SD           "/sdcard/logs"
#define LOG_DIR_LFS          "/lfs/logs"
#define LOG_MAX_SIGNALS      64

/* Flush thresholds — write to disk when EITHER of these hits.
 * Trades data-loss-on-power-cut window for write throughput. */
#define LOG_FLUSH_EVERY_SAMPLES  100   /* roughly every 1s at 100Hz */
#define LOG_FLUSH_EVERY_MS       2000  /* hard upper bound on lost data */

/* Per-log size cap when writing to LittleFS. The 8.8 MB LFS partition is
 * shared with layouts/images/fonts, so we keep individual logs modest so
 * users can record a few short traces, download them, and delete. 1 MB ~=
 * 10 minutes at 10 Hz with ~16 signals at ~160 B per sample. Logger auto-
 * stops cleanly when this is hit so we never wedge LFS on a runaway log. */
#define LOG_LFS_MAX_BYTES  (1024u * 1024u)

static FILE         *s_log_file       = NULL;
static lv_timer_t   *s_log_timer      = NULL;
static bool          s_active         = false;
static char          s_filename[80]   = "";
static char          s_log_dir[24]    = "";    /* "/sdcard/logs" or "/lfs/logs" */
static bool          s_on_lfs         = false; /* true → file-size cap applies */
static uint32_t      s_sample_count   = 0;
static uint32_t      s_start_time_ms  = 0;
static uint32_t      s_last_flush_ms  = 0;
static int16_t       s_signal_indices[LOG_MAX_SIGNALS];
static uint16_t      s_signal_count   = 0;
static uint16_t      s_rate_hz        = LOG_DEFAULT_RATE_HZ;

static uint32_t _get_ms(void) {
	return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void _ensure_log_dir(const char *dir) {
	struct stat st;
	if (stat(dir, &st) != 0)
		mkdir(dir, 0755);
}

/* Convert configured Hz to LVGL timer period in ms. 0 = Max ⇒ 1ms (LVGL will
 * coalesce to its actual tick rate, typically 5-14ms ≈ 70-200Hz effective). */
static uint32_t _period_ms_for_rate(uint16_t hz) {
	if (hz == 0)   return 1;            /* Max — coalesces to LVGL tick */
	if (hz > 1000) hz = 1000;           /* hard cap */
	uint32_t period = 1000U / hz;
	return period == 0 ? 1 : period;
}

static void _write_header(void) {
	if (!s_log_file) return;
	fprintf(s_log_file, "timestamp_ms");
	for (uint16_t i = 0; i < s_signal_count; i++) {
		signal_t *sig = signal_get_by_index(s_signal_indices[i]);
		if (sig) fprintf(s_log_file, ",%s", sig->name);
	}
	fprintf(s_log_file, "\n");
	fflush(s_log_file);
}

/* Forward decl — used by _log_timer_cb to auto-stop when LFS cap hit. */
esp_err_t data_logger_stop(void);

static void _log_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (!s_active || !s_log_file) return;

	uint32_t now      = _get_ms();
	uint32_t elapsed  = now - s_start_time_ms;
	fprintf(s_log_file, "%lu", (unsigned long)elapsed);

	for (uint16_t i = 0; i < s_signal_count; i++) {
		signal_t *sig = signal_get_by_index(s_signal_indices[i]);
		if (sig && !sig->is_stale)
			fprintf(s_log_file, ",%.4f", (double)sig->current_value);
		else
			fprintf(s_log_file, ",");
	}
	fprintf(s_log_file, "\n");
	s_sample_count++;

	/* Flush either every N samples or every N ms — whichever comes first.
	 * At higher rates the sample-based threshold dominates; at lower rates
	 * the time-based threshold guarantees data isn't lost on a brief power
	 * cut even at slow rates. */
	bool flushed = false;
	if (s_sample_count % LOG_FLUSH_EVERY_SAMPLES == 0 ||
	    (now - s_last_flush_ms) >= LOG_FLUSH_EVERY_MS) {
		fflush(s_log_file);
		s_last_flush_ms = now;
		flushed = true;
	}

	/* LFS cap: after each flush, check file size and auto-stop if exceeded.
	 * Without this an unattended log could fill the entire shared partition
	 * (8.8 MB also holds layouts / images / fonts). SD has gigabytes free so
	 * we skip the cap there entirely. */
	if (flushed && s_on_lfs) {
		long pos = ftell(s_log_file);
		if (pos > 0 && (size_t)pos >= LOG_LFS_MAX_BYTES) {
			ESP_LOGW(TAG, "LFS log reached %u-byte cap — auto-stopping",
			         (unsigned)LOG_LFS_MAX_BYTES);
			data_logger_stop();
		}
	}
}

/* Tear down + recreate the LVGL timer with the current s_rate_hz period.
 * Safe to call when not active (just removes any timer). */
static void _restart_timer(void) {
	if (s_log_timer) {
		lv_timer_del(s_log_timer);
		s_log_timer = NULL;
	}
	if (s_active) {
		uint32_t period = _period_ms_for_rate(s_rate_hz);
		s_log_timer = lv_timer_create(_log_timer_cb, period, NULL);
	}
}

void data_logger_init(void) {
	uint16_t saved = LOG_DEFAULT_RATE_HZ;
	if (config_store_load_log_rate_hz(&saved) == ESP_OK) {
		s_rate_hz = saved;
	}
	ESP_LOGI(TAG, "Initialized at %u Hz%s",
	         s_rate_hz, s_rate_hz == 0 ? " (Max)" : "");
}

esp_err_t data_logger_start(void) {
	if (s_active) return ESP_ERR_INVALID_STATE;

	/* Pick storage tier: SD card if mounted (cheap, large), else LittleFS
	 * (always available, capped at LOG_LFS_MAX_BYTES per file). This lets
	 * users record without an SD card and download via /api/log/download. */
	if (sd_manager_is_mounted()) {
		strncpy(s_log_dir, LOG_DIR_SD, sizeof(s_log_dir) - 1);
		s_on_lfs = false;
	} else {
		strncpy(s_log_dir, LOG_DIR_LFS, sizeof(s_log_dir) - 1);
		s_on_lfs = true;
	}
	s_log_dir[sizeof(s_log_dir) - 1] = '\0';
	_ensure_log_dir(s_log_dir);

	/* Generate filename using seconds since boot (ESP32 may not have RTC) */
	uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000000ULL);
	snprintf(s_filename, sizeof(s_filename), "%s/log_%lu.csv",
	         s_log_dir, (unsigned long)ts);

	s_log_file = fopen(s_filename, "w");
	if (!s_log_file) {
		ESP_LOGE(TAG, "Failed to create log file: %s", s_filename);
		s_filename[0] = '\0';
		s_log_dir[0]  = '\0';
		return ESP_FAIL;
	}

	/* Snapshot current signal list */
	s_signal_count = 0;
	uint16_t total = signal_get_count();
	for (uint16_t i = 0; i < total && s_signal_count < LOG_MAX_SIGNALS; i++) {
		signal_t *sig = signal_get_by_index(i);
		if (sig && sig->can_id != 0) {
			s_signal_indices[s_signal_count++] = i;
		}
	}

	_write_header();

	s_sample_count  = 0;
	s_start_time_ms = _get_ms();
	s_last_flush_ms = s_start_time_ms;
	s_active        = true;

	/* Create the LVGL timer with the active rate. */
	_restart_timer();

	ESP_LOGI(TAG, "Logging started: %s [%s] (%u signals, %u Hz%s%s)",
	         s_filename, s_on_lfs ? "LFS" : "SD",
	         s_signal_count,
	         s_rate_hz, s_rate_hz == 0 ? " [Max]" : "",
	         s_on_lfs ? " — capped at 1 MB" : "");
	return ESP_OK;
}

const char *data_logger_get_storage(void) {
	return s_on_lfs ? "lfs" : "sd";
}

const char *data_logger_get_dir(void) {
	return s_log_dir[0] ? s_log_dir : LOG_DIR_LFS;
}

uint32_t data_logger_lfs_max_bytes(void) {
	return LOG_LFS_MAX_BYTES;
}

esp_err_t data_logger_start_with_rate(uint16_t rate_hz, bool persist) {
	if (rate_hz > 1000) rate_hz = 1000;
	s_rate_hz = rate_hz;
	if (persist) config_store_save_log_rate_hz(rate_hz);
	return data_logger_start();
}

esp_err_t data_logger_stop(void) {
	if (!s_active) return ESP_ERR_INVALID_STATE;

	s_active = false;

	if (s_log_timer) {
		lv_timer_del(s_log_timer);
		s_log_timer = NULL;
	}

	if (s_log_file) {
		fflush(s_log_file);
		fclose(s_log_file);
		s_log_file = NULL;
	}

	ESP_LOGI(TAG, "Logging stopped: %s (%lu samples)",
	         s_filename, (unsigned long)s_sample_count);
	return ESP_OK;
}

bool data_logger_is_active(void) { return s_active; }
const char *data_logger_current_file(void) { return s_filename; }
uint32_t data_logger_get_sample_count(void) { return s_sample_count; }
uint32_t data_logger_get_elapsed_ms(void) {
	if (!s_active) return 0;
	return _get_ms() - s_start_time_ms;
}

void data_logger_set_rate_hz(uint16_t hz) {
	if (hz > 1000) hz = 1000;
	if (hz == s_rate_hz) return;
	s_rate_hz = hz;
	config_store_save_log_rate_hz(hz);
	/* If we're mid-log, tear down + recreate the timer with the new period
	 * so the change takes effect without restarting the file. */
	_restart_timer();
	ESP_LOGI(TAG, "Rate updated to %u Hz%s",
	         hz, hz == 0 ? " (Max)" : "");
}

uint16_t data_logger_get_rate_hz(void) { return s_rate_hz; }
