/**
 * serial_commands_upload.c — chunked image/font/OTA upload serial JSON-RPC
 * handlers.
 *
 * Methods: upload.start, upload.finish, upload.abort, plus the public
 * serial_commands_handle_binary() entry point (declared in
 * serial_commands.h) that accepts the raw binary chunk frames for an
 * in-progress upload session.
 *
 * All tightly coupled via the s_upload session state, which is a file-static
 * here only — nothing outside this file touches it directly.
 */
#include "serial_commands.h"
#include "serial_commands_internal.h"
#include "serial_protocol.h"
#include "uart_protocol.h"
#include "storage/boot_assets.h"
#include "widgets/font_manager.h"   /* FONT_MAX_FILE_SIZE */

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>   /* fsync, fileno */

static const char *TAG = "serial_cmd";

/* ── Upload session state ───────────────────────────────────────────────── */

static serial_upload_session_t s_upload = {0};

/* ── upload.start (image/font/OTA chunked transfer) ──────────────────────── */

void _handle_upload_start(int id, cJSON *params)
{
    if (s_upload.active) {
        _send_error(id, "Upload already in progress");
        return;
    }

    cJSON *type_item = cJSON_GetObjectItem(params, "type");
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    cJSON *size_item = cJSON_GetObjectItem(params, "size");
    if (!cJSON_IsString(type_item) || !cJSON_IsNumber(size_item)) {
        _send_error(id, "Missing type/size");
        return;
    }

    const char *type = type_item->valuestring;
    uint32_t total_size = (uint32_t)size_item->valuedouble;
    const char *name = cJSON_IsString(name_item) ? name_item->valuestring : "";

    /* Validate type */
    bool is_ota = (strcmp(type, "ota") == 0);
    bool is_image = (strcmp(type, "image") == 0);
    bool is_font = (strcmp(type, "font") == 0);
    if (!is_ota && !is_image && !is_font) {
        _send_error(id, "Invalid upload type (image/font/ota)");
        return;
    }

    if ((is_image || is_font) && !_name_is_safe(name)) {
        _send_error(id, "Invalid name");
        return;
    }

    /* Refuse to overwrite a built-in protected asset — the web handlers already
     * return 403 for these (web_server_assets.c), but the serial path bypassed
     * the check, so a serial client could clobber a bundled image/font. */
    if (is_image && boot_assets_is_protected_image(name)) {
        _send_error(id, "Cannot overwrite a built-in image");
        return;
    }
    if (is_font && boot_assets_is_protected_font(name)) {
        _send_error(id, "Cannot overwrite a built-in font");
        return;
    }

    if (is_image && total_size > IMAGE_MAX_SIZE) {
        _send_error(id, "Image too large");
        return;
    }
    /* Font cap mirrors the web path (FONT_MAX_FILE_SIZE); the serial path had
     * no cap, so an oversized font could waste flash or fill the partition. */
    if (is_font && total_size > FONT_MAX_FILE_SIZE) {
        _send_error(id, "Font too large");
        return;
    }

    /* Initialise session */
    memset(&s_upload, 0, sizeof(s_upload));
    strncpy(s_upload.type, type, sizeof(s_upload.type) - 1);
    strncpy(s_upload.name, name, sizeof(s_upload.name) - 1);
    s_upload.total_size = total_size;
    s_upload.total_chunks = (uint16_t)((total_size + UART_PROTO_CHUNK_SIZE - 1)
                                       / UART_PROTO_CHUNK_SIZE);
    /* Mask to 53 bits so the value survives JSON double round-trip */
    s_upload.session_id = ((uint64_t)esp_random() << 32 | esp_random())
                          & 0x1FFFFFFFFFFFFFull;

    if (is_ota) {
        /* Begin OTA partition write */
        const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
        if (!part) {
            _send_error(id, "No OTA partition available");
            return;
        }
        esp_ota_handle_t handle;
        esp_err_t err = esp_ota_begin(part, total_size, &handle);
        if (err != ESP_OK) {
            _send_error(id, "OTA begin failed");
            return;
        }
        s_upload.ota_handle = (void *)(uintptr_t)handle;
        s_upload.ota_partition = (void *)part;
    } else {
        /* Allocate accumulation buffer in PSRAM */
        s_upload.buffer = heap_caps_malloc(total_size, MALLOC_CAP_SPIRAM);
        if (!s_upload.buffer) {
            s_upload.buffer = malloc(total_size);
            if (!s_upload.buffer) {
                _send_error(id, "OOM for upload buffer");
                return;
            }
        }
    }

    s_upload.active = true;

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "session", (double)s_upload.session_id);
    cJSON_AddNumberToObject(r, "chunk_size", UART_PROTO_CHUNK_SIZE);
    cJSON_AddNumberToObject(r, "total_chunks", s_upload.total_chunks);
    _send_response(id, r, NULL);
}

/* ── upload.finish ───────────────────────────────────────────────────────── */

void _handle_upload_finish(int id, cJSON *params)
{
    (void)params;
    if (!s_upload.active) {
        _send_error(id, "No upload in progress");
        return;
    }

    bool is_ota = (strcmp(s_upload.type, "ota") == 0);
    bool is_image = (strcmp(s_upload.type, "image") == 0);
    bool is_font = (strcmp(s_upload.type, "font") == 0);

    if (is_ota) {
        esp_ota_handle_t handle = (esp_ota_handle_t)(uintptr_t)s_upload.ota_handle;
        const esp_partition_t *part = (const esp_partition_t *)s_upload.ota_partition;
        esp_err_t err = esp_ota_end(handle);
        if (err != ESP_OK) {
            _send_error(id, "OTA end failed");
            s_upload.active = false;
            return;
        }
        err = esp_ota_set_boot_partition(part);
        if (err != ESP_OK) {
            _send_error(id, "Set boot partition failed");
            s_upload.active = false;
            return;
        }
        s_upload.active = false;
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "status", "ok");
        cJSON_AddBoolToObject(r, "reboot_required", true);
        _send_response(id, r, NULL);
        return;
    }

    /* Image or font — write accumulated buffer to LittleFS. Reject an
     * incomplete transfer instead of publishing a truncated asset over the
     * existing good one (the old code only logged and wrote the short buffer). */
    if (s_upload.received != s_upload.total_size) {
        ESP_LOGW(TAG, "Upload incomplete: %u/%u bytes — rejecting",
                 (unsigned)s_upload.received, (unsigned)s_upload.total_size);
        free(s_upload.buffer);
        s_upload.buffer = NULL;
        s_upload.active = false;
        _send_error(id, "Upload incomplete");
        return;
    }

    char path[80];
    if (is_image) {
        _ensure_dir(LFS_IMAGE_DIR);
        /* Validate RDMIMG magic */
        if (s_upload.received < 12 ||
            memcmp(s_upload.buffer, "RDMI", 4) != 0) {
            free(s_upload.buffer);
            s_upload.buffer = NULL;
            s_upload.active = false;
            _send_error(id, "Invalid RDMIMG format");
            return;
        }
        snprintf(path, sizeof(path), "%s/%s.rdmimg", LFS_IMAGE_DIR,
                 s_upload.name);
    } else if (is_font) {
        _ensure_dir(LFS_FONT_DIR);
        snprintf(path, sizeof(path), "%s/%s.ttf", LFS_FONT_DIR,
                 s_upload.name);
    } else {
        free(s_upload.buffer);
        s_upload.buffer = NULL;
        s_upload.active = false;
        _send_error(id, "Unknown upload type");
        return;
    }

    /* Atomic publish (mirrors the web image path + channel_manager save): stage
     * into path.tmp, fsync, keep the previous asset as .bak across the rename
     * window, then rename .tmp over the live file. rename() is atomic on
     * LittleFS, so a crash leaves either the old or the new file — never a
     * half-written asset. The old code fopen(path,"wb")'d in place, truncating
     * the good asset before the first byte and remove()'ing it on a short
     * write. */
    char tmp_path[96], bak_path[96];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    snprintf(bak_path, sizeof(bak_path), "%s.bak", path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        free(s_upload.buffer);
        s_upload.buffer = NULL;
        s_upload.active = false;
        _send_error(id, "Cannot write file");
        return;
    }
    size_t nw = fwrite(s_upload.buffer, 1, s_upload.received, f);
    bool wok = (nw == s_upload.received) &&
               (fflush(f) == 0) && (fsync(fileno(f)) == 0);
    if (fclose(f) != 0) wok = false;
    free(s_upload.buffer);
    s_upload.buffer = NULL;
    s_upload.active = false;

    if (!wok) {
        remove(tmp_path);
        _send_error(id, "Write incomplete");
        return;
    }

    rename(path, bak_path);                 /* keep previous good copy */
    if (rename(tmp_path, path) != 0) {
        rename(bak_path, path);             /* restore on failure */
        remove(tmp_path);
        _send_error(id, "Cannot publish file");
        return;
    }
    remove(bak_path);                       /* best-effort space reclaim */

    ESP_LOGI(TAG, "Upload complete: %s '%s' (%u bytes)",
             is_image ? "image" : "font", s_upload.name,
             (unsigned)s_upload.received);
    _send_ok(id);
}

/* ── upload.abort ────────────────────────────────────────────────────────── */

void _handle_upload_abort(int id, cJSON *params)
{
    (void)params;
    if (!s_upload.active) {
        _send_ok(id);
        return;
    }

    if (strcmp(s_upload.type, "ota") == 0) {
        esp_ota_handle_t handle = (esp_ota_handle_t)(uintptr_t)s_upload.ota_handle;
        esp_ota_abort(handle);
    }
    if (s_upload.buffer) {
        free(s_upload.buffer);
        s_upload.buffer = NULL;
    }
    s_upload.active = false;
    _send_ok(id);
}

/* ── Binary chunk handler ───────────────────────────────────────────────── */

esp_err_t serial_commands_handle_binary(const uint8_t *data, size_t len)
{
    if (!s_upload.active) {
        ESP_LOGW(TAG, "Binary chunk received but no upload in progress");
        return ESP_FAIL;
    }

    /* Binary chunk format:
     * [session_id: 8B] [chunk_idx: 2B LE] [data: rest] */
    if (len < 10) {
        ESP_LOGW(TAG, "Binary chunk too short (%u bytes)", (unsigned)len);
        return ESP_FAIL;
    }

    uint64_t session = 0;
    memcpy(&session, data, 8);
    if (session != s_upload.session_id) {
        ESP_LOGW(TAG, "Session mismatch");
        return ESP_FAIL;
    }

    uint16_t chunk_idx = (uint16_t)data[8] | ((uint16_t)data[9] << 8);
    const uint8_t *chunk_data = data + 10;
    size_t chunk_len = len - 10;

    if (chunk_idx != s_upload.next_chunk) {
        ESP_LOGW(TAG, "Chunk index mismatch: expected %u, got %u",
                 s_upload.next_chunk, chunk_idx);
        /* Send NACK */
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddNumberToObject(resp, "id", 0);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "chunk", chunk_idx);
        cJSON_AddBoolToObject(r, "ok", false);
        cJSON_AddNumberToObject(r, "expected", s_upload.next_chunk);
        cJSON_AddItemToObject(resp, "result", r);
        cJSON_AddNullToObject(resp, "error");
        char *json = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (json) { serial_protocol_send_json(json); free(json); }
        return ESP_FAIL;
    }

    bool is_ota = (strcmp(s_upload.type, "ota") == 0);
    size_t accepted = chunk_len;     /* bytes actually consumed */

    if (is_ota) {
        /* Write directly to OTA partition */
        esp_ota_handle_t handle = (esp_ota_handle_t)(uintptr_t)s_upload.ota_handle;
        esp_err_t err = esp_ota_write(handle, chunk_data, chunk_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            /* Send NACK */
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddNumberToObject(resp, "id", 0);
            cJSON *r = cJSON_CreateObject();
            cJSON_AddNumberToObject(r, "chunk", chunk_idx);
            cJSON_AddBoolToObject(r, "ok", false);
            cJSON_AddItemToObject(resp, "result", r);
            cJSON_AddNullToObject(resp, "error");
            char *json = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);
            if (json) { serial_protocol_send_json(json); free(json); }
            return ESP_FAIL;
        }
    } else {
        /* Accumulate in buffer — clamp to remaining space */
        accepted = chunk_len;
        if (s_upload.received + accepted > s_upload.total_size)
            accepted = s_upload.total_size - s_upload.received;
        memcpy(s_upload.buffer + s_upload.received, chunk_data, accepted);
    }

    s_upload.received += accepted;
    s_upload.next_chunk++;

    /* Send ACK */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "id", 0);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "chunk", chunk_idx);
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddItemToObject(resp, "result", r);
    cJSON_AddNullToObject(resp, "error");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (json) { serial_protocol_send_json(json); free(json); }

    return ESP_OK;
}
