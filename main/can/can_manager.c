/*
 * can_manager.c — TWAI peripheral lifecycle and CAN receive task.
 *
 * Owns all TWAI hardware configuration, the acceptance filter, and the
 * simplified receive task that enqueues frames for the LVGL task to
 * process via signal_dispatch_frame().
 */
#include "can_manager.h"
#include "can_id_tracker.h"
#include "obd2.h"
#include "signal.h"
#include "signal_sim.h"

#include "driver/twai.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "storage/config_store.h"
#include "storage/can_raw_logger.h"


static const char *TAG = "CAN_MGR";

/* ── TWAI hardware configuration ────────────────────────────────────── */

static twai_timing_config_t g_t_config = TWAI_TIMING_CONFIG_500KBITS();
static twai_general_config_t g_config =
	TWAI_GENERAL_CONFIG_DEFAULT(20, 19, TWAI_MODE_NORMAL);
static twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

/* Current live bitrate index (0=125k,1=250k,2=500k,3=1M). Tracks what's
 * actually installed so callers (e.g. the OBD2 auto-search scan) can read it
 * and restore it. */
static uint8_t g_bitrate_index = 2;

/* True iff the wizard's ECU auto-detect probe (or other diagnostic) has
 * forced the filter to ACCEPT_ALL via can_set_promiscuous_mode. While
 * true, reconfigure_can_filter() becomes a no-op so signal binds in
 * other UIs (channel binds, OBD2 PID toggles) can't narrow the filter
 * underneath an in-flight probe. */
static bool s_promiscuous_active = false;

/* OBD2 29-bit (extended addressing) mode. When true the acceptance
 * filter is built ACCEPT_ALL so 0x18DAF1xx responses reach dispatch.
 * Loaded from NVS at can_init (a scan that locked a 29-bit car persists
 * it); toggled transiently by the OBD2 auto-search scan. */
static bool s_obd_extended = false;

/* Tracks whether the TWAI driver is currently installed (and the RX path
 * is meant to be live). Used by reconfigure_can_filter()'s equality
 * guard: a dead/uninstalled driver must NEVER be skipped just because
 * f_config happens to match the rebuilt filter (see FIX #6 — filter
 * wedge). Set true after every successful install+start, cleared on
 * uninstall/suspend. */
static bool s_driver_installed = false;

/* ── Receive task management ─────────────────────────────────────────── */

static TaskHandle_t canTaskHandle = NULL;
static volatile bool can_task_should_stop = false;
static volatile bool s_can_task_running = false;
static volatile uint32_t s_last_rx_can_id = 0;
static volatile uint32_t s_rx_frame_count = 0;
static volatile bool     s_suspended = false;

#define CAN_TASK_PRIORITY 7

/* Queue used to hand CAN frames from the TWAI RX task to the LVGL thread.
 * The LVGL thread drains this via can_process_queued_frames(), ensuring
 * that all widget/UI work happens on a single thread while the RX loop
 * stays completely non-blocking. */
static QueueHandle_t s_can_queue = NULL;

/* LVGL mutex helpers — defined in main.c, declared in ui/lvgl_helpers.h */
extern bool rdm_lvgl_lock(int timeout_ms);
extern void rdm_lvgl_unlock(void);

/* ── CAN receive task ─────────────────────────────────────────────────
 *
 * Simplified: receive one frame and enqueue it for the LVGL task to process.
 * The task never touches LVGL directly — it only writes to s_can_queue.
 */
/* CAN bus-off recovery limits */
#define CAN_RECOVERY_MAX_RETRIES    10
#define CAN_RECOVERY_INITIAL_MS     100
#define CAN_RECOVERY_MAX_MS         5000

/* Bus-off recovery: how long to wait for the driver to leave RECOVERING
 * (the ISR moves it to STOPPED on bus-recovery-complete) before we give
 * up this attempt. ~1 s at 5 ms granularity. */
#define CAN_BUSOFF_WAIT_MAX_MS      1000
#define CAN_BUSOFF_WAIT_STEP_MS     5

/* Map a twai_state_t to a short human string for the diagnostic log. */
static const char *_twai_state_name(twai_state_t s) {
	switch (s) {
	case TWAI_STATE_STOPPED:    return "STOPPED";
	case TWAI_STATE_RUNNING:    return "RUNNING";
	case TWAI_STATE_BUS_OFF:    return "BUS_OFF";
	case TWAI_STATE_RECOVERING: return "RECOVERING";
	default:                    return "UNKNOWN";
	}
}

/* FIX #1 — bus-off recovery. twai_initiate_recovery() is the ONLY API that
 * exits the BUS_OFF state: it requires state==BUS_OFF, transitions the
 * driver to RECOVERING, and the driver ISR sets state to STOPPED once
 * bus-recovery completes; twai_start() from STOPPED is then legal.
 *
 * On a quiet bus in NORMAL mode the TX error counter climbs past 256 and
 * the controller latches BUS_OFF. In that state twai_receive() returns
 * ESP_ERR_TIMEOUT (NOT INVALID_STATE), so the legacy recovery branch never
 * fired and CAN stayed dead until a full driver reinstall. This helper is
 * the shared sequence used by both the RX task and can_recover().
 *
 * @param info  current status info (already fetched, state==BUS_OFF).
 * @return true if the driver was brought back to RUNNING, false otherwise.
 *
 * Must be called from a context that may block briefly (RX task /
 * diagnostics tick). Respects can_task_should_stop so a concurrent stop
 * request aborts the wait promptly. */
static bool _recover_from_bus_off(const twai_status_info_t *info) {
	ESP_LOGW(TAG, "CAN BUS_OFF detected (tx_err=%lu) — initiating recovery",
			 (unsigned long)info->tx_error_counter);

	esp_err_t rec = twai_initiate_recovery();
	if (rec != ESP_OK) {
		ESP_LOGE(TAG, "twai_initiate_recovery failed: %s", esp_err_to_name(rec));
		return false;
	}

	/* Wait (bounded) for the ISR to finish bus recovery and move us to
	 * STOPPED. Do NOT busy-spin: yield 5 ms per iteration and bail early
	 * if a stop was requested. */
	int waited_ms = 0;
	twai_status_info_t st;
	while (waited_ms < CAN_BUSOFF_WAIT_MAX_MS) {
		if (can_task_should_stop) {
			ESP_LOGW(TAG, "Bus-off recovery aborted — stop requested");
			return false;
		}
		vTaskDelay(pdMS_TO_TICKS(CAN_BUSOFF_WAIT_STEP_MS));
		waited_ms += CAN_BUSOFF_WAIT_STEP_MS;
		if (twai_get_status_info(&st) == ESP_OK &&
		    st.state == TWAI_STATE_STOPPED) {
			break;
		}
	}

	if (twai_get_status_info(&st) != ESP_OK || st.state != TWAI_STATE_STOPPED) {
		ESP_LOGE(TAG, "Bus-off recovery timed out after %d ms (state=%s)",
				 waited_ms,
				 (twai_get_status_info(&st) == ESP_OK)
				     ? _twai_state_name(st.state) : "QUERY_FAIL");
		return false;
	}

	esp_err_t serr = twai_start();
	if (serr != ESP_OK) {
		ESP_LOGE(TAG, "Bus-off recovery: twai_start failed: %s",
				 esp_err_to_name(serr));
		return false;
	}
	ESP_LOGI(TAG, "CAN bus-off recovery complete — RUNNING again");
	return true;
}

static void can_receive_task(void *pvParameter) {
	(void)pvParameter;
	static uint32_t s_queue_drop_count = 0;
	int recovery_retries = 0;
	uint32_t recovery_delay_ms = CAN_RECOVERY_INITIAL_MS;

	/* FIX #1 — throttle bus-off recovery to ~once/second so a hard-down
	 * bus doesn't hammer twai_initiate_recovery() in a tight loop. */
	int64_t last_busoff_attempt_us = 0;

	/* INSTRUMENTATION (D) — low-rate state log. Track the last-logged TWAI
	 * state so we emit on every transition, plus a heartbeat every ~5 s so
	 * we can see bus-off / error-counter climb on-device without spamming.*/
	twai_state_t last_logged_state = (twai_state_t)0xFF; /* impossible → forces first log */
	int64_t last_state_log_us = 0;

	s_can_task_running = true;

	while (!can_task_should_stop) {
		twai_message_t message;
		esp_err_t ret = twai_receive(&message, pdMS_TO_TICKS(5));

		/* INSTRUMENTATION (D): cheap state snapshot. Log on state change OR
		 * every ~5 s. twai_get_status_info() is a lightweight register read;
		 * we only call the logger when something is worth printing. */
		{
			twai_status_info_t dbg;
			if (twai_get_status_info(&dbg) == ESP_OK) {
				int64_t now_us = esp_timer_get_time();
				bool changed = (dbg.state != last_logged_state);
				/* State transitions are worth seeing at INFO; the periodic
				 * heartbeat is steady-state noise once a unit is known healthy,
				 * so it goes to DEBUG and only every 60 s (was INFO every 5 s). */
				bool heartbeat = (now_us - last_state_log_us) >= 60LL * 1000 * 1000;
				if (changed) {
					ESP_LOGI(TAG,
						"TWAI state=%s tx_err=%lu rx_err=%lu rx_frames=%lu (transition)",
						_twai_state_name(dbg.state),
						(unsigned long)dbg.tx_error_counter,
						(unsigned long)dbg.rx_error_counter,
						(unsigned long)s_rx_frame_count);
					last_logged_state = dbg.state;
					last_state_log_us = now_us;
				} else if (heartbeat) {
					ESP_LOGD(TAG,
						"TWAI state=%s tx_err=%lu rx_err=%lu rx_frames=%lu",
						_twai_state_name(dbg.state),
						(unsigned long)dbg.tx_error_counter,
						(unsigned long)dbg.rx_error_counter,
						(unsigned long)s_rx_frame_count);
					last_state_log_us = now_us;
				}
			}
		}

		if (ret == ESP_OK) {
			/* Successful receive resets recovery state */
			recovery_retries = 0;
			recovery_delay_ms = CAN_RECOVERY_INITIAL_MS;

			s_last_rx_can_id = message.identifier;
			s_rx_frame_count++;
			if (s_can_queue != NULL) {
				/* Non-blocking enqueue; drop oldest frames if the queue is
				 * momentarily full rather than stalling the RX loop. */
				if (xQueueSendToBack(s_can_queue, &message, 0) != pdPASS) {
					s_queue_drop_count++;
					if ((s_queue_drop_count % 10000) == 1) {
						ESP_LOGW(TAG, "CAN queue overflow (total drops: %lu)", (unsigned long)s_queue_drop_count);
					}
				}
			}
		} else if (ret == ESP_ERR_TIMEOUT) {
			/* FIX #1 — bus-off detection. In NORMAL mode on a quiet bus the
			 * TX error counter climbs past 256 and the controller latches
			 * TWAI_STATE_BUS_OFF. In that state twai_receive() keeps
			 * returning ESP_ERR_TIMEOUT (not INVALID_STATE), so this is the
			 * ONLY place we can notice it. Poll status; if bus-off, run the
			 * recovery sequence (throttled to ~once/sec). */
			twai_status_info_t info;
			if (twai_get_status_info(&info) == ESP_OK &&
			    info.state == TWAI_STATE_BUS_OFF) {
				int64_t now_us = esp_timer_get_time();
				if (now_us - last_busoff_attempt_us >= 1LL * 1000 * 1000) {
					last_busoff_attempt_us = now_us;
					_recover_from_bus_off(&info);
					/* Force a fresh state log on next loop iteration so the
					 * RUNNING/STILL-BUS_OFF transition is visible on-device. */
					last_logged_state = (twai_state_t)0xFF;
				} else {
					vTaskDelay(pdMS_TO_TICKS(CAN_BUSOFF_WAIT_STEP_MS));
				}
			} else {
				vTaskDelay(pdMS_TO_TICKS(1));
			}
		} else if (ret == ESP_ERR_INVALID_STATE) {
			/* Intentional stop: _stop_can_task() calls twai_stop() which
			 * makes twai_receive() return INVALID_STATE. Exit before the
			 * recovery path calls twai_start() — that would race with
			 * can_suspend()'s twai_driver_uninstall(). */
			if (can_task_should_stop) break;
			if (recovery_retries >= CAN_RECOVERY_MAX_RETRIES) {
				ESP_LOGE(TAG, "CAN recovery failed after %d retries, giving up",
						 CAN_RECOVERY_MAX_RETRIES);
				/* Sleep for 5 seconds before allowing retries again */
				vTaskDelay(pdMS_TO_TICKS(CAN_RECOVERY_MAX_MS));
				recovery_retries = 0;
				recovery_delay_ms = CAN_RECOVERY_INITIAL_MS;
				continue;
			}
			ESP_LOGW(TAG, "CAN bus error, recovery attempt %d/%d (delay %lu ms)",
					 recovery_retries + 1, CAN_RECOVERY_MAX_RETRIES,
					 (unsigned long)recovery_delay_ms);
			twai_stop();
			vTaskDelay(pdMS_TO_TICKS(recovery_delay_ms));
			esp_err_t start_err = twai_start();
			if (start_err != ESP_OK) {
				ESP_LOGE(TAG, "CAN recovery failed: %s", esp_err_to_name(start_err));
			}
			recovery_retries++;
			/* Exponential backoff: double delay, cap at max */
			recovery_delay_ms *= 2;
			if (recovery_delay_ms > CAN_RECOVERY_MAX_MS) {
				recovery_delay_ms = CAN_RECOVERY_MAX_MS;
			}
		} else {
			ESP_LOGW(TAG, "CAN receive error: %s", esp_err_to_name(ret));
			vTaskDelay(pdMS_TO_TICKS(1));
		}
	}

	ESP_LOGI(TAG, "CAN task exited gracefully");
	s_can_task_running = false;
	/* Created via *WithCaps (PSRAM stack) — must be torn down with the matching
	 * deleter or the PSRAM stack + TCB leak on every self-exit. */
	vTaskDeleteWithCaps(NULL);
}

/* Spawn the CAN RX task. PSRAM stack keeps the scarce internal SRAM free, and
 * pinning to core 0 holds the documented CAN-on-core-0 / LVGL-on-core-1 split
 * (the previous WithCaps recreations were unpinned, so the task drifted to
 * core 1 after the first filter rebuild). All create sites route through here
 * so they can't diverge; created *WithCaps, so EVERY teardown must use
 * vTaskDeleteWithCaps(). */
static BaseType_t _spawn_can_rx_task(void) {
	return xTaskCreatePinnedToCoreWithCaps(
		can_receive_task, "can_receive_task", 4096, NULL,
		CAN_TASK_PRIORITY, &canTaskHandle, 0 /*core*/, MALLOC_CAP_SPIRAM);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void build_twai_filter_from_signals(twai_filter_config_t *out_filter) {
	/* 29-bit OBD2 mode: the single-filter mask built below only matches
	 * standard frames; extended responses (0x18DAF1xx) would be dropped in
	 * hardware. Stay wide open — software dispatch filters per-frame. */
	if (s_obd_extended) {
		*out_filter = (twai_filter_config_t)TWAI_FILTER_CONFIG_ACCEPT_ALL();
		return;
	}
	uint16_t sig_count = signal_get_count();
	if (sig_count == 0) {
		*out_filter = (twai_filter_config_t)TWAI_FILTER_CONFIG_ACCEPT_ALL();
		return;
	}

	/* Collect unique CAN IDs from the signal registry.
	 * 64 entries is plenty — standard CAN uses 11-bit IDs and typical
	 * layouts have far fewer unique IDs than signals. */
	uint32_t ids[64];
	int count = 0;

	for (uint16_t i = 0; i < sig_count; i++) {
		signal_t *sig = signal_get_by_index(i);
		if (!sig || sig->can_id == 0)
			continue;
		/* Extended (29-bit) IDs don't fit the standard-frame acceptance filter
		 * built below, and masking them to 11 bits would filter out their real
		 * frames entirely. Fall back to accept-all — software dispatch already
		 * matches on the full ID, so only the hardware pre-filter is skipped. */
		if (sig->can_id > 0x7FFu) {
			*out_filter = (twai_filter_config_t)TWAI_FILTER_CONFIG_ACCEPT_ALL();
			return;
		}
		uint32_t sid = sig->can_id & 0x7FFu;
		/* Deduplicate */
		bool dup = false;
		for (int j = 0; j < count; j++) {
			if (ids[j] == sid) { dup = true; break; }
		}
		if (!dup) {
			if (count >= 64) {
				ESP_LOGW(TAG, "Too many unique CAN IDs, accepting all");
				*out_filter = (twai_filter_config_t)TWAI_FILTER_CONFIG_ACCEPT_ALL();
				return;
			}
			ids[count++] = sid;
		}
	}

	/* Always include the OBD2 response range 0x7E8..0x7EF in the filter.
	 * One-shot diagnostic endpoints (DTC read/clear, VIN, ECU name) and the
	 * background DTC monitor can fire at any time — even when no PIDs are
	 * being polled (e.g. user is on a native broadcast preset like FG or
	 * BA/BF). Previously this was gated on obd2_is_running(), which left
	 * responses filtered out for those callers and made every diagnostic
	 * UI silently time out. Software paths in obd2_rx_handler / signal
	 * dispatch already filter unwanted frames, so widening the hardware
	 * mask costs nothing functionally. */
	for (uint32_t id = OBD2_RESPONSE_ID_FIRST; id <= OBD2_RESPONSE_ID_LAST; id++) {
		if (count >= 64) break;
		bool dup = false;
		for (int j = 0; j < count; j++) {
			if (ids[j] == id) { dup = true; break; }
		}
		if (!dup) ids[count++] = id;
	}

	if (count == 0) {
		*out_filter = (twai_filter_config_t)TWAI_FILTER_CONFIG_ACCEPT_ALL();
		return;
	}

	uint32_t ref = ids[0];
	uint32_t varying = 0;
	for (int i = 1; i < count; i++)
		varying |= (ref ^ ids[i]);

	uint32_t compare_bits = (~varying) & 0x7FFu;
	uint32_t code_bits = ref & compare_bits;

	out_filter->acceptance_code = code_bits << 21;
	out_filter->acceptance_mask = 0xFFFFFFFFu & ~(compare_bits << 21);
	out_filter->single_filter = true;
}

/** Stop the CAN receive task gracefully.  Sets the stop flag first so the
 *  task checks it on next iteration, then halts the TWAI peripheral so
 *  twai_receive() unblocks immediately.  Waits for the task to signal exit
 *  via s_can_task_running rather than calling eTaskGetState() on a
 *  potentially deleted handle. */
static void _stop_can_task(void) {
	if (canTaskHandle == NULL) return;

	/* Set stop flag BEFORE twai_stop() so the task exits cleanly on next
	 * loop iteration instead of entering the error recovery path. */
	can_task_should_stop = true;

	/* Stop TWAI so twai_receive() unblocks with ESP_ERR_INVALID_STATE */
	twai_stop();

	/* Wait up to 500 ms for graceful exit */
	for (int i = 0; i < 50; i++) {
		if (!s_can_task_running)
			break;
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	if (s_can_task_running) {
		ESP_LOGW(TAG, "CAN task did not exit gracefully, force-deleting");
		vTaskDeleteWithCaps(canTaskHandle);
		s_can_task_running = false;
	}
	canTaskHandle = NULL;
	can_task_should_stop = false;
}

/* Install the TWAI driver with retry. The peripheral sometimes needs more
 * than the typical 50-100 ms quiet window to fully release after an
 * uninstall before it'll accept a fresh install — single-shot install
 * intermittently returns ESP_ERR_INVALID_STATE and leaves CAN dead until
 * reboot. Centralised so every install path (boot, filter rebuild,
 * bitrate change, resume, recovery) gets the same backoff behaviour.
 *
 * 3 attempts with exponential delays: 200, 400, 600 ms between retries.
 * Each retry uninstalls again to clear any half-state from the failed
 * attempt — without this the peripheral can latch into a partial-install
 * mode that no amount of waiting clears.
 *
 * Heap snapshot logged on each failure: the TWAI driver allocates ~4-6 KB
 * of internal SRAM at install time (queue + ISR + driver object). When
 * internal SRAM is fragmented or exhausted, the driver's allocation
 * path can report ESP_ERR_INVALID_STATE rather than ESP_ERR_NO_MEM,
 * making it look like a state bug. Logging free heap pinpoints which.
 *
 * @param ctx  short context string for logging (e.g. "filter rebuild")
 * @return     ESP_OK on success, last error code if all attempts fail.
 */
static esp_err_t _install_twai_with_retry(const char *ctx) {
	esp_err_t err = ESP_FAIL;
	for (int attempt = 0; attempt < 3; attempt++) {
		err = twai_driver_install(&g_config, &g_t_config, &f_config);
		if (err == ESP_OK) return ESP_OK;

		size_t free_int  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
		size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
		size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
		ESP_LOGW(TAG, "%s: install attempt %d/3 failed: %s (0x%04x) — "
				 "free int=%u 8bit=%u largest_int=%u",
				 ctx, attempt + 1, esp_err_to_name(err), (unsigned)err,
				 (unsigned)free_int, (unsigned)free_8bit, (unsigned)largest_int);
		twai_driver_uninstall();
		vTaskDelay(pdMS_TO_TICKS(200 * (attempt + 1)));
	}
	ESP_LOGE(TAG, "%s: twai_driver_install failed after retries: %s",
			 ctx, esp_err_to_name(err));
	return err;
}

/* Compare two TWAI acceptance filters bit-for-bit. Returns true if they
 * would deliver the exact same set of frames. Used to short-circuit
 * reconfigure_can_filter() when the rebuilt filter matches what's
 * already installed — most widget preset changes don't actually change
 * the bus filter (e.g. binding a gauge to RPM vs MAP when both already
 * pass through). Skipping the teardown saves the TWAI bounce AND the
 * memory churn that triggers ESP_ERR_INVALID_STATE on heap-tight builds. */
static bool _twai_filter_equal(const twai_filter_config_t *a,
								const twai_filter_config_t *b) {
	if (!a || !b) return false;
	return a->acceptance_code == b->acceptance_code &&
	       a->acceptance_mask == b->acceptance_mask &&
	       a->single_filter   == b->single_filter;
}

/** Map a bitrate index (0-3) to the corresponding TWAI timing config. */
static twai_timing_config_t _bitrate_to_timing(uint8_t bitrate_code) {
	switch (bitrate_code) {
	case 0:  return (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
	case 1:  return (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
	case 3:  return (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
	default: return (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
	}
}

void reconfigure_can_filter(void) {
	/* While the bus scan owns the TWAI peripheral (can_suspend), ANY
	 * filter rebuild here would reinstall the normal driver + respawn the
	 * RX task underneath the scan's own install/uninstall cycles. The two
	 * then fight over the peripheral: the scan's installs fail ("CAN
	 * driver busy" on the wizard) and the rogue RX task spams
	 * twai_start() from its recovery loop. Triggers are easy to hit
	 * mid-scan — wizard teardown, menu save, a web-editor channel edit.
	 * Defer: can_resume() rebuilds the filter from the live registry when
	 * the scan finishes, so nothing is lost. */
	if (s_suspended) {
		ESP_LOGI(TAG, "Filter rebuild deferred — CAN suspended (bus scan owns the peripheral)");
		return;
	}
	/* While the wizard's ECU probe is open, the filter must stay
	 * promiscuous — refuse to narrow it just because some unrelated UI
	 * touched the signal registry mid-probe. The probe's own
	 * can_set_promiscuous_mode(false) call rebuilds the filter when
	 * the user commits a preset. */
	if (s_promiscuous_active) {
		ESP_LOGI(TAG, "Filter rebuild skipped — promiscuous mode active");
		return;
	}
	/* Build the new filter first WITHOUT touching the live one. If the
	 * resulting filter would deliver the same set of frames as what's
	 * already installed, skip the entire teardown/reinstall — saves a
	 * ~600 ms TWAI bounce AND avoids the heap churn that triggers
	 * ESP_ERR_INVALID_STATE on memory-tight builds. Most widget preset
	 * changes don't actually change the bus filter (e.g. rebinding a
	 * gauge between two signals that share a CAN ID, or switching among
	 * OBD2 PIDs while the 0x7E8-0x7EF response range stays mapped). */
	twai_filter_config_t new_filter;
	build_twai_filter_from_signals(&new_filter);

	/* FIX #6 — filter wedge. Only take the equality short-circuit if the
	 * driver is genuinely installed AND running. A previous failed install
	 * could have left the driver uninstalled while f_config still claims
	 * the (narrow) filter that never got applied; skipping here would leave
	 * CAN permanently dead. Confirm RUNNING via the live status before we
	 * trust f_config as "what's actually installed". */
	if (_twai_filter_equal(&new_filter, &f_config)) {
		twai_status_info_t info;
		bool live_running = s_driver_installed &&
		                    twai_get_status_info(&info) == ESP_OK &&
		                    info.state == TWAI_STATE_RUNNING;
		if (live_running) {
			ESP_LOGI(TAG, "Filter rebuild: unchanged (code=0x%08X mask=0x%08X) — skipping",
					 (unsigned)new_filter.acceptance_code,
					 (unsigned)new_filter.acceptance_mask);
			return;
		}
		ESP_LOGW(TAG, "Filter rebuild: filter matches but driver not RUNNING "
				 "— forcing full reinstall");
	}

	_stop_can_task();

	vTaskDelay(pdMS_TO_TICKS(50));
	twai_driver_uninstall();
	s_driver_installed = false;
	vTaskDelay(pdMS_TO_TICKS(50));
	/* FIX #6 — do NOT commit f_config=new_filter until the install+start
	 * actually succeed. If the install fails we leave f_config describing
	 * the previously-installed filter so the next call's equality guard
	 * can't see new_filter and wrongly skip the retry. */
	twai_filter_config_t prev_filter = f_config;
	f_config = new_filter;
	if (_install_twai_with_retry("filter rebuild") != ESP_OK) {
		f_config = prev_filter;  /* roll back so equality guard won't wedge */
		return;
	}
	if (twai_start() != ESP_OK) {
		ESP_LOGE(TAG, "Filter rebuild: twai_start failed");
		twai_driver_uninstall();
		f_config = prev_filter;  /* roll back so equality guard won't wedge */
		return;
	}
	s_driver_installed = true;
	vTaskDelay(pdMS_TO_TICKS(50));

	_spawn_can_rx_task();
}

bool can_is_promiscuous(void) { return s_promiscuous_active; }

void can_set_promiscuous_mode(bool enable) {
	if (enable == s_promiscuous_active) return;

	/* Scan owns the peripheral — record the request and let can_resume()
	 * install the matching filter when the scan hands CAN back. Touching
	 * the driver here mid-scan causes the same install fight as
	 * reconfigure_can_filter (see comment there). */
	if (s_suspended) {
		s_promiscuous_active = enable;
		ESP_LOGI(TAG, "Promiscuous %s deferred — CAN suspended (applied on resume)",
		         enable ? "on" : "off");
		return;
	}

	twai_filter_config_t new_filter;
	if (enable) {
		new_filter = (twai_filter_config_t)TWAI_FILTER_CONFIG_ACCEPT_ALL();
	} else {
		build_twai_filter_from_signals(&new_filter);
	}
	/* FIX #6 — only short-circuit on filter equality if the driver is truly
	 * installed AND running; otherwise a half-torn-down driver would be
	 * skipped and CAN would stay dead. */
	if (_twai_filter_equal(&new_filter, &f_config)) {
		twai_status_info_t info;
		bool live_running = s_driver_installed &&
		                    twai_get_status_info(&info) == ESP_OK &&
		                    info.state == TWAI_STATE_RUNNING;
		if (live_running) {
			s_promiscuous_active = enable;
			ESP_LOGI(TAG, "Promiscuous %s — filter already matches, no reinstall",
				 enable ? "on" : "off");
			return;
		}
		ESP_LOGW(TAG, "Promiscuous %s — filter matches but driver not RUNNING, "
			 "forcing full reinstall", enable ? "on" : "off");
	}

	_stop_can_task();

	vTaskDelay(pdMS_TO_TICKS(50));
	twai_driver_uninstall();
	s_driver_installed = false;
	vTaskDelay(pdMS_TO_TICKS(50));
	/* FIX #6 — defer the f_config commit until install+start succeed so a
	 * failed install can't wedge the equality guard on the next call. */
	twai_filter_config_t prev_filter = f_config;
	f_config = new_filter;
	if (_install_twai_with_retry(enable ? "promiscuous on" :
	                                       "promiscuous off") != ESP_OK) {
		ESP_LOGE(TAG, "Promiscuous swap install failed");
		f_config = prev_filter;  /* roll back so equality guard won't wedge */
		return;
	}
	if (twai_start() != ESP_OK) {
		ESP_LOGE(TAG, "Promiscuous swap start failed");
		twai_driver_uninstall();
		f_config = prev_filter;  /* roll back so equality guard won't wedge */
		return;
	}
	s_driver_installed = true;
	vTaskDelay(pdMS_TO_TICKS(50));

	_spawn_can_rx_task();

	s_promiscuous_active = enable;
	ESP_LOGI(TAG, "Promiscuous mode %s", enable ? "ENABLED" : "disabled");
}

void can_init(void) {
	/* Load saved bitrate from NVS (default index 2 = 500 kbps) */
	uint8_t saved_bitrate = 2;
	config_store_load_bitrate(&saved_bitrate);
	g_bitrate_index = saved_bitrate;

	/* OBD2 addressing mode persisted by a previous auto-search scan lock.
	 * Must load BEFORE the filter is built below so a 29-bit car's
	 * responses aren't hardware-filtered on the first boot after a scan. */
	uint8_t obd_ext = 0;
	config_store_load_obd_extended(&obd_ext);
	s_obd_extended = (obd_ext != 0);
	if (s_obd_extended) {
		ESP_LOGI(TAG, "OBD2 29-bit addressing active (persisted)");
	}

	g_t_config = _bitrate_to_timing(saved_bitrate);
	static const char *bitrate_labels[] = {"125 kbps", "250 kbps", "500 kbps", "1 Mbps"};
	ESP_LOGI(TAG, "CAN bitrate: %s", bitrate_labels[saved_bitrate > 3 ? 2 : saved_bitrate]);

	build_twai_filter_from_signals(&f_config);

	if (_install_twai_with_retry("init") == ESP_OK) {
		s_driver_installed = true;
		ESP_LOGI(TAG, "TWAI driver installed");
	}

	/* Do NOT call twai_start() here — the RX task is not running yet.
	 * If CAN bus traffic arrives before the task drains the HW FIFO,
	 * the TWAI ISR spins and triggers the interrupt watchdog.
	 * twai_start() is called in can_start_task() instead. */

	/* Create the CAN frame queue once the driver is up.  64 entries keeps
	 * RAM usage modest while providing enough buffer. */
	if (s_can_queue == NULL) {
		s_can_queue = xQueueCreate(64, sizeof(twai_message_t));
		if (s_can_queue == NULL) {
			ESP_LOGE(TAG, "Failed to create CAN RX queue");
		}
	}
}

void can_start_task(void) {
	/* Start TWAI peripheral just before the RX task so there is no window
	 * where interrupts fire without anything draining the hardware FIFO. */
	if (twai_start() == ESP_OK)
		ESP_LOGI(TAG, "TWAI started");
	else
		ESP_LOGE(TAG, "TWAI start failed");

	_spawn_can_rx_task();
	ESP_LOGI(TAG, "CAN receive task started");
}

UBaseType_t can_task_get_priority(void) {
	if (canTaskHandle == NULL)
		return tskIDLE_PRIORITY;
	return uxTaskPriorityGet(canTaskHandle);
}

void can_task_set_priority(UBaseType_t priority) {
	if (canTaskHandle != NULL)
		vTaskPrioritySet(canTaskHandle, priority);
}

void can_change_bitrate(uint8_t bitrate_index) {
	/* Scan owns the peripheral — don't bounce the driver underneath it.
	 * can_resume() reloads the saved bitrate from NVS (callers persist it
	 * before calling here), so the new rate applies when the scan ends. */
	if (s_suspended) {
		ESP_LOGI(TAG, "Bitrate change deferred — CAN suspended (applied on resume)");
		return;
	}
	_stop_can_task();

	/* Apply new timing config */
	g_t_config = _bitrate_to_timing(bitrate_index);
	g_bitrate_index = bitrate_index;
	static const char *bitrate_labels[] = {"125 kbps", "250 kbps", "500 kbps", "1 Mbps"};
	ESP_LOGI(TAG, "CAN bitrate: %s", bitrate_labels[bitrate_index > 3 ? 2 : bitrate_index]);

	/* Uninstall, rebuild filter, reinstall (TWAI already stopped by _stop_can_task) */
	vTaskDelay(pdMS_TO_TICKS(50));
	twai_driver_uninstall();
	s_driver_installed = false;
	vTaskDelay(pdMS_TO_TICKS(50));

	/* FIX #6 pattern (see reconfigure_can_filter) — don't commit f_config until
	 * install+start actually succeed. This function used to skip that guard
	 * (only reconfigure_can_filter/can_set_promiscuous_mode had it), leaving
	 * f_config holding a filter that was never actually applied if either
	 * step below failed. */
	twai_filter_config_t prev_filter = f_config;
	build_twai_filter_from_signals(&f_config);

	if (_install_twai_with_retry("bitrate change") != ESP_OK) {
		f_config = prev_filter;  /* roll back so equality guard won't wedge */
		return;
	}
	vTaskDelay(pdMS_TO_TICKS(50));

	if (twai_start() != ESP_OK) {
		ESP_LOGE(TAG, "TWAI start failed after bitrate change");
		twai_driver_uninstall();
		f_config = prev_filter;  /* roll back so equality guard won't wedge */
		return;
	}
	s_driver_installed = true;
	vTaskDelay(pdMS_TO_TICKS(50));

	/* Recreate receive task — PSRAM stack avoids internal-SRAM OOM after WiFi init */
	if (_spawn_can_rx_task() != pdPASS) {
		ESP_LOGE(TAG, "Failed to create CAN task after bitrate change");
		canTaskHandle = NULL;
		/* Driver is started but nothing will ever drain it — tear down
		 * fully instead of leaving a stray running-but-taskless driver
		 * (matches can_resume()/can_recover()'s handling of this failure). */
		twai_stop();
		twai_driver_uninstall();
		s_driver_installed = false;
	}
	/* Clear suspended flag: scan may have left it true if can_resume() failed */
	s_suspended = false;
	ESP_LOGI(TAG, "Bitrate change completed");
}

uint8_t can_get_bitrate_index(void) {
	return g_bitrate_index;
}

void can_persist_bitrate(uint8_t bitrate_index) {
	config_store_save_bitrate(bitrate_index);
	g_bitrate_index = bitrate_index;
}

esp_err_t can_transmit_frame_ext(uint32_t can_id, bool extd,
                                 const uint8_t *data, uint8_t dlc) {
	twai_message_t msg = {0};
	msg.identifier = extd ? (can_id & 0x1FFFFFFFu) : (can_id & 0x7FFu);
	msg.extd = extd ? 1 : 0;
	msg.data_length_code = dlc > 8 ? 8 : dlc;
	if (data && dlc > 0)
		memcpy(msg.data, data, msg.data_length_code);
	esp_err_t ret = twai_transmit(&msg, pdMS_TO_TICKS(5));
	if (ret != ESP_OK)
		ESP_LOGD(TAG, "CAN TX 0x%lX failed: %s", (unsigned long)can_id, esp_err_to_name(ret));
	return ret;
}

esp_err_t can_transmit_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc) {
	return can_transmit_frame_ext(can_id, false, data, dlc);
}

void can_set_obd_extended(bool extended) {
	s_obd_extended = extended;
}

bool can_get_obd_extended(void) {
	return s_obd_extended;
}

void can_persist_obd_extended(bool extended) {
	s_obd_extended = extended;
	config_store_save_obd_extended(extended ? 1 : 0);
}

esp_err_t can_inject_rx_frame(uint32_t id, bool extd, const uint8_t *data, uint8_t dlc) {
	if (s_can_queue == NULL)
		return ESP_ERR_INVALID_STATE;
	twai_message_t msg = {0};
	msg.identifier = extd ? (id & 0x1FFFFFFFu) : (id & 0x7FFu);
	msg.extd = extd ? 1 : 0;
	msg.data_length_code = dlc > 8 ? 8 : dlc;
	if (data && msg.data_length_code > 0)
		memcpy(msg.data, data, msg.data_length_code);
	/* Same enqueue the RX task uses — drained + dispatched by
	 * can_process_queued_frames() on the LVGL task. Unlike the RX task (which
	 * sends with timeout 0 and drops on a full queue), wait briefly for space:
	 * on a busy bus the 64-deep queue is often full between LVGL drain cycles,
	 * and a dropped injection would look like a silent test failure. ~50 ms
	 * spans a few drain cycles; acceptable on the httpd task for a test path. */
	return (xQueueSendToBack(s_can_queue, &msg, pdMS_TO_TICKS(50)) == pdPASS)
	           ? ESP_OK : ESP_FAIL;
}

void can_process_queued_frames(void) {
	if (s_can_queue == NULL) {
		return;
	}

	/* Drain a bounded batch of frames per call to avoid starving other LVGL
	 * work if the bus is very busy. */
	const int max_batch = 32;
	int processed = 0;
	twai_message_t msg;

	/* Signal dispatch is COALESCED to the last frame per CAN id in the
	 * batch. On a busy bus (~300+ frames/s vs ~30 renders/s) ten-plus
	 * frames of the same id drain per render cycle; dispatching each one
	 * invalidated widgets at every INTERMEDIATE value — positions that are
	 * never rendered (only the last value before the next refresh is) —
	 * which flooded LVGL's dirty-rect buffer past LV_INV_BUF_SIZE and
	 * triggered its silent full-screen-redraw fallback: the periodic
	 * 130-170 ms hitch frames caught by /api/perf/bigframe's overflow
	 * backtrace (meter rear-inv × N intermediate needle positions).
	 * Gauges are pure value-state, so last-value-wins is lossless for
	 * them; the OBD2 decoder (ISO-TP is sequential!), the per-id tracker,
	 * and the raw trace logger still see EVERY frame below. */
	static struct {
		uint32_t id;
		uint8_t  data[8];
		uint8_t  dlc;
	} coal[32];   /* static: LVGL task only; max_batch bounds the count */
	int n_coal = 0;

	while (processed < max_batch &&
		   xQueueReceive(s_can_queue, &msg, 0) == pdTRUE) {
		/* When simulator is active, drain queue but skip dispatch */
		if (!signal_sim_is_active()) {
			int k;
			for (k = 0; k < n_coal; k++)
				if (coal[k].id == msg.identifier) break;
			if (k < (int)(sizeof(coal) / sizeof(coal[0]))) {
				coal[k].id  = msg.identifier;
				coal[k].dlc = msg.data_length_code > 8 ? 8 : msg.data_length_code;
				memcpy(coal[k].data, msg.data, coal[k].dlc);
				if (k == n_coal) n_coal++;
			}
			/* OBD2 response decoder: standard frames on 0x7E8-0x7EF, or
			 * 29-bit ISO 15765-4 responses 0x18DAF1xx (target F1 = tester)
			 * when the car uses extended addressing. Cheap early-return
			 * for everything else. */
			bool obd_std = !msg.extd &&
			               msg.identifier >= OBD2_RESPONSE_ID_FIRST &&
			               msg.identifier <= OBD2_RESPONSE_ID_LAST;
			bool obd_ext = msg.extd &&
			               (msg.identifier & OBD2_RESPONSE_29_MASK) ==
			                   OBD2_RESPONSE_29_BASE;
			if (obd_std || obd_ext) {
				obd2_rx_handler(msg.identifier, msg.data, msg.data_length_code);
			}
			/* Per-ID tracker for live diagnostics screen — captures every
			 * frame regardless of whether any signal matched. */
			can_id_tracker_record(msg.identifier, msg.extd,
			                      msg.data, msg.data_length_code);
			/* Raw capture — cheap no-op when the logger isn't active.
			 * Lets users send a full trace off-device for offline DBC
			 * decoding without needing signals defined in the layout. */
			can_raw_logger_record_frame(msg.identifier, msg.extd != 0,
			                            msg.data, msg.data_length_code);
		}
		processed++;
	}

	/* Dispatch once per unique id, in first-seen order, with the freshest
	 * payload. Widgets repaint at most once per id per render cycle. */
	for (int k = 0; k < n_coal; k++)
		signal_dispatch_frame(coal[k].id, coal[k].data, coal[k].dlc);
}

esp_err_t can_get_diagnostics(uint32_t *state, uint32_t *msgs_to_tx,
                              uint32_t *msgs_to_rx, uint32_t *tx_error_counter,
                              uint32_t *rx_error_counter, uint32_t *bus_error_count,
                              uint32_t *rx_missed) {
	twai_status_info_t info;
	esp_err_t err = twai_get_status_info(&info);
	if (err != ESP_OK) return err;
	if (state)            *state            = info.state;
	if (msgs_to_tx)       *msgs_to_tx       = info.msgs_to_tx;
	if (msgs_to_rx)       *msgs_to_rx       = info.msgs_to_rx;
	if (tx_error_counter) *tx_error_counter  = info.tx_error_counter;
	if (rx_error_counter) *rx_error_counter  = info.rx_error_counter;
	if (bus_error_count)  *bus_error_count   = info.bus_error_count;
	if (rx_missed)        *rx_missed         = info.arb_lost_count + info.rx_missed_count;
	return ESP_OK;
}

uint32_t can_get_last_rx_id(void) {
	return s_last_rx_can_id;
}

uint32_t can_get_rx_frame_count(void) {
	return s_rx_frame_count;
}

twai_timing_config_t can_get_timing_for_bitrate(uint8_t index) {
	return _bitrate_to_timing(index);
}

bool can_is_suspended(void) {
	return s_suspended;
}

void can_suspend(void) {
	if (s_suspended) return;
	ESP_LOGI(TAG, "Suspending CAN for bus test");
	/* Flip the flag FIRST: from here on reconfigure_can_filter /
	 * can_set_promiscuous_mode defer instead of reinstalling the driver
	 * underneath the scan. */
	s_suspended = true;
	_stop_can_task();
	vTaskDelay(pdMS_TO_TICKS(50));
	/* _stop_can_task only stops the driver when an RX task existed; stop
	 * explicitly so uninstall can't fail INVALID_STATE on a RUNNING driver
	 * (harmless no-op if already stopped). */
	twai_stop();
	twai_driver_uninstall();
	s_driver_installed = false;
	vTaskDelay(pdMS_TO_TICKS(50));
}

void can_resume(void) {
	if (!s_suspended) return;
	ESP_LOGI(TAG, "Resuming normal CAN operation");

	/* Rebuild the acceptance filter. Honour a promiscuous request that
	 * arrived (deferred) while the scan owned the peripheral — e.g. the
	 * wizard moving from the scan step straight into the ECU probe. */
	if (s_promiscuous_active) {
		f_config = (twai_filter_config_t)TWAI_FILTER_CONFIG_ACCEPT_ALL();
	} else {
		build_twai_filter_from_signals(&f_config);
	}

	/* Load saved bitrate */
	uint8_t saved_bitrate = 2;
	config_store_load_bitrate(&saved_bitrate);
	g_t_config = _bitrate_to_timing(saved_bitrate);

	/* Defensive teardown before the FIRST install attempt. Both calls
	 * are harmless if nothing's installed/running — the driver returns
	 * ESP_ERR_INVALID_STATE which we ignore. This catches the case where
	 * a prior failed install left partial state behind. */
	twai_stop();
	twai_driver_uninstall();
	s_driver_installed = false;
	vTaskDelay(pdMS_TO_TICKS(100));

	if (_install_twai_with_retry("Resume") != ESP_OK) {
		/* Leave s_suspended=true so a future call can retry. Previously
		 * cleared on failure, which silently locked CAN out until reboot. */
		return;
	}

	if (twai_start() != ESP_OK) {
		ESP_LOGE(TAG, "Resume: twai_start failed - uninstalling for clean retry");
		twai_driver_uninstall();
		return;  /* keep s_suspended=true so caller can retry */
	}
	s_driver_installed = true;
	vTaskDelay(pdMS_TO_TICKS(50));

	can_task_should_stop = false;
	if (_spawn_can_rx_task() != pdPASS) {
		ESP_LOGE(TAG, "Resume: failed to create CAN RX task");
		canTaskHandle = NULL;
		twai_stop();
		twai_driver_uninstall();
		s_driver_installed = false;
		return;  /* keep s_suspended=true so caller can retry */
	}
	s_suspended = false;
	ESP_LOGI(TAG, "CAN resumed");
}

bool can_recover(void) {
	/* Rate-limit so a hard-stuck state doesn't spin can_resume on every
	 * 1 s diagnostics tick. esp_timer_get_time() is monotonic since boot. */
	static int64_t s_last_attempt_us = 0;
	int64_t now = esp_timer_get_time();
	if (now - s_last_attempt_us < 5LL * 1000 * 1000) return false;
	s_last_attempt_us = now;

	if (s_suspended) {
		ESP_LOGW(TAG, "Recovery: was left suspended - retrying resume");
		can_resume();
		return true;
	}

	twai_status_info_t info;
	if (twai_get_status_info(&info) == ESP_OK) {
		/* FIX #1 — driver responds, but check WHICH state it's in. A bus-off
		 * controller still answers twai_get_status_info() with ESP_OK, so the
		 * old "alive → nothing to recover" early-return never noticed bus-off
		 * and CAN stayed dead. Run the recovery sequence here too; only treat
		 * RUNNING/STOPPED-healthy as genuinely nothing-to-do. */
		if (info.state == TWAI_STATE_BUS_OFF) {
			ESP_LOGW(TAG, "Recovery: driver in BUS_OFF (tx_err=%lu) — recovering",
					 (unsigned long)info.tx_error_counter);
			return _recover_from_bus_off(&info);
		}
		/* RUNNING, STOPPED, or RECOVERING (transient) — let the driver/RX
		 * task settle on its own; nothing for us to reinstall. */
		return false;
	}

	/* Driver is gone but we never got a suspend marker. Could be a corrupt
	 * state from a botched previous teardown. Run the full re-init: stop
	 * any leftover task handle, reinstall, restart, recreate RX task. */
	ESP_LOGW(TAG, "Recovery: TWAI driver missing - reinstalling from scratch");
	_stop_can_task();
	vTaskDelay(pdMS_TO_TICKS(50));
	/* Defensive uninstall - ESP_ERR_INVALID_STATE on a not-installed driver
	 * is fine, we just want to guarantee a clean slate. */
	twai_driver_uninstall();
	s_driver_installed = false;
	vTaskDelay(pdMS_TO_TICKS(50));

	build_twai_filter_from_signals(&f_config);
	uint8_t saved_bitrate = 2;
	config_store_load_bitrate(&saved_bitrate);
	g_t_config = _bitrate_to_timing(saved_bitrate);

	if (_install_twai_with_retry("Recovery") != ESP_OK) return true;
	if (twai_start() != ESP_OK) {
		ESP_LOGE(TAG, "Recovery: twai_start failed");
		twai_driver_uninstall();
		return true;
	}
	s_driver_installed = true;
	can_task_should_stop = false;
	if (_spawn_can_rx_task() != pdPASS) {
		ESP_LOGE(TAG, "Recovery: failed to create CAN RX task");
		canTaskHandle = NULL;
		twai_stop();
		twai_driver_uninstall();
		s_driver_installed = false;
		return true;
	}
	ESP_LOGI(TAG, "Recovery: CAN reinstalled and RX task started");
	return true;
}
