/**
 * serial_commands_assets.c — image/font asset serial JSON-RPC handlers.
 *
 * Methods: image.list, image.delete, font.list, font.delete,
 * download.start, download.chunk (chunked download of an image or font;
 * distinct from the chunked-UPLOAD handlers in serial_commands_upload.c).
 */
#include "serial_commands_internal.h"
#include "uart_protocol.h"
#include "serial_protocol.h"

#include "cJSON.h"
#include "storage/boot_assets.h"
#include "widgets/font_manager.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

/* LVGL mutex (defined in main.c) */
extern bool rdm_lvgl_lock(int timeout_ms);
extern void rdm_lvgl_unlock(void);

/* ── image.list ──────────────────────────────────────────────────────────── */

void _handle_image_list(int id, cJSON *params)
{
    (void)params;
    _ensure_dir(LFS_IMAGE_DIR);

    cJSON *arr = cJSON_CreateArray();
    DIR *d = opendir(LFS_IMAGE_DIR);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t flen = strlen(de->d_name);
            if (flen < 8 || strcmp(de->d_name + flen - 7, ".rdmimg") != 0)
                continue;

            char path[80];
            snprintf(path, sizeof(path), "%s/%s", LFS_IMAGE_DIR, de->d_name);

            /* Read RDMIMG header for dimensions */
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            uint8_t hdr[12];
            if (fread(hdr, 1, 12, f) == 12 && memcmp(hdr, "RDMI", 4) == 0) {
                uint16_t w = hdr[4] | (hdr[5] << 8);
                uint16_t h = hdr[6] | (hdr[7] << 8);

                struct stat st;
                stat(path, &st);

                char name_buf[32];
                size_t name_len = flen - 7;
                if (name_len >= sizeof(name_buf)) name_len = sizeof(name_buf) - 1;
                memcpy(name_buf, de->d_name, name_len);
                name_buf[name_len] = '\0';

                cJSON *obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "name", name_buf);
                cJSON_AddNumberToObject(obj, "width", w);
                cJSON_AddNumberToObject(obj, "height", h);
                cJSON_AddNumberToObject(obj, "size", st.st_size);
                cJSON_AddItemToArray(arr, obj);
            }
            fclose(f);
        }
        closedir(d);
    }
    _send_response(id, arr, NULL);
}

/* ── image.delete ────────────────────────────────────────────────────────── */

void _handle_image_delete(int id, cJSON *params)
{
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name_item) || !_name_is_safe(name_item->valuestring)) {
        _send_error(id, "Invalid name");
        return;
    }
    if (boot_assets_is_protected_image(name_item->valuestring)) {
        _send_error(id, "Built-in image cannot be deleted");
        return;
    }
    char path[80];
    snprintf(path, sizeof(path), "%s/%s.rdmimg", LFS_IMAGE_DIR,
             name_item->valuestring);
    if (remove(path) != 0) {
        _send_error(id, "Delete failed");
        return;
    }
    _send_ok(id);
}

/* ── download.start (returns metadata for chunked download) ─────────────── */

void _handle_download_start(int id, cJSON *params)
{
    cJSON *type_item = cJSON_GetObjectItem(params, "type");
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(type_item) || !cJSON_IsString(name_item)
        || !_name_is_safe(name_item->valuestring)) {
        _send_error(id, "Invalid type/name");
        return;
    }

    const char *type = type_item->valuestring;
    const char *name = name_item->valuestring;
    bool is_image = (strcmp(type, "image") == 0);
    bool is_font  = (strcmp(type, "font") == 0);
    if (!is_image && !is_font) {
        _send_error(id, "Invalid download type (image/font)");
        return;
    }

    char path[80];
    if (is_image)
        snprintf(path, sizeof(path), "%s/%s.rdmimg", LFS_IMAGE_DIR, name);
    else
        snprintf(path, sizeof(path), "%s/%s.ttf", LFS_FONT_DIR, name);

    struct stat st;
    if (stat(path, &st) != 0) {
        _send_error(id, is_image ? "Image not found" : "Font not found");
        return;
    }

    uint32_t file_size = (uint32_t)st.st_size;
    uint16_t total_chunks = (uint16_t)((file_size + DOWNLOAD_CHUNK_SIZE - 1)
                                       / DOWNLOAD_CHUNK_SIZE);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "name", name);
    cJSON_AddNumberToObject(r, "size", file_size);
    cJSON_AddNumberToObject(r, "chunks", total_chunks);
    cJSON_AddNumberToObject(r, "chunk_size", DOWNLOAD_CHUNK_SIZE);
    _send_response(id, r, NULL);
}

/* ── download.chunk (responds with raw binary frame — no base64/JSON) ───── */

void _handle_download_chunk(int id, cJSON *params)
{
    cJSON *type_item  = cJSON_GetObjectItem(params, "type");
    cJSON *name_item  = cJSON_GetObjectItem(params, "name");
    cJSON *index_item = cJSON_GetObjectItem(params, "index");
    if (!cJSON_IsString(type_item) || !cJSON_IsString(name_item)
        || !cJSON_IsNumber(index_item)
        || !_name_is_safe(name_item->valuestring)) {
        _send_error(id, "Invalid params");
        return;
    }

    const char *type = type_item->valuestring;
    const char *name = name_item->valuestring;
    uint16_t chunk_idx = (uint16_t)index_item->valueint;

    bool is_image = (strcmp(type, "image") == 0);
    bool is_font  = (strcmp(type, "font") == 0);
    if (!is_image && !is_font) {
        _send_error(id, "Invalid download type");
        return;
    }

    char path[80];
    if (is_image)
        snprintf(path, sizeof(path), "%s/%s.rdmimg", LFS_IMAGE_DIR, name);
    else
        snprintf(path, sizeof(path), "%s/%s.ttf", LFS_FONT_DIR, name);

    FILE *f = fopen(path, "rb");
    if (!f) {
        _send_error(id, "File not found");
        return;
    }

    /* Allocate buffer: type tag (1B) + chunk data */
    uint8_t *buf = malloc(1 + DOWNLOAD_CHUNK_SIZE);
    if (!buf) {
        fclose(f);
        _send_error(id, "OOM for read buffer");
        return;
    }

    uint32_t offset = (uint32_t)chunk_idx * DOWNLOAD_CHUNK_SIZE;
    fseek(f, offset, SEEK_SET);
    buf[0] = UART_PAYLOAD_BINARY;
    size_t nr = fread(buf + 1, 1, DOWNLOAD_CHUNK_SIZE, f);
    fclose(f);

    if (nr == 0) {
        free(buf);
        _send_error(id, "Read past end of file");
        return;
    }

    /* Send raw binary frame — desktop reads type tag to distinguish from JSON error */
    serial_protocol_send_frame(buf, 1 + nr);
    free(buf);
}

/* ── font.list ───────────────────────────────────────────────────────────── */

void _handle_font_list(int id, cJSON *params)
{
    (void)params;
    _ensure_dir(LFS_FONT_DIR);

    cJSON *arr = cJSON_CreateArray();
    DIR *d = opendir(LFS_FONT_DIR);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            size_t flen = strlen(de->d_name);
            if (flen < 5 || strcmp(de->d_name + flen - 4, ".ttf") != 0)
                continue;

            char name_buf[32];
            size_t name_len = flen - 4;
            if (name_len >= sizeof(name_buf)) name_len = sizeof(name_buf) - 1;
            memcpy(name_buf, de->d_name, name_len);
            name_buf[name_len] = '\0';

            char path[80];
            snprintf(path, sizeof(path), "%s/%s", LFS_FONT_DIR, de->d_name);
            struct stat st;
            stat(path, &st);

            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "name", name_buf);
            cJSON_AddNumberToObject(obj, "size", st.st_size);
            cJSON_AddItemToArray(arr, obj);
        }
        closedir(d);
    }
    _send_response(id, arr, NULL);
}

/* ── font.delete ─────────────────────────────────────────────────────────── */

void _handle_font_delete(int id, cJSON *params)
{
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name_item) || !_name_is_safe(name_item->valuestring)) {
        _send_error(id, "Invalid name");
        return;
    }
    if (boot_assets_is_protected_font(name_item->valuestring)) {
        _send_error(id, "Built-in font cannot be deleted");
        return;
    }
    /* Under the LVGL lock — lv_tiny_ttf_destroy() races the render task
     * otherwise (runs on the UART task here). */
    bool removed;
    if (rdm_lvgl_lock(2000)) {
        removed = font_manager_remove_family(name_item->valuestring);
        rdm_lvgl_unlock();
    } else {
        removed = font_manager_remove_family(name_item->valuestring);
    }
    if (!removed) {
        _send_error(id, "Font not found");
        return;
    }
    _send_ok(id);
}
