/**
 * can_upload.c — see can_upload.h.
 *
 * Layout:
 *   - can_upload_start() validates args, copies parameters into a heap
 *     struct, and spawns a 8 KB-stack task on core 0.
 *   - _upload_task() opens the file, computes HMAC, POSTs over HTTPS.
 *   - Status is held in a single global guarded by a mutex.
 *
 * Uses mbedTLS for HMAC-SHA256 and esp_http_client for the POST. Files
 * are buffered fully into PSRAM (capped at 10 MB by the server) — for
 * the typical few-hundred-KB CAN trace that's trivial.
 */
#include "storage/can_upload.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md.h"

#include "can_upload_secret.h"
#include "system/device_id.h"

static const char *TAG = "can_upload";

/* ── State ─────────────────────────────────────────────────────────────── */

static SemaphoreHandle_t s_status_mtx = NULL;
static can_upload_status_t s_status = { .state = CAN_UPLOAD_IDLE };

typedef struct {
    char filename[64];
    char make[64];
    char model[64];
    char notes[256];
} upload_params_t;

static void s_status_set(can_upload_state_t state, int http_status,
                         const char *msg, int uploaded)
{
    if (!s_status_mtx) return;
    xSemaphoreTake(s_status_mtx, portMAX_DELAY);
    s_status.state = state;
    s_status.http_status = http_status;
    s_status.uploaded_bytes = uploaded;
    if (msg) {
        strncpy(s_status.message, msg, sizeof(s_status.message) - 1);
        s_status.message[sizeof(s_status.message) - 1] = '\0';
    } else {
        s_status.message[0] = '\0';
    }
    xSemaphoreGive(s_status_mtx);
}

void can_upload_get_status(can_upload_status_t *out)
{
    if (!out) return;
    if (!s_status_mtx) {
        memset(out, 0, sizeof(*out));
        out->state = CAN_UPLOAD_IDLE;
        return;
    }
    xSemaphoreTake(s_status_mtx, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_mtx);
}

/* ── HMAC-SHA256 → hex ─────────────────────────────────────────────────── */

static esp_err_t s_hmac_sha256_hex(const char *secret, const char *msg, char hex_out[65])
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return ESP_FAIL;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    if (mbedtls_md_setup(&ctx, info, 1) != 0) {
        mbedtls_md_free(&ctx);
        return ESP_FAIL;
    }

    uint8_t out[32];
    int rc =
        mbedtls_md_hmac_starts(&ctx, (const unsigned char *)secret, strlen(secret)) ||
        mbedtls_md_hmac_update(&ctx, (const unsigned char *)msg, strlen(msg)) ||
        mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
    if (rc != 0) return ESP_FAIL;

    static const char *hex = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        hex_out[i * 2]     = hex[out[i] >> 4];
        hex_out[i * 2 + 1] = hex[out[i] & 0xF];
    }
    hex_out[64] = '\0';
    return ESP_OK;
}

/* ── File reader (LittleFS or SD) ──────────────────────────────────────── */

static char *s_read_log_file(const char *filename, size_t *out_size)
{
    char path[160];
    FILE *f = NULL;

    /* Try SD card first (matches data_logger preference), fall back to LFS. */
    snprintf(path, sizeof(path), "/sdcard/logs/%s", filename);
    f = fopen(path, "rb");
    if (!f) {
        snprintf(path, sizeof(path), "/lfs/logs/%s", filename);
        f = fopen(path, "rb");
    }
    if (!f) {
        ESP_LOGE(TAG, "Could not open log file '%s' (tried sdcard + lfs)", filename);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 10 * 1024 * 1024) {
        ESP_LOGE(TAG, "File '%s' has invalid size: %ld", filename, size);
        fclose(f);
        return NULL;
    }

    char *buf = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM);
    if (!buf) buf = malloc((size_t)size);
    if (!buf) {
        ESP_LOGE(TAG, "Out of memory reading %ld bytes", size);
        fclose(f);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        ESP_LOGE(TAG, "Short read on '%s': %u/%ld", filename, (unsigned)n, size);
        free(buf);
        return NULL;
    }

    *out_size = (size_t)size;
    return buf;
}

/* ── Worker task ───────────────────────────────────────────────────────── */

static void _upload_task(void *arg)
{
    upload_params_t *p = (upload_params_t *)arg;

    s_status_set(CAN_UPLOAD_RUNNING, 0, "Preparing upload...", 0);

    /* Read file into PSRAM */
    size_t file_size = 0;
    char *file_buf = s_read_log_file(p->filename, &file_size);
    if (!file_buf) {
        s_status_set(CAN_UPLOAD_FAILED, 0, "Could not read log file", 0);
        free(p);
        vTaskDelete(NULL);
        return;
    }

    /* Device ID */
    char device_id[MAX_SERIAL_LENGTH] = {0};
    if (get_device_serial(device_id) != ESP_OK || device_id[0] == '\0') {
        strncpy(device_id, "unknown", sizeof(device_id) - 1);
    }

    /* Timestamp (unix seconds) */
    int64_t now_us = esp_timer_get_time();
    (void)now_us;  /* unused — using wall clock if available */
    time_t now = 0;
    time(&now);
    if (now < 1700000000) {
        /* No NTP / RTC sync yet — server-side window will probably reject.
         * Use seconds-since-boot as a degenerate fallback so the request
         * still has a unique timestamp. */
        now = (time_t)(esp_timer_get_time() / 1000000);
    }

    /* Canonical HMAC message — must match worker exactly. */
    char msg[256];
    snprintf(msg, sizeof(msg), "%s\n%s\n%s\n%lld",
             p->make, p->model, device_id, (long long)now);

    char sig_hex[65];
    if (s_hmac_sha256_hex(RDM7_CAN_UPLOAD_HMAC_SECRET, msg, sig_hex) != ESP_OK) {
        s_status_set(CAN_UPLOAD_FAILED, 0, "HMAC computation failed", 0);
        free(file_buf);
        free(p);
        vTaskDelete(NULL);
        return;
    }

    /* HTTPS POST */
    esp_http_client_config_t cfg = {
        .url = RDM7_CAN_UPLOAD_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60 * 1000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        s_status_set(CAN_UPLOAD_FAILED, 0, "HTTP client init failed", 0);
        free(file_buf);
        free(p);
        vTaskDelete(NULL);
        return;
    }

    char ts_str[24];
    snprintf(ts_str, sizeof(ts_str), "%lld", (long long)now);

    esp_http_client_set_header(client, "X-Make",      p->make);
    esp_http_client_set_header(client, "X-Model",     p->model);
    esp_http_client_set_header(client, "X-Device-Id", device_id);
    esp_http_client_set_header(client, "X-Timestamp", ts_str);
    esp_http_client_set_header(client, "X-Signature", sig_hex);
    if (p->notes[0]) {
        esp_http_client_set_header(client, "X-Notes", p->notes);
    }
    esp_http_client_set_header(client, "Content-Type", "text/csv");
    esp_http_client_set_post_field(client, file_buf, (int)file_size);

    s_status_set(CAN_UPLOAD_RUNNING, 0, "Uploading...", 0);

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    if (err == ESP_OK && status_code >= 200 && status_code < 300) {
        char resp_msg[96];
        snprintf(resp_msg, sizeof(resp_msg), "Uploaded %u bytes (HTTP %d)",
                 (unsigned)file_size, status_code);
        s_status_set(CAN_UPLOAD_SUCCESS, status_code, resp_msg, (int)file_size);
        ESP_LOGI(TAG, "Upload OK: %s", resp_msg);
    } else {
        char err_msg[96];
        snprintf(err_msg, sizeof(err_msg), "HTTP %d (%s)", status_code,
                 err == ESP_OK ? "non-2xx" : esp_err_to_name(err));
        s_status_set(CAN_UPLOAD_FAILED, status_code, err_msg, 0);
        ESP_LOGE(TAG, "Upload failed: %s", err_msg);
    }

    esp_http_client_cleanup(client);
    free(file_buf);
    free(p);
    vTaskDelete(NULL);
}

/* ── Public API ────────────────────────────────────────────────────────── */

esp_err_t can_upload_start(const char *filename, const char *make,
                           const char *model, const char *notes)
{
    if (!filename || !make || !model) return ESP_ERR_INVALID_ARG;
    if (!filename[0] || !make[0] || !model[0]) return ESP_ERR_INVALID_ARG;
    if (strchr(filename, '/') || strchr(filename, '\\')) return ESP_ERR_INVALID_ARG;

    if (!s_status_mtx) {
        s_status_mtx = xSemaphoreCreateMutex();
        if (!s_status_mtx) return ESP_ERR_NO_MEM;
    }

    /* Reject overlapping starts. */
    can_upload_status_t snap;
    can_upload_get_status(&snap);
    if (snap.state == CAN_UPLOAD_RUNNING) return ESP_ERR_INVALID_STATE;

    upload_params_t *p = calloc(1, sizeof(*p));
    if (!p) return ESP_ERR_NO_MEM;
    strncpy(p->filename, filename, sizeof(p->filename) - 1);
    strncpy(p->make,     make,     sizeof(p->make) - 1);
    strncpy(p->model,    model,    sizeof(p->model) - 1);
    if (notes) strncpy(p->notes, notes, sizeof(p->notes) - 1);

    s_status_set(CAN_UPLOAD_RUNNING, 0, "Starting...", 0);

    /* 8 KB stack — mbedTLS handshake + esp_http_client both fit. Core 0
     * matches OTA pattern (LVGL on core 1, networking on core 0). */
    BaseType_t ok = xTaskCreatePinnedToCore(
        _upload_task, "can_upload", 8 * 1024, p, 3, NULL, 0);
    if (ok != pdPASS) {
        s_status_set(CAN_UPLOAD_FAILED, 0, "Task create failed", 0);
        free(p);
        return ESP_FAIL;
    }
    return ESP_OK;
}
