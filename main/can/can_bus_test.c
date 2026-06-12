/*
 * can_bus_test.c -- Automated CAN bus scan task.
 *
 * Temporarily takes ownership of the TWAI peripheral to test each bitrate
 * in listen-only mode.  Picks the bitrate with the most received frames.
 *
 * Runs on core 0, priority 5, stack 4096.  Communicates to LVGL via
 * rdm_async_call() so UI updates are thread-safe.
 */
#include "can_bus_test.h"
#include "system/rdm_lv_async.h"
#include "can_manager.h"

#include <string.h>

#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"   /* xTaskCreateWithCaps / vTaskDeleteWithCaps */
#include "lvgl.h"

static const char *TAG = "CAN_TEST";

/* ── State ─────────────────────────────────────────────────────────────── */

static can_scan_report_t s_report;
static volatile bool     s_running   = false;
static volatile bool     s_cancel    = false;
static can_scan_ui_cb_t  s_ui_cb     = NULL;
static TaskHandle_t      s_task_handle = NULL;
/* Wall-clock start of the current scan task. Used by can_bus_test_start to
 * detect a hung scan and force-reset. With no CAN connected the worst-case
 * runtime is ~12 s (4 bitrates × 3 install retries × backoff + listen
 * windows + can_resume retries). 30 s gives plenty of headroom. */
static int64_t           s_task_start_us = 0;
#define SCAN_TASK_HUNG_TIMEOUT_US (30LL * 1000 * 1000)

/* Duration to listen at each bitrate (ms) */
#define LISTEN_DURATION_MS  2000
#define LISTEN_POLL_MS      10

/* Per-bitrate step-by-step error trace for field diagnosis — every driver
 * call in the install cycle, exposed via /api/can/scan/status. The serial
 * console is owned by the desktop protocol in production builds, so this
 * is the only way to see WHICH call fails on a customer device. */
typedef struct {
    esp_err_t pre_stop;          /* defensive twai_stop() before attempt 1 */
    esp_err_t pre_uninstall;     /* defensive twai_driver_uninstall() */
    esp_err_t install[3];        /* twai_driver_install per attempt */
    esp_err_t start[3];          /* twai_start per attempt (0x7FFF = not reached) */
} scan_step_dbg_t;
#define SCAN_DBG_NOT_REACHED ((esp_err_t)0x7FFF)
static scan_step_dbg_t s_dbg[4];

/* TX/RX GPIOs (must match g_config in can_manager.c) */
#define CAN_TX_GPIO  20
#define CAN_RX_GPIO  19

/* ── Helpers ───────────────────────────────────────────────────────────── */

static void _notify_ui(void *arg) {
    (void)arg;
    if (s_ui_cb) s_ui_cb();
}

static void _push_ui_update(void) {
    rdm_async_call(_notify_ui, NULL);
}

/** Add a CAN ID to the unique-IDs list for a result entry. */
static void _track_unique_id(can_scan_bitrate_result_t *r, uint32_t id) {
    for (uint8_t i = 0; i < r->unique_id_count; i++) {
        if (r->unique_ids[i] == id) return;
    }
    if (r->unique_id_count < 32) {
        r->unique_ids[r->unique_id_count++] = id;
    }
}

/**
 * Install TWAI in listen-only mode with accept-all filter at the given
 * timing config. Retries up to 3 times on install/start failure -- with no
 * CAN wires connected the peripheral can take a few attempts to settle
 * between rapid uninstall/install cycles. Returns ESP_OK on success.
 *
 * Logs ESP_ERR codes both as a name and as a raw hex value so that
 * unfamiliar error codes can still be looked up if the symbol isn't in
 * the resolver table on a particular ESP-IDF build.
 */
static esp_err_t _install_listen_only(const twai_timing_config_t *t_config,
                                      scan_step_dbg_t *dbg) {
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO,
                                                          CAN_RX_GPIO,
                                                          TWAI_MODE_LISTEN_ONLY);
    /* 8-deep RX queue is plenty for a 2-second listen; smaller allocation
     * also rules out a heap-fragmentation NO_MEM as a cause of install
     * failure on a long-running unit. */
    g.rx_queue_len = 8;
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    /* Defensive teardown before the FIRST attempt. If a prior failed
     * cycle left the driver partially installed, this gets us back to a
     * known clean state. Both calls are harmless if nothing is there
     * (driver returns ESP_ERR_INVALID_STATE which we ignore). */
    dbg->pre_stop      = twai_stop();
    dbg->pre_uninstall = twai_driver_uninstall();
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        err = twai_driver_install(&g, t_config, &f);
        dbg->install[attempt] = err;
        if (err == ESP_OK) {
            err = twai_start();
            dbg->start[attempt] = err;
            if (err == ESP_OK) {
                /* Allow the TWAI controller to synchronize with the bus.
                 * With no bus connected, sync never completes but the
                 * controller is still in a valid state for receiving. */
                vTaskDelay(pdMS_TO_TICKS(50));
                return ESP_OK;
            }
            ESP_LOGW(TAG, "twai_start failed attempt %d/3: %s (0x%04x)",
                     attempt + 1, esp_err_to_name(err), err);
            twai_driver_uninstall();
        } else {
            ESP_LOGW(TAG, "twai_driver_install failed attempt %d/3: %s (0x%04x)",
                     attempt + 1, esp_err_to_name(err), err);
            twai_driver_uninstall();
        }
        /* Long backoff: 200/400/600 ms. The peripheral can need more
         * than the original 80 ms to release between rapid cycles. */
        vTaskDelay(pdMS_TO_TICKS(200 * (attempt + 1)));
    }
    return err;
}

/** Uninstall TWAI (stop + uninstall, ignoring errors). 50ms each side
 *  matches can_suspend; 20ms was insufficient for some no-bus scenarios. */
static void _uninstall(void) {
    twai_stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    twai_driver_uninstall();
    vTaskDelay(pdMS_TO_TICKS(50));
}

/**
 * Listen for frames for LISTEN_DURATION_MS.  Populates *result.
 * Returns early if s_cancel is set.
 */
static void _listen_for_traffic(can_scan_bitrate_result_t *result) {
    result->frames_received = 0;
    result->bus_errors = 0;
    result->unique_id_count = 0;
    result->traffic_detected = false;

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(LISTEN_DURATION_MS);

    while (xTaskGetTickCount() < deadline && !s_cancel) {
        twai_message_t msg;
        esp_err_t ret = twai_receive(&msg, pdMS_TO_TICKS(LISTEN_POLL_MS));
        if (ret == ESP_OK) {
            result->frames_received++;
            result->traffic_detected = true;
            _track_unique_id(result, msg.identifier);
        }
    }

    /* Grab error counters and log driver state */
    twai_status_info_t info;
    if (twai_get_status_info(&info) == ESP_OK) {
        result->bus_errors = info.bus_error_count;
        ESP_LOGI(TAG, "  listen result: state=%lu frames=%lu bus_err=%lu",
                 (unsigned long)info.state,
                 (unsigned long)result->frames_received,
                 (unsigned long)info.bus_error_count);
    }
}

/* ── Scan task ─────────────────────────────────────────────────────────── */

static void _scan_task(void *arg) {
    (void)arg;

    ESP_LOGI(TAG, "scan task on core %d", xPortGetCoreID());

    /* Phase 1: stop normal CAN operation */
    s_report.state = CAN_SCAN_STOPPING;
    _push_ui_update();
    can_suspend();
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Phase 2: test each bitrate */
    int8_t best_bitrate = -1;
    uint32_t best_frames = 0;

    for (uint8_t i = 0; i < 4 && !s_cancel; i++) {
        s_report.state = CAN_SCAN_TESTING_BITRATE;
        s_report.current_bitrate_idx = i;
        s_report.results[i].bitrate_index = i;
        _push_ui_update();

        twai_timing_config_t t = can_get_timing_for_bitrate(i);
        memset(&s_dbg[i], 0x7F, sizeof(s_dbg[i]));   /* mark all not-reached */
        esp_err_t err = _install_listen_only(&t, &s_dbg[i]);
        s_report.results[i].install_err = err;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install TWAI for bitrate %d: %s",
                     i, esp_err_to_name(err));
            /* Already cleaned up inside _install_listen_only. Mark this
             * bitrate as failed-to-install so the UI can show something
             * other than a frozen "Testing..." label. We use bus_errors
             * as the failure marker (set traffic_detected=false; UI
             * already treats no-traffic as "No traffic"). */
            s_report.results[i].traffic_detected = false;
            s_report.results[i].frames_received = 0;
            s_report.results[i].bus_errors      = 0xFFFFFFFFu;  /* sentinel */
            /* Bump the per-bitrate UI so user sees this slot move on */
            _push_ui_update();
            /* Brief settle delay before the next iteration -- gives the
             * peripheral time to release whatever state caused the install
             * to fail. Without this, multiple bitrates can fail in a row. */
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        _listen_for_traffic(&s_report.results[i]);
        _uninstall();

        if (s_report.results[i].traffic_detected) {
            if (s_report.results[i].frames_received > best_frames) {
                best_frames = s_report.results[i].frames_received;
                best_bitrate = (int8_t)i;
            }
        }

        ESP_LOGI(TAG, "Bitrate %d: %lu frames, %u unique IDs, %lu errors",
                 i,
                 (unsigned long)s_report.results[i].frames_received,
                 s_report.results[i].unique_id_count,
                 (unsigned long)s_report.results[i].bus_errors);
    }

    /* Phase 3: restore normal CAN operation */
    s_report.state = CAN_SCAN_RESTORING;
    _push_ui_update();
    can_resume();

    /* Phase 4: finalize report */
    s_report.recommended_bitrate = best_bitrate;
    s_report.state = s_cancel ? CAN_SCAN_CANCELLED : CAN_SCAN_COMPLETE;
    s_running = false;
    _push_ui_update();

    ESP_LOGI(TAG, "Scan %s. Recommended bitrate: %d",
             s_cancel ? "cancelled" : "complete", best_bitrate);

    s_task_handle = NULL;
    /* Matches xTaskCreateWithCaps so the PSRAM stack is freed too */
    vTaskDeleteWithCaps(NULL);
}

/* ── Public API ────────────────────────────────────────────────────────── */

bool can_bus_test_start(void) {
    /* Self-heal from a stuck "running" state. Three paths can leave
     * s_running stuck true:
     *   1. Scan task crashed / was force-deleted (task handle invalid)
     *   2. Scan task self-deleted but the flag wasn't cleared (race)
     *   3. Scan task is still cleaning up (legitimately running)
     *
     * For case 3 we honour it (return false). For cases 1 and 2 we force
     * a reset. Wall-clock hung-task timeout catches the worst-case where
     * eTaskGetState lies about a freed handle. */
    if (s_running) {
        bool handle_gone = (s_task_handle == NULL);
        bool task_deleted = handle_gone ||
            (eTaskGetState(s_task_handle) == eDeleted);
        int64_t age_us = esp_timer_get_time() - s_task_start_us;
        bool hung = (age_us > SCAN_TASK_HUNG_TIMEOUT_US);

        if (handle_gone || task_deleted) {
            ESP_LOGW(TAG, "stale s_running (handle_gone=%d deleted=%d) - resetting",
                     handle_gone, task_deleted);
            s_running     = false;
            s_task_handle = NULL;
        } else if (hung) {
            ESP_LOGW(TAG, "scan task hung for %lld ms - force-deleting",
                     age_us / 1000);
            /* WithCaps variant frees the PSRAM stack from outside the task */
            vTaskDeleteWithCaps(s_task_handle);
            s_running     = false;
            s_task_handle = NULL;
        } else {
            ESP_LOGI(TAG, "scan still running (age=%lld ms) - rejecting start",
                     age_us / 1000);
            return false;  /* genuinely running */
        }
    }

    /* Reset report */
    memset(&s_report, 0, sizeof(s_report));
    s_report.state = CAN_SCAN_IDLE;
    s_report.recommended_bitrate = -1;
    s_cancel = false;
    s_running = true;
    s_task_start_us = esp_timer_get_time();

    /* Stack in PSRAM — internal RAM is tight after WiFi init (often < 6 KB
     * free) so a plain xTaskCreatePinnedToCore would fail to allocate a
     * 4 KB internal-RAM stack. MALLOC_CAP_SPIRAM puts the stack in external
     * RAM; task code still runs from IRAM/flash as normal.
     *
     * Pinning to CORE 1 is REQUIRED, not a preference. esp_intr_alloc /
     * esp_intr_free are PER-CORE, and every runtime TWAI reinstall
     * (reconfigure_can_filter / promiscuous / bitrate change) happens on
     * the LVGL task = core 1 — so the live driver's interrupt lives on
     * core 1 from the first post-boot filter rebuild onward. A scan task
     * on core 0 (or unpinned and scheduled there) can neither free that
     * interrupt nor allocate a fresh one on its own exhausted core:
     * every install — all 4 bitrates AND the can_resume at the end —
     * fails ESP_ERR_NOT_FOUND. The wizard then shows "CAN driver busy"
     * on every row and CAN stays DEAD until reboot. Diagnosed live via
     * /api/can/scan/status step traces on a flooded MaxxECU bus. */
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        _scan_task, "can_scan", 4096, NULL, 5, &s_task_handle,
        1 /*core — MUST match the LVGL/reinstall core, see above*/,
        MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreateWithCaps failed (internal heap=%lu, ret=%d) - aborting start",
                 (unsigned long)xPortGetFreeHeapSize(), (int)ret);
        s_running = false;
        return false;
    }
    ESP_LOGI(TAG, "scan task started (handle=%p)", s_task_handle);
    return true;
}

void can_bus_test_cancel(void) {
    s_cancel = true;
}

bool can_bus_test_is_running(void) {
    return s_running;
}

can_scan_state_t can_bus_test_get_state(void) {
    return s_report.state;
}

uint8_t can_bus_test_get_current_bitrate(void) {
    return s_report.current_bitrate_idx;
}

const can_scan_report_t *can_bus_test_get_report(void) {
    return &s_report;
}

void can_bus_test_set_ui_callback(can_scan_ui_cb_t cb) {
    s_ui_cb = cb;
}

/* Step-trace accessor for /api/can/scan/status — see scan_step_dbg_t. */
void can_bus_test_get_step_dbg(uint8_t bitrate_idx,
                               esp_err_t *pre_stop, esp_err_t *pre_uninstall,
                               esp_err_t install[3], esp_err_t start[3]) {
    if (bitrate_idx > 3) return;
    const scan_step_dbg_t *d = &s_dbg[bitrate_idx];
    if (pre_stop)      *pre_stop      = d->pre_stop;
    if (pre_uninstall) *pre_uninstall = d->pre_uninstall;
    for (int i = 0; i < 3; i++) {
        if (install) install[i] = d->install[i];
        if (start)   start[i]   = d->start[i];
    }
}
