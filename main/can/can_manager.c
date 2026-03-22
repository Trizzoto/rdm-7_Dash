/*
 * can_manager.c — TWAI peripheral lifecycle and CAN receive task.
 *
 * Owns all TWAI hardware configuration, the acceptance filter, and the
 * simplified receive task that enqueues frames for the LVGL task to
 * process via signal_dispatch_frame().
 */
#include "can_manager.h"
#include "signal.h"
#include "signal_sim.h"

#include "driver/twai.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "storage/config_store.h"


static const char *TAG = "CAN_MGR";

/* ── TWAI hardware configuration ────────────────────────────────────── */

static twai_timing_config_t g_t_config = TWAI_TIMING_CONFIG_500KBITS();
static twai_general_config_t g_config =
	TWAI_GENERAL_CONFIG_DEFAULT(20, 19, TWAI_MODE_NORMAL);
static twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

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

/* LVGL mutex helpers — defined in main.c */
extern bool example_lvgl_lock(uint32_t timeout_ms);
extern void example_lvgl_unlock(void);

/* ── CAN receive task ─────────────────────────────────────────────────
 *
 * Simplified: receive one frame and enqueue it for the LVGL task to process.
 * The task never touches LVGL directly — it only writes to s_can_queue.
 */
/* CAN bus-off recovery limits */
#define CAN_RECOVERY_MAX_RETRIES    10
#define CAN_RECOVERY_INITIAL_MS     100
#define CAN_RECOVERY_MAX_MS         5000

static void can_receive_task(void *pvParameter) {
	(void)pvParameter;
	static uint32_t s_queue_drop_count = 0;
	int recovery_retries = 0;
	uint32_t recovery_delay_ms = CAN_RECOVERY_INITIAL_MS;

	s_can_task_running = true;

	while (!can_task_should_stop) {
		twai_message_t message;
		esp_err_t ret = twai_receive(&message, pdMS_TO_TICKS(5));

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
			vTaskDelay(pdMS_TO_TICKS(1));
		} else if (ret == ESP_ERR_INVALID_STATE) {
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
	vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void build_twai_filter_from_signals(twai_filter_config_t *out_filter) {
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
		vTaskDelete(canTaskHandle);
		s_can_task_running = false;
	}
	canTaskHandle = NULL;
	can_task_should_stop = false;
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
	_stop_can_task();

	build_twai_filter_from_signals(&f_config);

	vTaskDelay(pdMS_TO_TICKS(50));
	twai_driver_uninstall();
	vTaskDelay(pdMS_TO_TICKS(50));
	esp_err_t err = twai_driver_install(&g_config, &g_t_config, &f_config);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "twai_driver_install failed: %s", esp_err_to_name(err));
		return;
	}
	twai_start();
	vTaskDelay(pdMS_TO_TICKS(50));

	xTaskCreatePinnedToCore(can_receive_task, "can_receive_task", 4096, NULL,
							CAN_TASK_PRIORITY, &canTaskHandle, 0);
}

void can_init(void) {
	/* Load saved bitrate from NVS (default index 2 = 500 kbps) */
	uint8_t saved_bitrate = 2;
	config_store_load_bitrate(&saved_bitrate);

	g_t_config = _bitrate_to_timing(saved_bitrate);
	static const char *bitrate_labels[] = {"125 kbps", "250 kbps", "500 kbps", "1 Mbps"};
	ESP_LOGI(TAG, "CAN bitrate: %s", bitrate_labels[saved_bitrate > 3 ? 2 : saved_bitrate]);

	build_twai_filter_from_signals(&f_config);

	if (twai_driver_install(&g_config, &g_t_config, &f_config) == ESP_OK)
		ESP_LOGI(TAG, "TWAI driver installed");
	else
		ESP_LOGE(TAG, "TWAI driver install failed");

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

	xTaskCreatePinnedToCore(can_receive_task, "can_receive_task", 4096, NULL,
							CAN_TASK_PRIORITY, &canTaskHandle, 0);
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
	_stop_can_task();

	/* Apply new timing config */
	g_t_config = _bitrate_to_timing(bitrate_index);
	static const char *bitrate_labels[] = {"125 kbps", "250 kbps", "500 kbps", "1 Mbps"};
	ESP_LOGI(TAG, "CAN bitrate: %s", bitrate_labels[bitrate_index > 3 ? 2 : bitrate_index]);

	/* Uninstall, rebuild filter, reinstall (TWAI already stopped by _stop_can_task) */
	vTaskDelay(pdMS_TO_TICKS(50));
	twai_driver_uninstall();
	vTaskDelay(pdMS_TO_TICKS(50));

	build_twai_filter_from_signals(&f_config);

	if (twai_driver_install(&g_config, &g_t_config, &f_config) != ESP_OK) {
		ESP_LOGE(TAG, "TWAI driver install failed after bitrate change");
		return;
	}
	vTaskDelay(pdMS_TO_TICKS(50));

	if (twai_start() != ESP_OK) {
		ESP_LOGE(TAG, "TWAI start failed after bitrate change");
		return;
	}
	vTaskDelay(pdMS_TO_TICKS(50));

	/* Recreate receive task */
	if (xTaskCreatePinnedToCore(can_receive_task, "can_receive_task", 4096,
								NULL, CAN_TASK_PRIORITY, &canTaskHandle, 0) != pdPASS) {
		ESP_LOGE(TAG, "Failed to create CAN task after bitrate change");
		canTaskHandle = NULL;
	}
	ESP_LOGI(TAG, "Bitrate change completed");
}

esp_err_t can_transmit_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc) {
	twai_message_t msg = {0};
	msg.identifier = can_id & 0x7FFu;
	msg.data_length_code = dlc > 8 ? 8 : dlc;
	if (data && dlc > 0)
		memcpy(msg.data, data, msg.data_length_code);
	esp_err_t ret = twai_transmit(&msg, pdMS_TO_TICKS(5));
	if (ret != ESP_OK)
		ESP_LOGD(TAG, "CAN TX 0x%03lX failed: %s", (unsigned long)can_id, esp_err_to_name(ret));
	return ret;
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

	while (processed < max_batch &&
		   xQueueReceive(s_can_queue, &msg, 0) == pdTRUE) {
		/* When simulator is active, drain queue but skip dispatch */
		if (!signal_sim_is_active()) {
			signal_dispatch_frame(msg.identifier, msg.data, msg.data_length_code);
		}
		processed++;
	}
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

void can_suspend(void) {
	if (s_suspended) return;
	ESP_LOGI(TAG, "Suspending CAN for bus test");
	_stop_can_task();
	vTaskDelay(pdMS_TO_TICKS(50));
	twai_driver_uninstall();
	vTaskDelay(pdMS_TO_TICKS(50));
	s_suspended = true;
}

void can_resume(void) {
	if (!s_suspended) return;
	ESP_LOGI(TAG, "Resuming normal CAN operation");

	/* Rebuild acceptance filter from current signal registry */
	build_twai_filter_from_signals(&f_config);

	/* Load saved bitrate */
	uint8_t saved_bitrate = 2;
	config_store_load_bitrate(&saved_bitrate);
	g_t_config = _bitrate_to_timing(saved_bitrate);

	esp_err_t err = twai_driver_install(&g_config, &g_t_config, &f_config);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Resume: twai_driver_install failed: %s",
				 esp_err_to_name(err));
		s_suspended = false;
		return;
	}

	if (twai_start() != ESP_OK) {
		ESP_LOGE(TAG, "Resume: twai_start failed");
		s_suspended = false;
		return;
	}
	vTaskDelay(pdMS_TO_TICKS(50));

	can_task_should_stop = false;
	xTaskCreatePinnedToCore(can_receive_task, "can_receive_task", 4096,
							NULL, CAN_TASK_PRIORITY, &canTaskHandle, 0);
	s_suspended = false;
	ESP_LOGI(TAG, "CAN resumed");
}
