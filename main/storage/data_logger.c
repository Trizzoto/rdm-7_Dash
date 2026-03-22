#include "data_logger.h"
#include "widgets/signal.h"
#include "storage/sd_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "data_logger";

#define LOG_INTERVAL_MS  100   /* 10 Hz sample rate */
#define LOG_DIR          "/sdcard/logs"
#define LOG_MAX_SIGNALS  64

static FILE *s_log_file = NULL;
static lv_timer_t *s_log_timer = NULL;
static bool s_active = false;
static char s_filename[64] = "";
static uint32_t s_sample_count = 0;
static uint32_t s_start_time_ms = 0;
static int16_t s_signal_indices[LOG_MAX_SIGNALS];
static uint16_t s_signal_count = 0;

static uint32_t _get_ms(void) {
	return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void _ensure_log_dir(void) {
	struct stat st;
	if (stat(LOG_DIR, &st) != 0)
		mkdir(LOG_DIR, 0755);
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

static void _log_timer_cb(lv_timer_t *timer) {
	(void)timer;
	if (!s_active || !s_log_file) return;

	uint32_t elapsed = _get_ms() - s_start_time_ms;
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

	/* Flush every 50 samples (5 seconds at 10Hz) to limit data loss on power cut */
	if (s_sample_count % 50 == 0)
		fflush(s_log_file);
}

void data_logger_init(void) {
	/* Nothing to do at init -- SD mount is handled by sd_manager */
}

esp_err_t data_logger_start(void) {
	if (s_active) return ESP_ERR_INVALID_STATE;
	if (!sd_manager_is_mounted()) {
		ESP_LOGE(TAG, "SD card not mounted");
		return ESP_ERR_NOT_FOUND;
	}

	_ensure_log_dir();

	/* Generate filename using seconds since boot (ESP32 may not have RTC) */
	uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000000ULL);
	snprintf(s_filename, sizeof(s_filename), LOG_DIR "/log_%lu.csv", (unsigned long)ts);

	s_log_file = fopen(s_filename, "w");
	if (!s_log_file) {
		ESP_LOGE(TAG, "Failed to create log file: %s", s_filename);
		s_filename[0] = '\0';
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

	s_sample_count = 0;
	s_start_time_ms = _get_ms();
	s_active = true;

	/* Create LVGL timer for periodic sampling */
	s_log_timer = lv_timer_create(_log_timer_cb, LOG_INTERVAL_MS, NULL);

	ESP_LOGI(TAG, "Logging started: %s (%u signals)", s_filename, s_signal_count);
	return ESP_OK;
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

	ESP_LOGI(TAG, "Logging stopped: %s (%lu samples)", s_filename, (unsigned long)s_sample_count);
	return ESP_OK;
}

bool data_logger_is_active(void) { return s_active; }
const char *data_logger_current_file(void) { return s_filename; }
uint32_t data_logger_get_sample_count(void) { return s_sample_count; }
uint32_t data_logger_get_elapsed_ms(void) {
	if (!s_active) return 0;
	return _get_ms() - s_start_time_ms;
}
