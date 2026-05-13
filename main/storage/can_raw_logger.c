#include "can_raw_logger.h"
#include "storage/sd_manager.h"
#include "storage/data_logger.h"   /* data_logger_lfs_max_bytes() — share the cap */
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "can_raw_logger";

#define LOG_DIR_SD   "/sdcard/logs"
#define LOG_DIR_LFS  "/lfs/logs"

/* Periodically flush so a power-cut loses at most ~2 s of frames. The same
 * trade-off data_logger makes — at a typical 200-500 frame/s bus this still
 * flushes thousands of frames at a time, so SD/LFS write cost stays amortised. */
#define FLUSH_EVERY_FRAMES   500
#define FLUSH_EVERY_MS       2000

static FILE        *s_file        = NULL;
static bool         s_active      = false;
static bool         s_on_lfs      = false;
static char         s_filename[80] = "";
static char         s_log_dir[24]  = "";
static uint32_t     s_frame_count = 0;
static uint64_t     s_start_us    = 0;
static uint32_t     s_last_flush_ms = 0;

static uint32_t _ms_since_start(void) {
	if (s_start_us == 0) return 0;
	return (uint32_t)((esp_timer_get_time() - (int64_t)s_start_us) / 1000LL);
}

void can_raw_logger_init(void) {
	/* Nothing persistent yet — keep symmetry with data_logger_init() so the
	 * boot sequence can call both unconditionally. */
}

esp_err_t can_raw_logger_start(void) {
	if (s_active) return ESP_ERR_INVALID_STATE;

	/* SD if mounted (no cap), else LFS (capped at data_logger_lfs_max_bytes). */
	if (sd_manager_is_mounted()) {
		strncpy(s_log_dir, LOG_DIR_SD, sizeof(s_log_dir) - 1);
		s_on_lfs = false;
	} else {
		strncpy(s_log_dir, LOG_DIR_LFS, sizeof(s_log_dir) - 1);
		s_on_lfs = true;
	}
	s_log_dir[sizeof(s_log_dir) - 1] = '\0';

	struct stat st;
	if (stat(s_log_dir, &st) != 0) mkdir(s_log_dir, 0755);

	uint32_t ts = (uint32_t)(esp_timer_get_time() / 1000000ULL);
	snprintf(s_filename, sizeof(s_filename), "%s/canraw_%lu.csv",
	         s_log_dir, (unsigned long)ts);

	s_file = fopen(s_filename, "w");
	if (!s_file) {
		ESP_LOGE(TAG, "Failed to open %s for write", s_filename);
		s_filename[0] = '\0';
		s_log_dir[0]  = '\0';
		return ESP_FAIL;
	}

	/* SavvyCAN GVRET-CSV header — imports directly into SavvyCAN without any
	 * conversion step. Same layout other CAN tools (CANalyzer, Vector ASC
	 * converters, etc.) understand. Time Stamp is microseconds, IDs are hex
	 * with 0x prefix, Extended is true/false, Bus is the bus index (we have
	 * one bus → always 0), LEN is the DLC, and D1..D8 are individual bytes. */
	fputs("Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8\n", s_file);
	fflush(s_file);

	s_frame_count   = 0;
	s_start_us      = (uint64_t)esp_timer_get_time();
	s_last_flush_ms = 0;
	s_active        = true;
	ESP_LOGI(TAG, "Raw CAN capture started: %s [%s]%s",
	         s_filename, s_on_lfs ? "LFS" : "SD",
	         s_on_lfs ? " — capped" : "");
	return ESP_OK;
}

esp_err_t can_raw_logger_stop(void) {
	if (!s_active) return ESP_ERR_INVALID_STATE;
	s_active = false;
	if (s_file) {
		fflush(s_file);
		fclose(s_file);
		s_file = NULL;
	}
	ESP_LOGI(TAG, "Raw CAN capture stopped: %s (%lu frames)",
	         s_filename, (unsigned long)s_frame_count);
	return ESP_OK;
}

bool        can_raw_logger_is_active(void)    { return s_active; }
const char *can_raw_logger_current_file(void) { return s_filename; }
uint32_t    can_raw_logger_frame_count(void)  { return s_frame_count; }
uint32_t    can_raw_logger_elapsed_ms(void)   { return s_active ? _ms_since_start() : 0; }
const char *can_raw_logger_get_storage(void)  { return s_on_lfs ? "lfs" : "sd"; }

void can_raw_logger_record_frame(uint32_t id, bool ext,
                                  const uint8_t *data, uint8_t dlc) {
	if (!s_active || !s_file) return;

	uint32_t elapsed = _ms_since_start();   /* coarse 1 ms resolution */
	uint64_t now_us  = (uint64_t)esp_timer_get_time() - s_start_us;
	if (dlc > 8) dlc = 8;
	/* Row format: TimeStamp(us), ID(0x...), Extended(true|false), Bus(0),
	 * LEN, D1..D8 — each byte in its own column as 0xNN, unused bytes empty
	 * to make it obvious which were actually on the wire vs padding. */
	fprintf(s_file, "%llu,0x%lX,%s,0,%u",
	        (unsigned long long)now_us,
	        (unsigned long)id,
	        ext ? "true" : "false",
	        (unsigned)dlc);
	for (uint8_t i = 0; i < 8; i++) {
		if (i < dlc) fprintf(s_file, ",0x%02X", (unsigned)data[i]);
		else         fputc(',', s_file);
	}
	fputc('\n', s_file);
	s_frame_count++;

	/* Time-based flush threshold — at low frame rates the count-based
	 * threshold could otherwise leave data unwritten for many seconds. */
	bool flushed = false;
	if ((s_frame_count % FLUSH_EVERY_FRAMES) == 0 ||
	    (elapsed - s_last_flush_ms) >= FLUSH_EVERY_MS) {
		fflush(s_file);
		s_last_flush_ms = elapsed;
		flushed = true;
	}

	/* LFS cap protection — share the same limit as data_logger so the
	 * shared 8.8 MB partition is never wedged by a runaway capture. */
	if (flushed && s_on_lfs) {
		long pos = ftell(s_file);
		uint32_t cap = data_logger_lfs_max_bytes();
		if (pos > 0 && cap > 0 && (uint32_t)pos >= cap) {
			ESP_LOGW(TAG, "LFS cap reached (%lu bytes) — auto-stopping",
			         (unsigned long)cap);
			can_raw_logger_stop();
		}
	}
}
