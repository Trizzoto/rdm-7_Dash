#include "remote_touch.h"
#include "screen_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "remote_touch";

/* ── Latched pointer state ─────────────────────────────────────────────── */

static SemaphoreHandle_t s_mux = NULL;
static bool     s_enabled           = false;
static bool     s_pressed           = false;
static bool     s_release_requested = false;  /* deferred release — see below */
static int16_t  s_x                 = 0;
static int16_t  s_y                 = 0;
static lv_indev_drv_t s_drv;

/* Deferred-release semantics:
 *
 * LVGL polls the indev read_cb at ~30 Hz. Remote touch arrives via HTTP at
 * whatever cadence the browser fires pointer events (often faster than
 * 30 Hz — e.g., a quick click is a down + up within 10 ms). Without any
 * buffering, a fast click race looks like:
 *     t=0:  remote_touch_set(pressed=true)   -> s_pressed=true
 *     t=5:  remote_touch_set(pressed=false)  -> s_pressed=false
 *     t=33: read_cb polls                    -> reports REL
 * LVGL never sees a PR state and no click fires. The web interface feels dead.
 *
 * Fix: on `pressed=false`, just mark `s_release_requested` — don't clear
 * s_pressed. read_cb reports PR once (satisfying LVGL's press detection),
 * THEN flips s_pressed=false so the next poll reports REL. LVGL always
 * sees the full PR→REL transition and fires PRESSED + RELEASED cleanly. */

/* ── Helpers ───────────────────────────────────────────────────────────── */

static inline int16_t _clamp(int16_t v, int16_t lo, int16_t hi) {
	if (v < lo) return lo;
	if (v > hi) return hi;
	return v;
}

/* LVGL indev read callback — runs on the LVGL task. Return the latched
 * state; if remote control is disabled, always report "released" so a
 * stale press doesn't strand the UI in pressed state. */
static void _remote_touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
	(void)drv;
	if (xSemaphoreTake(s_mux, 0) == pdTRUE) {
		if (s_enabled && s_pressed) {
			data->state  = LV_INDEV_STATE_PR;
			data->point.x = s_x;
			data->point.y = s_y;
			/* If the client has already released, LVGL has now SEEN one
			 * PR poll — safe to apply the release on the next read. */
			if (s_release_requested) {
				s_pressed           = false;
				s_release_requested = false;
			}
		} else {
			data->state  = LV_INDEV_STATE_REL;
			data->point.x = s_x;  /* keep last coord so LVGL sees a clean release */
			data->point.y = s_y;
		}
		xSemaphoreGive(s_mux);
	} else {
		data->state = LV_INDEV_STATE_REL;
	}
}

/* ── Public API ────────────────────────────────────────────────────────── */

void remote_touch_init(lv_disp_t *disp) {
	if (s_mux) return;  /* already initialised */

	s_mux = xSemaphoreCreateMutex();
	if (!s_mux) {
		ESP_LOGE(TAG, "mutex create failed — remote touch disabled");
		return;
	}

	lv_indev_drv_init(&s_drv);
	s_drv.type    = LV_INDEV_TYPE_POINTER;
	s_drv.disp    = disp;
	s_drv.read_cb = _remote_touch_read_cb;
	lv_indev_drv_register(&s_drv);

	ESP_LOGI(TAG, "Virtual input device registered (coexists with GT911)");
}

void remote_touch_set(int16_t x, int16_t y, bool pressed) {
	if (!s_mux) return;
	x = _clamp(x, 0, SCREEN_W - 1);
	y = _clamp(y, 0, SCREEN_H - 1);
	if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
		s_x = x;
		s_y = y;
		if (pressed) {
			/* New press (or move while held) — latch immediately and clear
			 * any deferred release so rapid down→move→up sequences don't
			 * drop the press. */
			s_pressed           = true;
			s_release_requested = false;
		} else {
			/* Release: don't clear s_pressed yet. read_cb will report PR
			 * once (guaranteeing LVGL sees the press) and then flip to
			 * REL on the subsequent poll. */
			s_release_requested = true;
		}
		xSemaphoreGive(s_mux);
	}
}

void remote_touch_set_enabled(bool on) {
	if (!s_mux) return;
	if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
		if (s_enabled != on) ESP_LOGI(TAG, "Remote touch %s", on ? "enabled" : "disabled");
		s_enabled = on;
		if (!on) {
			s_pressed           = false;  /* force release when disabling */
			s_release_requested = false;
		}
		xSemaphoreGive(s_mux);
	}
}

bool remote_touch_is_enabled(void) {
	bool v = false;
	if (s_mux && xSemaphoreTake(s_mux, 0) == pdTRUE) {
		v = s_enabled;
		xSemaphoreGive(s_mux);
	}
	return v;
}
