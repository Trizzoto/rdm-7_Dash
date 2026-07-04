/**
 * serial_commands_logger.c — data logger + signal replay serial JSON-RPC
 * handlers.
 *
 * Methods: log.start, log.stop, log.status, log.list, log.delete,
 * log.download.start, log.download.chunk, log.config.get, log.config.set,
 * replay.start, replay.stop, replay.status.
 *
 * Owns the private deferred LVGL-task helpers for log start/stop/rate-set
 * and replay start/stop.
 */
#include "serial_commands_internal.h"
#include "system/rdm_lv_async.h"
#include "uart_protocol.h"
#include "serial_protocol.h"

#include "cJSON.h"
#include "esp_log.h"
#include "storage/data_logger.h"
#include "storage/signal_replay.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

/* ── Data Logger serial commands ────────────────────────────────────────── */

/* Optional rate carried via heap-allocated payload to the LVGL task. */
typedef struct {
    uint16_t rate_hz;
    bool     persist;
} serial_log_start_args_t;

static void _deferred_serial_log_start(void *arg)
{
    serial_log_start_args_t *a = (serial_log_start_args_t *)arg;
    if (a) {
        data_logger_start_with_rate(a->rate_hz, a->persist);
        free(a);
    } else {
        data_logger_start();
    }
}

void _handle_log_start(int id, cJSON *params)
{
    /* Optional params: { "rate_hz": N, "persist": true|false } */
    serial_log_start_args_t *a = NULL;
    if (params) {
        cJSON *rate = cJSON_GetObjectItemCaseSensitive(params, "rate_hz");
        cJSON *pers = cJSON_GetObjectItemCaseSensitive(params, "persist");
        if (cJSON_IsNumber(rate)) {
            a = (serial_log_start_args_t *)calloc(1, sizeof(*a));
            if (a) {
                int v = rate->valueint;
                if (v < 0)    v = 0;
                if (v > 1000) v = 1000;
                a->rate_hz = (uint16_t)v;
                a->persist = cJSON_IsBool(pers) ? cJSON_IsTrue(pers) : true;
            }
        }
    }
    rdm_async_call(_deferred_serial_log_start, a);
    _send_ok(id);
}

static void _deferred_serial_log_stop(void *arg)
{
    (void)arg;
    data_logger_stop();
}

void _handle_log_stop(int id, cJSON *params)
{
    (void)params;
    rdm_async_call(_deferred_serial_log_stop, NULL);
    _send_ok(id);
}

/* lv_async_call shim for runtime rate changes via "log.config.set". */
static void _deferred_serial_log_set_rate(void *arg)
{
    uint16_t *p = (uint16_t *)arg;
    if (p) {
        data_logger_set_rate_hz(*p);
        free(p);
    }
}

void _handle_log_status(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "active", data_logger_is_active());
    cJSON_AddStringToObject(r, "file", data_logger_current_file());
    cJSON_AddNumberToObject(r, "samples", data_logger_get_sample_count());
    cJSON_AddNumberToObject(r, "elapsed_ms", data_logger_get_elapsed_ms());
    cJSON_AddNumberToObject(r, "rate_hz", data_logger_get_rate_hz());
    _send_response(id, r, NULL);
}

/* GET current rate. */
void _handle_log_config_get(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    uint16_t hz = data_logger_get_rate_hz();
    cJSON_AddNumberToObject(r, "rate_hz", hz);
    cJSON_AddBoolToObject(r, "is_max", hz == 0);
    _send_response(id, r, NULL);
}

/* SET rate via params: { "rate_hz": N }. 0 = Max. */
void _handle_log_config_set(int id, cJSON *params)
{
    if (!params) {
        _send_error(id, "params required");
        return;
    }
    cJSON *rate = cJSON_GetObjectItemCaseSensitive(params, "rate_hz");
    if (!cJSON_IsNumber(rate)) {
        _send_error(id, "rate_hz (number) required");
        return;
    }
    int v = rate->valueint;
    if (v < 0)    v = 0;
    if (v > 1000) v = 1000;
    uint16_t *arg = (uint16_t *)malloc(sizeof(uint16_t));
    if (!arg) { _send_error(id, "OOM"); return; }
    *arg = (uint16_t)v;
    rdm_async_call(_deferred_serial_log_set_rate, arg);
    _send_ok(id);
}

/* ── Signal Replay (CSV playback through signal system) ─────────────────── */

typedef struct {
    char  path[96];
    float speed;
    bool  loop;
} serial_replay_args_t;

static void _deferred_serial_replay_start(void *arg)
{
    serial_replay_args_t *a = (serial_replay_args_t *)arg;
    if (a) {
        signal_replay_start(a->path, a->speed, a->loop);
        free(a);
    }
}

static void _deferred_serial_replay_stop(void *arg)
{
    (void)arg;
    signal_replay_stop();
}

void _handle_replay_start(int id, cJSON *params)
{
    if (!params) { _send_error(id, "params required"); return; }
    cJSON *file_item  = cJSON_GetObjectItemCaseSensitive(params, "file");
    cJSON *speed_item = cJSON_GetObjectItemCaseSensitive(params, "speed");
    cJSON *loop_item  = cJSON_GetObjectItemCaseSensitive(params, "loop");
    if (!cJSON_IsString(file_item) || !file_item->valuestring) {
        _send_error(id, "file (string) required");
        return;
    }
    serial_replay_args_t *a = (serial_replay_args_t *)calloc(1, sizeof(*a));
    if (!a) { _send_error(id, "OOM"); return; }
    const char *fn = file_item->valuestring;
    if (fn[0] == '/') {
        strncpy(a->path, fn, sizeof(a->path) - 1);
    } else {
        snprintf(a->path, sizeof(a->path), "/sdcard/logs/%s", fn);
    }
    a->speed = cJSON_IsNumber(speed_item) ? (float)speed_item->valuedouble : 1.0f;
    a->loop  = cJSON_IsBool(loop_item) && cJSON_IsTrue(loop_item);
    rdm_async_call(_deferred_serial_replay_start, a);
    _send_ok(id);
}

void _handle_replay_stop(int id, cJSON *params)
{
    (void)params;
    rdm_async_call(_deferred_serial_replay_stop, NULL);
    _send_ok(id);
}

void _handle_replay_status(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "active", signal_replay_is_active());
    cJSON_AddStringToObject(r, "file", signal_replay_get_file());
    cJSON_AddNumberToObject(r, "row", signal_replay_get_row());
    cJSON_AddNumberToObject(r, "total_rows", signal_replay_get_total_rows());
    cJSON_AddNumberToObject(r, "speed", (double)signal_replay_get_speed());
    _send_response(id, r, NULL);
}

void _handle_log_list(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateArray();
    DIR *d = opendir("/sdcard/logs");
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_type == DT_REG) {
                cJSON *item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", ent->d_name);
                char path[128];
                snprintf(path, sizeof(path), "/sdcard/logs/%s", ent->d_name);
                struct stat st;
                if (stat(path, &st) == 0)
                    cJSON_AddNumberToObject(item, "size", st.st_size);
                cJSON_AddItemToArray(r, item);
            }
        }
        closedir(d);
    }
    _send_response(id, r, NULL);
}

void _handle_log_delete(int id, cJSON *params)
{
    cJSON *j = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(j) || !_filename_is_safe(j->valuestring)) {
        _send_error(id, "Invalid name");
        return;
    }
    char path[128];
    snprintf(path, sizeof(path), "/sdcard/logs/%s", j->valuestring);
    if (remove(path) != 0) {
        _send_error(id, "Delete failed");
        return;
    }
    _send_ok(id);
}

/* ── Log download (chunked binary) ─────────────────────────────────────── */

void _handle_log_download_start(int id, cJSON *params)
{
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name_item) || !_filename_is_safe(name_item->valuestring)) {
        _send_error(id, "Invalid name"); return;
    }
    char path[128];
    snprintf(path, sizeof(path), "/sdcard/logs/%s", name_item->valuestring);
    struct stat st;
    if (stat(path, &st) != 0) { _send_error(id, "File not found"); return; }

    uint32_t file_size = (uint32_t)st.st_size;
    uint16_t total_chunks = (uint16_t)((file_size + DOWNLOAD_CHUNK_SIZE - 1)
                                       / DOWNLOAD_CHUNK_SIZE);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "name", name_item->valuestring);
    cJSON_AddNumberToObject(r, "size", file_size);
    cJSON_AddNumberToObject(r, "chunks", total_chunks);
    cJSON_AddNumberToObject(r, "chunk_size", DOWNLOAD_CHUNK_SIZE);
    _send_response(id, r, NULL);
}

void _handle_log_download_chunk(int id, cJSON *params)
{
    cJSON *name_item  = cJSON_GetObjectItem(params, "name");
    cJSON *index_item = cJSON_GetObjectItem(params, "index");
    if (!cJSON_IsString(name_item) || !cJSON_IsNumber(index_item)
        || !_filename_is_safe(name_item->valuestring)) {
        _send_error(id, "Invalid params"); return;
    }

    char path[128];
    snprintf(path, sizeof(path), "/sdcard/logs/%s", name_item->valuestring);
    FILE *f = fopen(path, "rb");
    if (!f) { _send_error(id, "File not found"); return; }

    uint8_t *buf = malloc(1 + DOWNLOAD_CHUNK_SIZE);
    if (!buf) { fclose(f); _send_error(id, "OOM"); return; }

    uint32_t offset = (uint32_t)index_item->valueint * DOWNLOAD_CHUNK_SIZE;
    fseek(f, offset, SEEK_SET);
    buf[0] = UART_PAYLOAD_BINARY;
    size_t nr = fread(buf + 1, 1, DOWNLOAD_CHUNK_SIZE, f);
    fclose(f);

    if (nr == 0) { free(buf); _send_error(id, "Read past end"); return; }

    serial_protocol_send_frame(buf, 1 + nr);
    free(buf);
}
