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
#include "system/rdm_lv_async.h"
#include "cJSON.h"
#include "can/obd2.h"
#include "can/obd2_dtc_db.h"
#include "storage/sd_manager.h"
#include "widgets/signal.h"
#include "widgets/signal_sim.h"
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

/* ── Blocking-request lifetime ────────────────────────────────────────────
 * Every handler in this file parks the httpd task on a binary semaphore
 * while an async chain runs on the LVGL task, sharing a heap ctx. On
 * semaphore timeout the chain is still in flight, so handlers used to
 * deliberately LEAK ctx + semaphore (freeing would be a use-after-free when
 * the late OBD2 callback finally wrote into it). That is a leak on EVERY
 * ECU-silent timeout — the common case on a car without OBD2 wired, polled
 * by the editor's OBD2 modal. Refcount instead: the handler and the chain
 * each hold one reference; whoever releases last frees. Never touch the ctx
 * after releasing your reference. */
static void _obd2_ctx_release(volatile int *refs, SemaphoreHandle_t done, void *ctx) {
    if (__atomic_fetch_sub(refs, 1, __ATOMIC_ACQ_REL) == 1) {
        vSemaphoreDelete(done);
        free(ctx);
    }
}

/* ── DTC read ─────────────────────────────────────────────────────────── */

typedef struct {
    SemaphoreHandle_t done;
    volatile int      refs;   /* 2 = httpd handler + async chain */
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
    _obd2_ctx_release(&ctx->refs, ctx->done, ctx);   /* chain's reference */
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
    ctx->refs = 2;
    ctx->mode = mode;

    rdm_async_call(_dtc_kick, ctx);
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

    _obd2_ctx_release(&ctx->refs, ctx->done, ctx);   /* handler's reference */

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_server_send_json(req, resp);
}

/* ── Clear DTCs ───────────────────────────────────────────────────────── */

typedef struct {
    SemaphoreHandle_t done;
    volatile int      refs;
    bool              ok;
} clear_ctx_t;

static void _clear_done(bool ok, void *user) {
    clear_ctx_t *ctx = (clear_ctx_t *)user;
    ctx->ok = ok;
    xSemaphoreGive(ctx->done);
    _obd2_ctx_release(&ctx->refs, ctx->done, ctx);
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

    ctx->refs = 2;
    rdm_async_call(_clear_kick, ctx);
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

    _obd2_ctx_release(&ctx->refs, ctx->done, ctx);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_server_send_json(req, resp);
}

/* ── VIN read ─────────────────────────────────────────────────────────── */

typedef struct {
    SemaphoreHandle_t done;
    volatile int      refs;
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
    _obd2_ctx_release(&ctx->refs, ctx->done, ctx);
}

static void _vin_kick(void *arg) {
    vin_ctx_t *ctx = (vin_ctx_t *)arg;
    obd2_read_vin(_vin_done, ctx);
}

/* ── ECU Name read ────────────────────────────────────────────────────
 * Mirrors the VIN path — same context shape, same async bridge. */

typedef struct {
    SemaphoreHandle_t done;
    volatile int      refs;
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
    _obd2_ctx_release(&ctx->refs, ctx->done, ctx);
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

    ctx->refs = 2;
    rdm_async_call(_ecuname_kick, ctx);
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

    _obd2_ctx_release(&ctx->refs, ctx->done, ctx);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_server_send_json(req, resp);
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

    ctx->refs = 2;
    rdm_async_call(_vin_kick, ctx);
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

    _obd2_ctx_release(&ctx->refs, ctx->done, ctx);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_server_send_json(req, resp);
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
    volatile int      refs;           /* 2 = httpd handler + async chain */
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
        rdm_async_call(_snap_kick_pending, ctx);
    } else if (mode == 0x07) {
        ctx->pending_ok = ok; ctx->pending_count = ok ? take : 0;
        if (ok && codes) memcpy(ctx->pending, codes, take * sizeof(obd2_dtc_t));
        rdm_async_call(_snap_kick_permanent, ctx);
    } else if (mode == 0x0A) {
        ctx->permanent_ok = ok; ctx->permanent_count = ok ? take : 0;
        if (ok && codes) memcpy(ctx->permanent, codes, take * sizeof(obd2_dtc_t));
        rdm_async_call(_snap_kick_vin, ctx);
    }
}

static void _snap_vin_done(bool ok, const char *vin, void *user) {
    snap_ctx_t *ctx = (snap_ctx_t *)user;
    ctx->vin_ok = ok;
    if (ok && vin) {
        strncpy(ctx->vin, vin, sizeof(ctx->vin) - 1);
    }
    /* Chain to ECU name — last OBD2 fetch in the snapshot chain. */
    rdm_async_call(_snap_kick_ecuname, ctx);
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
        _obd2_ctx_release(&ctx->refs, ctx->done, ctx);
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
    _obd2_ctx_release(&ctx->refs, ctx->done, ctx);   /* chain's reference */
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

    ctx->refs = 2;
    /* Start the chain. _snap_kick_stored -> Mode 03 -> (cb) -> Mode 07 ->
     * (cb) -> Mode 0A -> (cb) -> Mode 09 -> (cb) -> write file -> sem. */
    rdm_async_call(_snap_kick_stored, ctx);

    /* Generous 12 s budget: 5 OBD2 round-trips (Mode 03/07/0A + VIN +
     * ECU name) × 2 s each = 10 s, plus a couple of seconds for the SD
     * write. On timeout the in-flight chain still completes, writes into
     * the still-alive ctx, and drops the LAST reference — no leak. */
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

    _obd2_ctx_release(&ctx->refs, ctx->done, ctx);   /* handler's reference */

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_server_send_json(req, resp);
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

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_server_send_json(req, root);
}

static const httpd_uri_t protocols_uri = {
    .uri = "/api/obd2/protocols", .method = HTTP_GET,
    .handler = _protocols_handler, .user_ctx = NULL};

/* GET /api/obd2/pids — current polled-PID set.
 *
 * Returns the live "what is OBD2 actively polling right now?" list so the
 * web Setup-mode OBD2 stat ("3 PIDs polled" / "Not configured") doesn't
 * silently 404 and read as Not Configured even when polling is on.
 *
 * Response:
 *   { "pids": [
 *       {"service": 1,  "pid": 12},   // M01 PID 0x0C (RPM)
 *       {"service": 33, "pid": 128},  // M21 PID 0x80 (Toyota engine block)
 *       ...
 *   ] }
 */
static esp_err_t _pids_handler(httpd_req_t *req) {
    uint32_t enabled[OBD2_MAX_ENABLED];
    uint8_t  n = obd2_get_enabled(enabled, OBD2_MAX_ENABLED);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "pids");
    for (uint8_t i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "service", obd2_decode_service(enabled[i]));
        cJSON_AddNumberToObject(item, "pid",     obd2_decode_pid(enabled[i]));
        cJSON_AddItemToArray(arr, item);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_server_send_json(req, root);
}

static const httpd_uri_t pids_uri = {
    .uri = "/api/obd2/pids", .method = HTTP_GET,
    .handler = _pids_handler, .user_ctx = NULL};

/* POST /api/obd2/sim?on=0|1 — toggle the bench virtual-ECU simulator.
 *
 * Bench aid: with no real ECU on the bus, OBD2 can't be verified because
 * nothing answers a request. When enabled, the firmware synthesizes Mode 01
 * responses for every polled PID (see obd2_sim_set_enabled) so bound gauges
 * (fuel level etc.) visibly move and /api/obd2/pids + /protocols light up —
 * proving the whole channel→signal→widget pipeline with no car attached.
 *
 * Not persisted; default OFF on boot. Returns:
 *   { ok, sim, pids_polled, signal_sim_active }
 * `signal_sim_active:true` warns the user that the signal simulator is on,
 * which DROPS injected OBD2 responses — they must turn it off first. */
static esp_err_t _sim_handler(httpd_req_t *req) {
    /* GET → report current state (no change); POST ?on=0|1 → set it. The
     * web Developer Options modal GETs on open to reflect the live state. */
    bool is_get = (req->method == HTTP_GET);
    int on = -1;
    if (!is_get) {
        char q[48];
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            char v[8];
            if (httpd_query_key_value(q, "on", v, sizeof(v)) == ESP_OK) {
                on = atoi(v);
            }
        }
        if (on < 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Use ?on=0 or ?on=1");
            return ESP_FAIL;
        }
    }

    if (!rdm_lvgl_lock(1200)) {
        return web_server_send_busy(req);
    }
    if (!is_get) obd2_sim_set_enabled(on != 0);
    bool sim_on = obd2_sim_enabled();
    uint32_t enabled[OBD2_MAX_ENABLED];
    uint8_t  npids = obd2_get_enabled(enabled, OBD2_MAX_ENABLED);
    bool sigsim = signal_sim_is_active();
    rdm_lvgl_unlock();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject  (root, "ok", true);
    cJSON_AddBoolToObject  (root, "sim", sim_on);
    cJSON_AddNumberToObject(root, "pids_polled", npids);
    cJSON_AddBoolToObject  (root, "signal_sim_active", sigsim);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_server_send_json(req, root);
}

static const httpd_uri_t sim_uri = {
    .uri = "/api/obd2/sim", .method = HTTP_POST,
    .handler = _sim_handler, .user_ctx = NULL};

static const httpd_uri_t sim_get_uri = {
    .uri = "/api/obd2/sim", .method = HTTP_GET,
    .handler = _sim_handler, .user_ctx = NULL};

/* POST /api/obd2/scan — run the OBD2 discovery scan (with auto-search across
 * 500k/250k bitrates + 0x7DF/0x7E0 addressing) and return what the vehicle
 * supports. Same async-bridge pattern as VIN/ECU-name. Returns
 *   { ok, completed, count, pids:[...] }   (PIDs are Mode 01 PID bytes)
 * or { ok:false, error } on timeout. Generous wait: the auto-search may
 * bounce the CAN bitrate (~250 ms each) before giving up. */
typedef struct {
    SemaphoreHandle_t done;
    volatile int      refs;
    bool              completed;
    uint8_t           count;
    uint8_t           pids[OBD2_SCAN_MAX_PIDS];
} scan_ctx_t;

static void _scan_done_cb(const obd2_scan_result_t *r, void *user) {
    scan_ctx_t *ctx = (scan_ctx_t *)user;
    if (r) {
        ctx->completed = r->completed;
        ctx->count     = r->count;
        uint8_t n = (r->count < OBD2_SCAN_MAX_PIDS) ? r->count : OBD2_SCAN_MAX_PIDS;
        memcpy(ctx->pids, r->pids, n);
    }
    xSemaphoreGive(ctx->done);
    _obd2_ctx_release(&ctx->refs, ctx->done, ctx);
}

static void _scan_kick(void *arg) {
    obd2_discovery_start(_scan_done_cb, arg);
}

static esp_err_t _scan_handler(httpd_req_t *req) {
    scan_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM"); return ESP_FAIL; }
    ctx->done = xSemaphoreCreateBinary();
    if (!ctx->done) { free(ctx); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem alloc"); return ESP_FAIL; }

    ctx->refs = 2;
    rdm_async_call(_scan_kick, ctx);
    BaseType_t got = xSemaphoreTake(ctx->done, pdMS_TO_TICKS(8000));

    cJSON *resp = cJSON_CreateObject();
    if (got != pdTRUE) {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "error", "Scan timeout");
    } else {
        cJSON_AddBoolToObject  (resp, "ok", true);
        cJSON_AddBoolToObject  (resp, "completed", ctx->completed);
        cJSON_AddNumberToObject(resp, "count", ctx->count);
        cJSON *arr = cJSON_AddArrayToObject(resp, "pids");
        for (uint8_t i = 0; i < ctx->count && i < OBD2_SCAN_MAX_PIDS; i++) {
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(ctx->pids[i]));
        }
    }
    _obd2_ctx_release(&ctx->refs, ctx->done, ctx);

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return web_server_send_json(req, resp);
}

static const httpd_uri_t scan_uri = {
    .uri = "/api/obd2/scan", .method = HTTP_POST,
    .handler = _scan_handler, .user_ctx = NULL};

void web_server_obd2_register(httpd_handle_t server) {
    REGISTER_URI(server, &dtcs_uri);
    REGISTER_URI(server, &clear_uri);
    REGISTER_URI(server, &vin_uri);
    REGISTER_URI(server, &ecuname_uri);
    REGISTER_URI(server, &snapshot_uri);
    REGISTER_URI(server, &protocols_uri);
    REGISTER_URI(server, &pids_uri);
    REGISTER_URI(server, &sim_uri);
    REGISTER_URI(server, &sim_get_uri);
    REGISTER_URI(server, &scan_uri);
}
