/* web_server_obd2.c — OBD2 diagnostic endpoints
 *
 * GET  /api/obd2/dtcs?mode=stored|pending|permanent
 *   Returns { ok, mode, count, codes:[{code, desc}, ...] }
 *   Default mode=stored. Synchronous: blocks the HTTP handler on a
 *   semaphore while the OBD2 ISO-TP request/response round-trips.
 *
 * POST /api/obd2/clear
 *   Returns { ok, error? }. Fires Mode 04 (clear stored + pending DTCs).
 *   Most ECUs require engine off — error string surfaces ECU rejection.
 *
 * GET  /api/obd2/vin
 *   Returns { ok, vin }. Fires Mode 09 PID 0x02.
 *
 * Same async-bridge pattern as /api/obd2/test_pid (web_server_signals.c):
 * heap-allocated context, lv_async_call hands off to LVGL task to fire
 * the OBD2 request, FreeRTOS semaphore blocks the HTTP handler until the
 * obd2 callback fires (or a generous wall-clock timeout expires). On
 * timeout we leak the context (small + bounded) so a late callback can't
 * UAF after we've returned.
 */
#include "web_server_internal.h"
#include "cJSON.h"
#include "can/obd2.h"
#include "can/obd2_dtc_db.h"
#include "storage/sd_manager.h"
#include "widgets/signal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── DTC read ─────────────────────────────────────────────────────────── */

typedef struct {
    SemaphoreHandle_t done;
    bool              ok;
    uint8_t           mode;
    uint8_t           count;
    obd2_dtc_t        codes[OBD2_MAX_DTCS];
} dtc_ctx_t;

static void _dtc_done(bool ok, const obd2_dtc_t *codes, uint8_t count,
                       uint8_t mode, void *user) {
    dtc_ctx_t *ctx = (dtc_ctx_t *)user;
    ctx->ok    = ok;
    ctx->mode  = mode;
    ctx->count = (count > OBD2_MAX_DTCS) ? OBD2_MAX_DTCS : count;
    if (ok && codes && ctx->count > 0) {
        memcpy(ctx->codes, codes, ctx->count * sizeof(obd2_dtc_t));
    }
    xSemaphoreGive(ctx->done);
}

static void _dtc_kick(void *arg) {
    dtc_ctx_t *ctx = (dtc_ctx_t *)arg;
    switch (ctx->mode) {
        case 0x03: obd2_read_stored_dtcs(_dtc_done, ctx);    break;
        case 0x07: obd2_read_pending_dtcs(_dtc_done, ctx);   break;
        case 0x0A: obd2_read_permanent_dtcs(_dtc_done, ctx); break;
        default:
            /* unknown mode — feed it back via the callback path so the
             * HTTP handler unblocks cleanly */
            _dtc_done(false, NULL, 0, ctx->mode, ctx);
            break;
    }
}

static esp_err_t _dtcs_handler(httpd_req_t *req) {
    /* Parse mode= from query string; default stored. */
    uint8_t mode = 0x03;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "mode", val, sizeof(val)) == ESP_OK) {
            if      (strcmp(val, "pending")   == 0) mode = 0x07;
            else if (strcmp(val, "permanent") == 0) mode = 0x0A;
            else if (strcmp(val, "stored")    == 0) mode = 0x03;
        }
    }

    dtc_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(ctx);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem alloc");
        return ESP_FAIL;
    }
    ctx->mode = mode;

    lv_async_call(_dtc_kick, ctx);
    /* obd2 DTC timeout is 2 s — give 3 s HTTP wait as upper bound. */
    BaseType_t got = xSemaphoreTake(ctx->done, pdMS_TO_TICKS(3000));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "mode", mode);
    const char *mode_str = (mode == 0x07) ? "pending"
                          : (mode == 0x0A) ? "permanent" : "stored";
    cJSON_AddStringToObject(resp, "mode_name", mode_str);

    if (got != pdTRUE) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "Internal timeout");
        cJSON_AddNumberToObject(resp, "count", 0);
        cJSON_AddArrayToObject(resp, "codes");
    } else if (!ctx->ok) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "No response from ECU");
        cJSON_AddNumberToObject(resp, "count", 0);
        cJSON_AddArrayToObject(resp, "codes");
    } else {
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddNumberToObject(resp, "count", ctx->count);
        cJSON *arr = cJSON_AddArrayToObject(resp, "codes");
        for (uint8_t i = 0; i < ctx->count; i++) {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "code", ctx->codes[i].code);
            const char *desc = obd2_dtc_lookup(ctx->codes[i].code);
            cJSON_AddStringToObject(entry, "desc", desc ? desc : "");
            cJSON_AddItemToArray(arr, entry);
        }
    }

    if (got == pdTRUE) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
    }
    /* If got != pdTRUE we leak ctx — see header comment. */

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t r = httpd_resp_sendstr(req, json);
    free(json);
    return r;
}

/* ── Clear DTCs ───────────────────────────────────────────────────────── */

typedef struct {
    SemaphoreHandle_t done;
    bool              ok;
} clear_ctx_t;

static void _clear_done(bool ok, void *user) {
    clear_ctx_t *ctx = (clear_ctx_t *)user;
    ctx->ok = ok;
    xSemaphoreGive(ctx->done);
}

static void _clear_kick(void *arg) {
    clear_ctx_t *ctx = (clear_ctx_t *)arg;
    obd2_clear_dtcs(_clear_done, ctx);
}

static esp_err_t _clear_handler(httpd_req_t *req) {
    clear_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(ctx);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem alloc");
        return ESP_FAIL;
    }

    lv_async_call(_clear_kick, ctx);
    BaseType_t got = xSemaphoreTake(ctx->done, pdMS_TO_TICKS(2500));

    cJSON *resp = cJSON_CreateObject();
    if (got != pdTRUE) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "Internal timeout");
    } else if (!ctx->ok) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error",
            "ECU rejected clear (try with engine off + ignition on)");
    } else {
        cJSON_AddBoolToObject(resp, "ok", true);
    }

    if (got == pdTRUE) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
    }

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t r = httpd_resp_sendstr(req, json);
    free(json);
    return r;
}

/* ── VIN read ─────────────────────────────────────────────────────────── */

typedef struct {
    SemaphoreHandle_t done;
    bool              ok;
    char              vin[20];
} vin_ctx_t;

static void _vin_done(bool ok, const char *vin, void *user) {
    vin_ctx_t *ctx = (vin_ctx_t *)user;
    ctx->ok = ok;
    if (ok && vin) {
        strncpy(ctx->vin, vin, sizeof(ctx->vin) - 1);
        ctx->vin[sizeof(ctx->vin) - 1] = '\0';
    }
    xSemaphoreGive(ctx->done);
}

static void _vin_kick(void *arg) {
    vin_ctx_t *ctx = (vin_ctx_t *)arg;
    obd2_read_vin(_vin_done, ctx);
}

/* ── ECU Name read ────────────────────────────────────────────────────
 * Mirrors the VIN path — same context shape, same async bridge. */

typedef struct {
    SemaphoreHandle_t done;
    bool              ok;
    char              name[24];
} ecuname_ctx_t;

static void _ecuname_done(bool ok, const char *name, void *user) {
    ecuname_ctx_t *ctx = (ecuname_ctx_t *)user;
    ctx->ok = ok;
    if (ok && name) {
        strncpy(ctx->name, name, sizeof(ctx->name) - 1);
        ctx->name[sizeof(ctx->name) - 1] = '\0';
    }
    xSemaphoreGive(ctx->done);
}

static void _ecuname_kick(void *arg) {
    ecuname_ctx_t *ctx = (ecuname_ctx_t *)arg;
    obd2_read_ecu_name(_ecuname_done, ctx);
}

static esp_err_t _ecuname_handler(httpd_req_t *req) {
    ecuname_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(ctx);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem alloc");
        return ESP_FAIL;
    }

    lv_async_call(_ecuname_kick, ctx);
    BaseType_t got = xSemaphoreTake(ctx->done, pdMS_TO_TICKS(2500));

    cJSON *resp = cJSON_CreateObject();
    if (got != pdTRUE) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "Internal timeout");
    } else if (!ctx->ok) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "No response from ECU");
    } else {
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "ecu_name", ctx->name);
    }

    if (got == pdTRUE) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
    }

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t r = httpd_resp_sendstr(req, json);
    free(json);
    return r;
}

static esp_err_t _vin_handler(httpd_req_t *req) {
    vin_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(ctx);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem alloc");
        return ESP_FAIL;
    }

    lv_async_call(_vin_kick, ctx);
    BaseType_t got = xSemaphoreTake(ctx->done, pdMS_TO_TICKS(2500));

    cJSON *resp = cJSON_CreateObject();
    if (got != pdTRUE) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "Internal timeout");
    } else if (!ctx->ok) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "No response from ECU");
    } else {
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "vin", ctx->vin);
    }

    if (got == pdTRUE) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
    }

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t r = httpd_resp_sendstr(req, json);
    free(json);
    return r;
}

/* ── Diagnostic snapshot ───────────────────────────────────────────────
 *
 * POST /api/obd2/snapshot — captures stored+pending+permanent DTCs, VIN,
 * and a live snapshot of every signal in the registry into a single
 * timestamped JSON file on the SD card. Designed for sharing with a
 * mechanic: a single artifact that summarises everything the dash sees.
 *
 * Chains Mode 03 -> Mode 07 -> Mode 0A -> Mode 09 sequentially because
 * obd2 only allows one DTC/VIN request in flight at a time. Each
 * callback queues the next request via lv_async_call to keep the
 * orchestration on the LVGL task.
 *
 * Total wall-clock: ~4-8 s (4 OBD2 round trips at up to 2 s each +
 * SD write). HTTP handler waits on a single semaphore for the whole
 * chain to complete.
 *
 * On success returns { ok:true, path, bytes }. The path can then be
 * downloaded via /api/sd/file or similar (existing endpoint elsewhere). */

typedef struct {
    SemaphoreHandle_t done;
    int               stage;          /* 0..4: which step is in flight */
    bool              all_ok;         /* false if ANY step failed (file still written) */
    /* Buffered results */
    bool        stored_ok;     uint8_t    stored_count;    obd2_dtc_t stored[OBD2_MAX_DTCS];
    bool        pending_ok;    uint8_t    pending_count;   obd2_dtc_t pending[OBD2_MAX_DTCS];
    bool        permanent_ok;  uint8_t    permanent_count; obd2_dtc_t permanent[OBD2_MAX_DTCS];
    bool        vin_ok;        char       vin[20];
    bool        ecuname_ok;    char       ecu_name[24];
    /* Output */
    char        out_path[96];
    size_t      out_bytes;
} snap_ctx_t;

/* Forward decls for state machine */
static void _snap_kick_stored(void *arg);
static void _snap_kick_pending(void *arg);
static void _snap_kick_permanent(void *arg);
static void _snap_kick_vin(void *arg);
static void _snap_kick_ecuname(void *arg);
static void _snap_write_and_done(snap_ctx_t *ctx);

static void _snap_stage_dtc_done(bool ok, const obd2_dtc_t *codes, uint8_t count,
                                  uint8_t mode, void *user) {
    snap_ctx_t *ctx = (snap_ctx_t *)user;
    uint8_t take = (count > OBD2_MAX_DTCS) ? OBD2_MAX_DTCS : count;
    if (mode == 0x03) {
        ctx->stored_ok = ok; ctx->stored_count = ok ? take : 0;
        if (ok && codes) memcpy(ctx->stored, codes, take * sizeof(obd2_dtc_t));
        lv_async_call(_snap_kick_pending, ctx);
    } else if (mode == 0x07) {
        ctx->pending_ok = ok; ctx->pending_count = ok ? take : 0;
        if (ok && codes) memcpy(ctx->pending, codes, take * sizeof(obd2_dtc_t));
        lv_async_call(_snap_kick_permanent, ctx);
    } else if (mode == 0x0A) {
        ctx->permanent_ok = ok; ctx->permanent_count = ok ? take : 0;
        if (ok && codes) memcpy(ctx->permanent, codes, take * sizeof(obd2_dtc_t));
        lv_async_call(_snap_kick_vin, ctx);
    }
}

static void _snap_vin_done(bool ok, const char *vin, void *user) {
    snap_ctx_t *ctx = (snap_ctx_t *)user;
    ctx->vin_ok = ok;
    if (ok && vin) {
        strncpy(ctx->vin, vin, sizeof(ctx->vin) - 1);
    }
    /* Chain to ECU name — last OBD2 fetch in the snapshot chain. */
    lv_async_call(_snap_kick_ecuname, ctx);
}

static void _snap_ecuname_done(bool ok, const char *name, void *user) {
    snap_ctx_t *ctx = (snap_ctx_t *)user;
    ctx->ecuname_ok = ok;
    if (ok && name) {
        strncpy(ctx->ecu_name, name, sizeof(ctx->ecu_name) - 1);
    }
    /* Final stage — write the file. Live signal snapshot happens here
     * too because it needs the LVGL task (signal registry is
     * single-threaded). */
    _snap_write_and_done(ctx);
}

static void _snap_kick_stored(void *arg) {
    obd2_read_stored_dtcs(_snap_stage_dtc_done, arg);
}
static void _snap_kick_pending(void *arg) {
    obd2_read_pending_dtcs(_snap_stage_dtc_done, arg);
}
static void _snap_kick_permanent(void *arg) {
    obd2_read_permanent_dtcs(_snap_stage_dtc_done, arg);
}
static void _snap_kick_vin(void *arg) {
    obd2_read_vin(_snap_vin_done, arg);
}
static void _snap_kick_ecuname(void *arg) {
    obd2_read_ecu_name(_snap_ecuname_done, arg);
}

/* Build the JSON document, write it to SD, fill out_path/out_bytes,
 * release the semaphore. Runs on LVGL task. */
static void _snap_write_and_done(snap_ctx_t *ctx) {
    cJSON *root = cJSON_CreateObject();
    int64_t now_us = esp_timer_get_time();
    cJSON_AddNumberToObject(root, "uptime_s", (double)now_us / 1000000.0);
    cJSON_AddStringToObject(root, "vin", ctx->vin_ok ? ctx->vin : "");
    cJSON_AddStringToObject(root, "ecu_name", ctx->ecuname_ok ? ctx->ecu_name : "");

    /* DTC sections — one array per bucket. */
    const struct { const char *name; bool ok; uint8_t count; obd2_dtc_t *codes; } buckets[] = {
        { "stored",    ctx->stored_ok,    ctx->stored_count,    ctx->stored    },
        { "pending",   ctx->pending_ok,   ctx->pending_count,   ctx->pending   },
        { "permanent", ctx->permanent_ok, ctx->permanent_count, ctx->permanent },
    };
    cJSON *dtcs = cJSON_AddObjectToObject(root, "dtcs");
    for (size_t b = 0; b < sizeof(buckets)/sizeof(buckets[0]); b++) {
        cJSON *bucket = cJSON_AddObjectToObject(dtcs, buckets[b].name);
        cJSON_AddBoolToObject(bucket, "ok", buckets[b].ok);
        cJSON *arr = cJSON_AddArrayToObject(bucket, "codes");
        for (uint8_t i = 0; i < buckets[b].count; i++) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "code", buckets[b].codes[i].code);
            const char *desc = obd2_dtc_lookup(buckets[b].codes[i].code);
            cJSON_AddStringToObject(e, "desc", desc ? desc : "");
            cJSON_AddItemToArray(arr, e);
        }
    }

    /* Live signal snapshot — every registered signal with current value
     * and stale flag. Order matches registry index. */
    cJSON *signals = cJSON_AddArrayToObject(root, "signals");
    uint16_t scount = signal_get_count();
    for (uint16_t i = 0; i < scount; i++) {
        signal_t *sig = signal_get_by_index(i);
        if (!sig) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", sig->name);
        cJSON_AddNumberToObject(e, "value", (double)sig->current_value);
        cJSON_AddStringToObject(e, "unit", sig->unit);
        cJSON_AddBoolToObject(e, "stale", sig->is_stale);
        if (sig->can_id) cJSON_AddNumberToObject(e, "can_id", (double)sig->can_id);
        cJSON_AddItemToArray(signals, e);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        ctx->all_ok = false;
        ctx->out_path[0] = '\0';
        ctx->out_bytes = 0;
        xSemaphoreGive(ctx->done);
        return;
    }

    /* Filename: /sdcard/diagnostics/snap_<uptime_s>.json — uptime instead
     * of wall-clock because the dash often has no RTC and no time sync at
     * the moment a snapshot is captured (think: car in driveway, no WiFi). */
    time_t epoch = (time_t)(now_us / 1000000LL);
    bool have_time = (epoch > 1700000000);  /* RTC has been set */
    if (sd_manager_is_mounted()) {
        mkdir("/sdcard/diagnostics", 0755);
        if (have_time) {
            struct tm tm_local;
            localtime_r(&epoch, &tm_local);
            snprintf(ctx->out_path, sizeof(ctx->out_path),
                     "/sdcard/diagnostics/snap_%04d%02d%02d_%02d%02d%02d.json",
                     tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
                     tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec);
        } else {
            snprintf(ctx->out_path, sizeof(ctx->out_path),
                     "/sdcard/diagnostics/snap_uptime_%lld.json",
                     (long long)epoch);
        }
        /* POSIX open instead of fopen — same heap rationale as
         * layout_manager (no per-FILE newlib lock allocation). */
        int fd = open(ctx->out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            size_t len = strlen(json);
            size_t total = 0;
            while (total < len) {
                ssize_t n = write(fd, json + total, len - total);
                if (n <= 0) break;
                total += (size_t)n;
            }
            close(fd);
            ctx->out_bytes = total;
            ctx->all_ok = (total == len);
        } else {
            ctx->all_ok = false;
            ctx->out_bytes = 0;
            ctx->out_path[0] = '\0';
        }
    } else {
        ctx->all_ok = false;
        ctx->out_bytes = 0;
        ctx->out_path[0] = '\0';
    }
    free(json);
    xSemaphoreGive(ctx->done);
}

static esp_err_t _snapshot_handler(httpd_req_t *req) {
    snap_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) {
        free(ctx);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem alloc");
        return ESP_FAIL;
    }

    /* Start the chain. _snap_kick_stored -> Mode 03 -> (cb) -> Mode 07 ->
     * (cb) -> Mode 0A -> (cb) -> Mode 09 -> (cb) -> write file -> sem. */
    lv_async_call(_snap_kick_stored, ctx);

    /* Generous 12 s budget: 5 OBD2 round-trips (Mode 03/07/0A + VIN +
     * ECU name) × 2 s each = 10 s, plus a couple of seconds for the SD
     * write. If we time out, the in-flight obd2 request will still
     * complete and harmlessly write into the leaked context. */
    BaseType_t got = xSemaphoreTake(ctx->done, pdMS_TO_TICKS(12000));

    cJSON *resp = cJSON_CreateObject();
    if (got != pdTRUE) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "Snapshot timed out");
    } else if (!ctx->all_ok || ctx->out_path[0] == '\0') {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error",
            sd_manager_is_mounted() ? "Write failed" : "No SD card mounted");
    } else {
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "path", ctx->out_path);
        cJSON_AddNumberToObject(resp, "bytes", (double)ctx->out_bytes);
        /* Include the summary so the caller can render a "you've got
         * N stored DTCs" toast without a second roundtrip. */
        cJSON *summary = cJSON_AddObjectToObject(resp, "summary");
        cJSON_AddNumberToObject(summary, "stored",    ctx->stored_count);
        cJSON_AddNumberToObject(summary, "pending",   ctx->pending_count);
        cJSON_AddNumberToObject(summary, "permanent", ctx->permanent_count);
        cJSON_AddStringToObject(summary, "vin", ctx->vin_ok ? ctx->vin : "");
        cJSON_AddStringToObject(summary, "ecu_name", ctx->ecuname_ok ? ctx->ecu_name : "");
    }

    if (got == pdTRUE) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
    }

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t r = httpd_resp_sendstr(req, json);
    free(json);
    return r;
}

/* ── URI descriptors ─────────────────────────────────────────────────── */

static const httpd_uri_t dtcs_uri = {
    .uri = "/api/obd2/dtcs", .method = HTTP_GET,
    .handler = _dtcs_handler, .user_ctx = NULL};
static const httpd_uri_t clear_uri = {
    .uri = "/api/obd2/clear", .method = HTTP_POST,
    .handler = _clear_handler, .user_ctx = NULL};
static const httpd_uri_t vin_uri = {
    .uri = "/api/obd2/vin", .method = HTTP_GET,
    .handler = _vin_handler, .user_ctx = NULL};
static const httpd_uri_t ecuname_uri = {
    .uri = "/api/obd2/ecuname", .method = HTTP_GET,
    .handler = _ecuname_handler, .user_ctx = NULL};
static const httpd_uri_t snapshot_uri = {
    .uri = "/api/obd2/snapshot", .method = HTTP_POST,
    .handler = _snapshot_handler, .user_ctx = NULL};

/* GET /api/obd2/protocols — per-service live-response status.
 *
 * Returns the live "is the ECU answering this protocol?" state for the
 * eight services the firmware tracks. Used by the OBD2 Setup modal to
 * draw a blue dot next to each mode the ECU is currently responding to,
 * helping users diagnose partial OBD2 connectivity ("Trouble Codes
 * works but Vehicle Info times out — why?").
 *
 * Response:
 *   {
 *     "fresh_window_ms": 5000,
 *     "protocols": [
 *       {"service": 1,  "name": "M01", "fresh": true,  "age_ms": 320},
 *       {"service": 2,  "name": "M02", "fresh": false, "age_ms": null},
 *       {"service": 3,  "name": "M03", "fresh": false, "age_ms": null},
 *       ...
 *     ]
 *   }
 *
 * age_ms is null when the service has never responded. */
static esp_err_t _protocols_handler(httpd_req_t *req) {
    /* Services tracked: 0x01 live data, 0x02 freeze frame, 0x03 stored DTCs,
     * 0x07 pending DTCs, 0x09 vehicle info (VIN/ECUname), 0x0A permanent
     * DTCs, 0x21 manufacturer-specific (Toyota etc.), 0x22 UDS read-by-id
     * (Ford/GM/VW/newer Toyota). */
    static const struct { uint8_t svc; const char *name; } SERVICES[] = {
        {0x01, "M01"}, {0x02, "M02"}, {0x03, "M03"}, {0x07, "M07"},
        {0x09, "M09"}, {0x0A, "M0A"}, {0x21, "M21"}, {0x22, "M22"},
    };
    const int n_services = (int)(sizeof(SERVICES) / sizeof(SERVICES[0]));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "fresh_window_ms", OBD2_PROTOCOL_FRESH_WINDOW_MS);
    cJSON *arr = cJSON_AddArrayToObject(root, "protocols");
    for (int i = 0; i < n_services; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "service", SERVICES[i].svc);
        cJSON_AddStringToObject(item, "name", SERVICES[i].name);
        cJSON_AddBoolToObject  (item, "fresh", obd2_protocol_is_fresh(SERVICES[i].svc));
        uint32_t age = obd2_protocol_age_ms(SERVICES[i].svc);
        if (age == UINT32_MAX) cJSON_AddNullToObject(item, "age_ms");
        else                   cJSON_AddNumberToObject(item, "age_ms", (double)age);
        cJSON_AddItemToArray(arr, item);
    }

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc"); return ESP_FAIL; }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    esp_err_t r = httpd_resp_send(req, s, strlen(s));
    free(s);
    return r;
}

static const httpd_uri_t protocols_uri = {
    .uri = "/api/obd2/protocols", .method = HTTP_GET,
    .handler = _protocols_handler, .user_ctx = NULL};

void web_server_obd2_register(httpd_handle_t server) {
    REGISTER_URI(server, &dtcs_uri);
    REGISTER_URI(server, &clear_uri);
    REGISTER_URI(server, &vin_uri);
    REGISTER_URI(server, &ecuname_uri);
    REGISTER_URI(server, &snapshot_uri);
    REGISTER_URI(server, &protocols_uri);
}
