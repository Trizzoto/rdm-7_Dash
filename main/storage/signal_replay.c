/*
 * signal_replay.c — CSV log playback through the signal system.
 *
 * Streams the file row-by-row. We don't load the whole file into memory: a
 * line buffer + the current FILE* position is enough for arbitrary-size logs.
 *
 * Pacing: the LVGL timer fires at REPLAY_TIMER_MS, advances the position
 * forward by `delta * speed` virtual milliseconds, and emits all rows whose
 * timestamps fall in that window. This keeps the timer cadence constant
 * regardless of the log's row spacing — sparse 1Hz logs and dense 200Hz
 * logs both play smoothly.
 */
#include "signal_replay.h"
#include "widgets/signal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "sig_replay";

#define REPLAY_TIMER_MS    20      /* worker tick — 50Hz pacing */
#define REPLAY_LINE_MAX    1024
#define REPLAY_MAX_COLS    64      /* matches LOG_MAX_SIGNALS in data_logger */
#define REPLAY_NAME_MAX    32

typedef struct {
	char     name[REPLAY_NAME_MAX]; /* column header text */
	int16_t  signal_index;          /* -1 = no matching signal, skip injection */
} replay_col_t;

static FILE       *s_file               = NULL;
static lv_timer_t *s_timer              = NULL;
static char        s_path[80]           = "";
static replay_col_t s_cols[REPLAY_MAX_COLS];
static uint16_t    s_col_count          = 0;
static uint32_t    s_row_index          = 0;     /* 0-based row #, advances per emitted row */
static uint32_t    s_total_rows         = 0;     /* counted on start by walking the file */
static uint32_t    s_first_ts_ms        = 0;     /* timestamp of the first row */
static uint32_t    s_next_ts_ms         = 0;     /* timestamp of the next row to emit (file-relative) */
static uint64_t    s_replay_start_us    = 0;     /* esp_timer at replay start */
static char        s_pending_line[REPLAY_LINE_MAX];
static bool        s_have_pending       = false;  /* s_pending_line holds a row not yet emitted */
static float       s_speed              = 1.0f;
static bool        s_loop               = false;

static void _replay_timer_cb(lv_timer_t *t);

static void _close(void)
{
	if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
	if (s_file)  { fclose(s_file);        s_file  = NULL; }
	s_path[0]      = '\0';
	s_col_count    = 0;
	s_row_index    = 0;
	s_total_rows   = 0;
	s_have_pending = false;
}

/* Read a line, stripping trailing \r\n. Returns false on EOF. */
static bool _read_line(char *buf, int buflen)
{
	if (!s_file) return false;
	if (!fgets(buf, buflen, s_file)) return false;
	int n = (int)strlen(buf);
	while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r')) buf[--n] = '\0';
	return true;
}

/* Parse the header row, mapping columns to signal indices. Returns false if
 * the layout doesn't match (no timestamp_ms column, etc). */
static bool _parse_header(const char *line)
{
	s_col_count = 0;
	const char *p = line;
	while (*p && s_col_count < REPLAY_MAX_COLS) {
		const char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);
		if (len >= REPLAY_NAME_MAX) len = REPLAY_NAME_MAX - 1;
		memcpy(s_cols[s_col_count].name, p, len);
		s_cols[s_col_count].name[len] = '\0';
		/* Trim leading whitespace */
		char *trim = s_cols[s_col_count].name;
		while (*trim && isspace((unsigned char)*trim)) trim++;
		if (trim != s_cols[s_col_count].name) {
			memmove(s_cols[s_col_count].name, trim, strlen(trim) + 1);
		}
		/* First column must be timestamp_ms — mark with sentinel index -2 so
		 * we know to parse it as the row time rather than inject. */
		if (s_col_count == 0) {
			if (strcmp(s_cols[0].name, "timestamp_ms") != 0) {
				ESP_LOGE(TAG, "First column is '%s', expected 'timestamp_ms'",
				         s_cols[0].name);
				return false;
			}
			s_cols[0].signal_index = -2;
		} else {
			s_cols[s_col_count].signal_index =
				signal_find_by_name(s_cols[s_col_count].name);
			if (s_cols[s_col_count].signal_index < 0) {
				ESP_LOGI(TAG, "Column '%s' has no matching signal — will skip",
				         s_cols[s_col_count].name);
			}
		}
		s_col_count++;
		if (!comma) break;
		p = comma + 1;
	}
	if (s_col_count < 2) {
		ESP_LOGE(TAG, "Need at least timestamp_ms + 1 signal column");
		return false;
	}
	return true;
}

/* Parse a data row's first column into a uint32_t (timestamp_ms). Returns
 * false if the line is malformed. Doesn't inject anything — that's done by
 * _emit_row. */
static bool _peek_row_ts(const char *line, uint32_t *out_ts)
{
	char buf[16];
	const char *comma = strchr(line, ',');
	if (!comma) return false;
	size_t len = (size_t)(comma - line);
	if (len == 0 || len >= sizeof(buf)) return false;
	memcpy(buf, line, len);
	buf[len] = '\0';
	char *end = NULL;
	unsigned long v = strtoul(buf, &end, 10);
	if (end == buf) return false;
	*out_ts = (uint32_t)v;
	return true;
}

/* Parse + inject all signal columns for one row. */
static void _emit_row(const char *line)
{
	const char *p = line;
	uint16_t col = 0;
	while (*p && col < s_col_count) {
		const char *comma = strchr(p, ',');
		size_t len = comma ? (size_t)(comma - p) : strlen(p);
		/* Skip column 0 (timestamp) and any column with no matching signal. */
		if (col > 0 && s_cols[col].signal_index >= 0 && len > 0) {
			char val_buf[32];
			if (len < sizeof(val_buf)) {
				memcpy(val_buf, p, len);
				val_buf[len] = '\0';
				char *end = NULL;
				float v = strtof(val_buf, &end);
				if (end != val_buf) {
					signal_inject_test_value(s_cols[col].name, v);
				}
			}
		}
		col++;
		if (!comma) break;
		p = comma + 1;
	}
}

/* Count data rows by walking the file. Cheap one-time cost on start. Restores
 * the file position to the first data row after counting. */
static uint32_t _count_data_rows(void)
{
	long start_pos = ftell(s_file);
	uint32_t n = 0;
	char buf[REPLAY_LINE_MAX];
	while (_read_line(buf, sizeof(buf))) {
		if (buf[0] != '\0') n++;
	}
	clearerr(s_file);
	fseek(s_file, start_pos, SEEK_SET);
	return n;
}

esp_err_t signal_replay_start(const char *path, float speed, bool loop)
{
	if (!path) return ESP_ERR_INVALID_ARG;

	/* Stop any existing replay first */
	_close();

	s_file = fopen(path, "r");
	if (!s_file) {
		ESP_LOGE(TAG, "fopen('%s') failed: errno=%d", path, errno);
		return ESP_FAIL;
	}
	strncpy(s_path, path, sizeof(s_path) - 1);
	s_path[sizeof(s_path) - 1] = '\0';

	/* Header */
	char line[REPLAY_LINE_MAX];
	if (!_read_line(line, sizeof(line)) || !_parse_header(line)) {
		_close();
		return ESP_ERR_INVALID_ARG;
	}

	/* Row count + capture first timestamp by peeking at the first data row */
	s_total_rows = _count_data_rows();
	if (s_total_rows == 0) {
		ESP_LOGW(TAG, "No data rows in '%s'", path);
		_close();
		return ESP_ERR_INVALID_SIZE;
	}
	if (!_read_line(s_pending_line, sizeof(s_pending_line)) ||
	    !_peek_row_ts(s_pending_line, &s_next_ts_ms)) {
		_close();
		return ESP_ERR_INVALID_RESPONSE;
	}
	s_first_ts_ms     = s_next_ts_ms;
	s_have_pending    = true;
	s_row_index       = 0;
	s_replay_start_us = (uint64_t)esp_timer_get_time();

	if (speed < 0.1f)   speed = 0.1f;
	if (speed > 100.0f) speed = 100.0f;
	s_speed = speed;
	s_loop  = loop;

	s_timer = lv_timer_create(_replay_timer_cb, REPLAY_TIMER_MS, NULL);
	ESP_LOGI(TAG, "Replay started: %s (%lu rows, %u cols, %.1fx, loop=%d)",
	         path, (unsigned long)s_total_rows, (unsigned)s_col_count,
	         (double)speed, loop ? 1 : 0);
	return ESP_OK;
}

void signal_replay_stop(void)
{
	if (!s_file) return;
	ESP_LOGI(TAG, "Replay stopped at row %lu/%lu",
	         (unsigned long)s_row_index, (unsigned long)s_total_rows);
	_close();
}

bool signal_replay_is_active(void)         { return s_file != NULL; }
uint32_t signal_replay_get_row(void)       { return s_file ? s_row_index : 0; }
uint32_t signal_replay_get_total_rows(void){ return s_file ? s_total_rows : 0; }
const char *signal_replay_get_file(void)   { return s_file ? s_path : ""; }
float signal_replay_get_speed(void)        { return s_speed; }

/* Worker: emit every row whose file-relative timestamp is <= the wall clock
 * elapsed since start (scaled by speed). Reads forward through the file as
 * needed. */
static void _replay_timer_cb(lv_timer_t *t)
{
	(void)t;
	if (!s_file) return;

	uint64_t elapsed_us  = (uint64_t)esp_timer_get_time() - s_replay_start_us;
	/* Map wall-clock elapsed time to virtual file time:
	 *   virtual_ms = first_ts_ms + (elapsed_ms * speed) */
	uint32_t virt_ms = s_first_ts_ms +
	                   (uint32_t)((elapsed_us / 1000ULL) * (uint64_t)(s_speed * 1000.0f) / 1000ULL);

	while (s_have_pending && s_next_ts_ms <= virt_ms) {
		_emit_row(s_pending_line);
		s_row_index++;

		/* Try to read the next row */
		if (!_read_line(s_pending_line, sizeof(s_pending_line)) ||
		    s_pending_line[0] == '\0') {
			s_have_pending = false;
		} else if (!_peek_row_ts(s_pending_line, &s_next_ts_ms)) {
			s_have_pending = false;
		}
	}

	/* End of file? Either loop or stop. */
	if (!s_have_pending) {
		if (s_loop) {
			/* Rewind past the header */
			rewind(s_file);
			char line[REPLAY_LINE_MAX];
			(void)_read_line(line, sizeof(line)); /* skip header */
			if (_read_line(s_pending_line, sizeof(s_pending_line)) &&
			    _peek_row_ts(s_pending_line, &s_next_ts_ms)) {
				s_first_ts_ms     = s_next_ts_ms;
				s_have_pending    = true;
				s_row_index       = 0;
				s_replay_start_us = (uint64_t)esp_timer_get_time();
			} else {
				ESP_LOGW(TAG, "Loop rewind failed; stopping");
				signal_replay_stop();
			}
		} else {
			signal_replay_stop();
		}
	}
}
