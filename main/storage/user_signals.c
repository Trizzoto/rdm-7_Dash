/*
 * user_signals.c - see user_signals.h.
 *
 * Fixed-size array (USER_SIGNALS_MAX entries) backed by /lfs/dbc/user.json.
 * Boot loads the file into the array; appends rewrite the whole file.
 * 64 entries is plenty for typical user use cases; bump the cap if it
 * becomes a constraint.
 */
#include "storage/user_signals.h"
#include "cJSON.h"
#include "esp_log.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "user_signals";

#define USER_SIGNALS_DIR  "/lfs/dbc"
#define USER_SIGNALS_PATH USER_SIGNALS_DIR "/user.json"
#define USER_SIGNALS_MAX_FILE_BYTES (64 * 1024)   /* cap parser memory */

static user_signal_t s_signals[USER_SIGNALS_MAX];
static uint16_t      s_count  = 0;
static bool          s_inited = false;

static esp_err_t _ensure_dir(void) {
    struct stat st;
    if (stat(USER_SIGNALS_DIR, &st) == 0) return ESP_OK;
    if (mkdir(USER_SIGNALS_DIR, 0775) == 0) return ESP_OK;
    if (errno == EEXIST) return ESP_OK;
    ESP_LOGW(TAG, "mkdir %s failed: %d", USER_SIGNALS_DIR, errno);
    return ESP_FAIL;
}

static char *_read_file_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    if (len < 0 || (size_t)len > USER_SIGNALS_MAX_FILE_BYTES) {
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[got] = '\0';
    if (out_len) *out_len = got;
    return buf;
}

static esp_err_t _save_to_disk(void) {
    if (_ensure_dir() != ESP_OK) return ESP_FAIL;

    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "version", 1);
    cJSON *arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "signals", arr);

    for (uint16_t i = 0; i < s_count; i++) {
        const user_signal_t *s = &s_signals[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name",   s->name);
        cJSON_AddNumberToObject(o, "can_id", (double)s->can_id);
        cJSON_AddNumberToObject(o, "start",  s->start_bit);
        cJSON_AddNumberToObject(o, "length", s->length);
        cJSON_AddNumberToObject(o, "scale",  s->scale);
        cJSON_AddNumberToObject(o, "offset", s->offset);
        cJSON_AddBoolToObject  (o, "signed", s->is_signed);
        cJSON_AddNumberToObject(o, "endian", s->endian);
        cJSON_AddStringToObject(o, "unit",   s->unit);
        cJSON_AddItemToArray(arr, o);
    }

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) return ESP_ERR_NO_MEM;

    FILE *f = fopen(USER_SIGNALS_PATH, "wb");
    if (!f) { free(out); return ESP_FAIL; }
    size_t len   = strlen(out);
    size_t wrote = fwrite(out, 1, len, f);
    fclose(f);
    free(out);
    return (wrote == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t user_signals_init(void) {
    if (s_inited) return ESP_OK;
    s_inited = true;
    s_count  = 0;

    _ensure_dir();   /* best-effort */

    size_t flen = 0;
    char *buf = _read_file_all(USER_SIGNALS_PATH, &flen);
    if (!buf) {
        ESP_LOGI(TAG, "no user library yet (%s)", USER_SIGNALS_PATH);
        return ESP_OK;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "user.json parse failed, starting empty");
        return ESP_OK;
    }

    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "signals");
    if (cJSON_IsArray(arr)) {
        cJSON *o;
        cJSON_ArrayForEach(o, arr) {
            if (s_count >= USER_SIGNALS_MAX) break;
            user_signal_t *s = &s_signals[s_count];
            memset(s, 0, sizeof(*s));

            cJSON *it;
            it = cJSON_GetObjectItem(o, "name");
            if (cJSON_IsString(it) && it->valuestring) {
                strncpy(s->name, it->valuestring, sizeof(s->name) - 1);
            }
            if (s->name[0] == '\0') continue;   /* skip nameless entries */

            it = cJSON_GetObjectItem(o, "can_id");
            if (cJSON_IsNumber(it)) s->can_id = (uint32_t)it->valuedouble;
            it = cJSON_GetObjectItem(o, "start");
            if (cJSON_IsNumber(it)) s->start_bit = (uint8_t)it->valueint;
            it = cJSON_GetObjectItem(o, "length");
            if (cJSON_IsNumber(it)) s->length = (uint8_t)it->valueint;
            it = cJSON_GetObjectItem(o, "scale");
            if (cJSON_IsNumber(it)) s->scale = (float)it->valuedouble;
            it = cJSON_GetObjectItem(o, "offset");
            if (cJSON_IsNumber(it)) s->offset = (float)it->valuedouble;
            it = cJSON_GetObjectItem(o, "signed");
            if (cJSON_IsBool(it))   s->is_signed = cJSON_IsTrue(it);
            it = cJSON_GetObjectItem(o, "endian");
            if (cJSON_IsNumber(it)) s->endian = (uint8_t)it->valueint;
            it = cJSON_GetObjectItem(o, "unit");
            if (cJSON_IsString(it) && it->valuestring) {
                strncpy(s->unit, it->valuestring, sizeof(s->unit) - 1);
            }
            s_count++;
        }
    }
    cJSON_Delete(root);

    ESP_LOGI(TAG, "loaded %u user signals from %s", s_count, USER_SIGNALS_PATH);
    return ESP_OK;
}

uint16_t user_signals_count(void) { return s_count; }

const user_signal_t *user_signals_get(uint16_t index) {
    if (index >= s_count) return NULL;
    return &s_signals[index];
}

const user_signal_t *user_signals_find(const char *name) {
    if (!name) return NULL;
    for (uint16_t i = 0; i < s_count; i++) {
        if (strcmp(s_signals[i].name, name) == 0) return &s_signals[i];
    }
    return NULL;
}

esp_err_t user_signals_append(const user_signal_t *sig) {
    if (!sig || !s_inited)         return ESP_ERR_INVALID_ARG;
    if (sig->name[0] == '\0')      return ESP_ERR_INVALID_ARG;

    /* If the name already exists, overwrite in place. */
    for (uint16_t i = 0; i < s_count; i++) {
        if (strcmp(s_signals[i].name, sig->name) == 0) {
            s_signals[i] = *sig;
            return _save_to_disk();
        }
    }

    if (s_count >= USER_SIGNALS_MAX) return ESP_ERR_NO_MEM;
    s_signals[s_count++] = *sig;
    return _save_to_disk();
}
