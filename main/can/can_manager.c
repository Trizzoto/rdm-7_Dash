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
static void can_receive_task(void *pvParameter) {
	(void)pvParameter;
	static uint32_t s_queue_drop_count = 0;

	while (!can_task_should_stop) {
		twai_message_t message;
		esp_err_t ret = twai_receive(&message, pdMS_TO_TICKS(5));

		if (ret == ESP_OK) {
			if (s_can_queue != NULL) {
				/* Non-blocking enqueue; drop oldest frames if the queue is
				 * momentarily full rather than stalling the RX loop. */
				if (xQueueSendToBack(s_can_queue, &message, 0) != pdPASS) {
					s_queue_drop_count++;
					if ((s_queue_drop_count % 100) == 1) {
						ESP_LOGW(TAG, "CAN queue overflow (total drops: %lu)", (unsigned long)s_queue_drop_count);
					}
				}
			}
		} else if (ret == ESP_ERR_TIMEOUT) {
			vTaskDelay(pdMS_TO_TICKS(1));
		} else if (ret == ESP_ERR_INVALID_STATE) {
			ESP_LOGW(TAG, "CAN bus error, attempting recovery");
			twai_stop();
			vTaskDelay(pdMS_TO_TICKS(100));
			esp_err_t start_err = twai_start();
			if (start_err != ESP_OK) {
				ESP_LOGE(TAG, "CAN recovery failed: %s", esp_err_to_name(start_err));
			}
		} else {
			ESP_LOGW(TAG, "CAN receive error: %s", esp_err_to_name(ret));
			vTaskDelay(pdMS_TO_TICKS(1));
		}
	}

	ESP_LOGI(TAG, "CAN task exited gracefully");
	can_task_should_stop = false;
	vTaskDelete(NULL);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void build_twai_filter_from_signals(twai_filter_config_t *out_filter) {
	uint16_t sig_count = signal_get_count();
	if (sig_count == 0) {
		*out_filter = (twai_filter_config_t)TWAI_FILTER_CONFIG_ACCEPT_ALL();
		return;
	}

	/* Collect unique CAN IDs from the signal registry */
	uint32_t ids[MAX_SIGNALS];
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
		if (!dup && count < MAX_SIGNALS)
			ids[count++] = sid;
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

/** Stop the CAN receive task gracefully.  Halts the TWAI peripheral first
 *  so twai_receive() returns immediately, then waits for the task to exit. */
static void _stop_can_task(void) {
	if (canTaskHandle == NULL) return;

	/* Stop TWAI first so twai_receive() unblocks with ESP_ERR_INVALID_STATE */
	twai_stop();
	can_task_should_stop = true;

	/* Wait up to 500 ms for graceful exit */
	for (int i = 0; i < 50; i++) {
		if (eTaskGetState(canTaskHandle) == eDeleted)
			break;
		vTaskDelay(pdMS_TO_TICKS(10));
	}
	if (eTaskGetState(canTaskHandle) != eDeleted) {
		ESP_LOGW(TAG, "CAN task did not exit gracefully, force-deleting");
		vTaskDelete(canTaskHandle);
	}
	canTaskHandle = NULL;
}

void reconfigure_can_filter(void) {
	_stop_can_task();

	build_twai_filter_from_signals(&f_config);

	vTaskDelay(pdMS_TO_TICKS(50));
	twai_driver_uninstall();
	vTaskDelay(pdMS_TO_TICKS(50));
	if (twai_driver_install(&g_config, &g_t_config, &f_config) == ESP_OK)
		twai_start();
	vTaskDelay(pdMS_TO_TICKS(50));

	xTaskCreatePinnedToCore(can_receive_task, "can_receive_task", 4096, NULL, 7,
							&canTaskHandle, 0);
}

void can_init(void) {
	/* Load saved bitrate from NVS (default index 2 = 500 kbps) */
	uint8_t saved_bitrate = 2;
	config_store_load_bitrate(&saved_bitrate);

	switch (saved_bitrate) {
	case 0:
		g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
		ESP_LOGI(TAG, "CAN bitrate: 125 kbps");
		break;
	case 1:
		g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
		ESP_LOGI(TAG, "CAN bitrate: 250 kbps");
		break;
	case 3:
		g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
		ESP_LOGI(TAG, "CAN bitrate: 1 Mbps");
		break;
	default:
	case 2:
		g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
		ESP_LOGI(TAG, "CAN bitrate: 500 kbps");
		break;
	}

	build_twai_filter_from_signals(&f_config);

	if (twai_driver_install(&g_config, &g_t_config, &f_config) == ESP_OK)
		ESP_LOGI(TAG, "TWAI driver installed");
	else
		ESP_LOGE(TAG, "TWAI driver install failed");

	if (twai_start() == ESP_OK)
		ESP_LOGI(TAG, "TWAI started");
	else
		ESP_LOGE(TAG, "TWAI start failed");

	/* Create the CAN frame queue once the driver is up.  32 entries is more
	 * than enough for this dashboard workload and keeps RAM usage modest. */
	if (s_can_queue == NULL) {
		s_can_queue = xQueueCreate(32, sizeof(twai_message_t));
		if (s_can_queue == NULL) {
			ESP_LOGE(TAG, "Failed to create CAN RX queue");
		}
	}
}

void can_start_task(void) {
	xTaskCreatePinnedToCore(can_receive_task, "can_receive_task", 4096, NULL, 4,
							&canTaskHandle, 0);
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
	switch (bitrate_index) {
	case 0:
		g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
		ESP_LOGI(TAG, "CAN bitrate: 125 kbps");
		break;
	case 1:
		g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
		ESP_LOGI(TAG, "CAN bitrate: 250 kbps");
		break;
	case 3:
		g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
		ESP_LOGI(TAG, "CAN bitrate: 1 Mbps");
		break;
	default:
	case 2:
		g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
		ESP_LOGI(TAG, "CAN bitrate: 500 kbps");
		break;
	}

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

	/* Recreate receive task at priority 7 (same as reconfigure_can_filter) */
	if (xTaskCreatePinnedToCore(can_receive_task, "can_receive_task", 4096,
								NULL, 7, &canTaskHandle, 0) != pdPASS) {
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
	const int max_batch = 8;
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
