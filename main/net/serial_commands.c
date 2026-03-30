/**
 * serial_commands.c — JSON-RPC command dispatcher for UART serial protocol.
 *
 * Each method handler calls the same core logic as the HTTP web server
 * handlers, then sends a JSON response frame over UART.
 */
#include "serial_commands.h"
#include "serial_protocol.h"
#include "uart_protocol.h"

#include "cJSON.h"
#include "display_capture.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "layout/layout_manager.h"
#include "storage/config_store.h"
#include "storage/data_logger.h"
#include "storage/sd_manager.h"
#include "system/device_id.h"
#include "system/rdm_settings.h"
#include "ui/dashboard.h"
#include "ui/screens/splash_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/ui.h"
#include "ui/settings/device_settings.h"
#include "widgets/font_manager.h"
#include "widgets/signal.h"
#include "widgets/signal_internal.h"
#include "widgets/signal_sim.h"
#include "lvgl.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "serial_cmd";

/* LVGL mutex (defined in main.c) */
extern bool example_lvgl_lock(int timeout_ms);
extern void example_lvgl_unlock(void);

/* Deferred screen reload — must run on LVGL task */
static void _deferred_screen_reload(void *arg)
{
    (void)arg;
    lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
    ui_Screen3_screen_init();
    lv_scr_load(ui_Screen3);
    if (old && old != ui_Screen3 && lv_obj_is_valid(old))
        lv_obj_del(old);
}

/* Image/font directory paths (same as web_server.c) */
#define LFS_IMAGE_DIR "/lfs/images"
#define LFS_FONT_DIR  "/lfs/fonts"
#define IMAGE_MAX_SIZE (1200 * 1024)

/* ── Upload session state ───────────────────────────────────────────────── */

static serial_upload_session_t s_upload = {0};

/* ── Helper: path-safety check (replicates web_server.c _name_is_safe) ── */

static bool _name_is_safe(const char *name)
{
    if (!name || !name[0]) return false;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '.' || *p < 0x20) return false;
    }
    return true;
}

/* ── Helper: ensure directories exist ───────────────────────────────────── */

static void _ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0775);
    }
}

/* ── Helper: send JSON-RPC response ─────────────────────────────────────── */

static void _send_response(int id, cJSON *result, const char *error)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "id", id);
    if (error) {
        cJSON_AddNullToObject(resp, "result");
        cJSON_AddStringToObject(resp, "error", error);
    } else {
        if (result)
            cJSON_AddItemToObject(resp, "result", result);
        else
            cJSON_AddNullToObject(resp, "result");
        cJSON_AddNullToObject(resp, "error");
    }
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (json) {
        serial_protocol_send_json(json);
        free(json);
    }
}

static void _send_ok(int id)
{
    cJSON *r = cJSON_CreateString("ok");
    _send_response(id, r, NULL);
}

static void _send_error(int id, const char *msg)
{
    _send_response(id, NULL, msg);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Method Handlers
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── device.info ─────────────────────────────────────────────────────────── */

static void _handle_device_info(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    char serial[MAX_SERIAL_LENGTH];
    get_device_serial(serial);
    cJSON_AddStringToObject(r, "serial", serial);

    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON_AddStringToObject(r, "version", desc->version);
    cJSON_AddNumberToObject(r, "schema", LAYOUT_SCHEMA_VERSION);
    cJSON_AddStringToObject(r, "project", desc->project_name);

    cJSON *hw = cJSON_AddObjectToObject(r, "hardware");
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    cJSON_AddStringToObject(hw, "chip", CONFIG_IDF_TARGET);
    cJSON_AddNumberToObject(hw, "cores", chip_info.cores);
    cJSON_AddNumberToObject(hw, "psram_mb",
        (double)heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / (1024 * 1024));
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    cJSON_AddNumberToObject(hw, "flash_mb",
        (double)flash_size / (1024 * 1024));

    _send_response(id, r, NULL);
}

/* ── storage.info ────────────────────────────────────────────────────────── */

static void _handle_storage_info(int id, cJSON *params)
{
    (void)params;
    size_t total = 0, used = 0;
    if (esp_littlefs_info("littlefs", &total, &used) != ESP_OK) {
        _send_error(id, "Cannot read storage info");
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "total", total);
    cJSON_AddNumberToObject(r, "used", used);
    cJSON_AddNumberToObject(r, "free", (total > used) ? total - used : 0);

    cJSON *sd = cJSON_AddObjectToObject(r, "sd");
    if (sd_manager_is_mounted()) {
        size_t sd_total = 0, sd_used = 0, sd_free = 0;
        cJSON_AddBoolToObject(sd, "mounted", true);
        if (sd_manager_get_info(&sd_total, &sd_used, &sd_free) == ESP_OK) {
            cJSON_AddNumberToObject(sd, "total", sd_total);
            cJSON_AddNumberToObject(sd, "used", sd_used);
            cJSON_AddNumberToObject(sd, "free", sd_free);
        }
    } else {
        cJSON_AddBoolToObject(sd, "mounted", false);
    }
    _send_response(id, r, NULL);
}

/* ── layout.list ─────────────────────────────────────────────────────────── */

static void _handle_layout_list(int id, cJSON *params)
{
    (void)params;
    char names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
    int count = layout_manager_list(names, LAYOUT_MAX_COUNT);
    if (count < 0) {
        _send_error(id, "Failed to list layouts");
        return;
    }

    char active[LAYOUT_MAX_NAME];
    if (layout_manager_get_active(active, sizeof(active)) != ESP_OK)
        strcpy(active, "default");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "active", active);
    cJSON *arr = cJSON_AddArrayToObject(r, "layouts");
    for (int i = 0; i < count; i++) {
        if (names[i][0] == '_') continue; /* skip system layouts */
        cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    }
    _send_response(id, r, NULL);
}

/* ── layout.current ──────────────────────────────────────────────────────── */

static void _handle_layout_current(int id, cJSON *params)
{
    (void)params;
    bool is_splash = splash_screen_is_edit_mode();
    char name[LAYOUT_MAX_NAME];
    if (is_splash) {
        snprintf(name, sizeof(name), "_splash_%s",
                 splash_screen_get_active_name());
    } else if (rdm_settings_get_active_layout(name, sizeof(name)) != ESP_OK) {
        _send_error(id, "Failed to read active layout");
        return;
    }

    if (!example_lvgl_lock(1000)) {
        _send_error(id, "LVGL busy");
        return;
    }

    widget_t **widgets;
    uint8_t count;
    if (is_splash) {
        widgets = splash_screen_get_widgets();
        count = splash_screen_get_widget_count();
    } else {
        widgets = dashboard_get_widgets();
        count = dashboard_get_widget_count();
    }
    cJSON *root = layout_manager_build_json(name, widgets, count);
    example_lvgl_unlock();

    if (!root) {
        _send_error(id, "Failed to build layout JSON");
        return;
    }
    _send_response(id, root, NULL);
}

/* ── layout.raw ──────────────────────────────────────────────────────────── */

static void _handle_layout_raw(int id, cJSON *params)
{
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name_item)) {
        _send_error(id, "Missing 'name' parameter");
        return;
    }
    const char *name = name_item->valuestring;

    char *buf = malloc(LAYOUT_MAX_FILE_BYTES);
    if (!buf) { _send_error(id, "OOM"); return; }

    size_t out_len = 0;
    esp_err_t err = layout_manager_read_raw(name, buf, LAYOUT_MAX_FILE_BYTES,
                                            &out_len);
    if (err != ESP_OK) {
        free(buf);
        _send_error(id, "Layout not found");
        return;
    }

    /* Parse raw JSON and send as result */
    cJSON *layout = cJSON_ParseWithLength(buf, out_len);
    free(buf);
    if (!layout) {
        _send_error(id, "Invalid layout JSON");
        return;
    }
    _send_response(id, layout, NULL);
}

/* ── layout.save ─────────────────────────────────────────────────────────── */

static void _handle_layout_save(int id, cJSON *params)
{
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    cJSON *data_item = cJSON_GetObjectItem(params, "data");
    if (!cJSON_IsString(name_item) || !cJSON_IsObject(data_item)) {
        _send_error(id, "Missing 'name' or 'data'");
        return;
    }
    const char *name = name_item->valuestring;
    if (!_name_is_safe(name)) {
        _send_error(id, "Invalid layout name");
        return;
    }

    esp_err_t err = layout_manager_save_raw(name, data_item);
    if (err != ESP_OK) {
        _send_error(id, "Save failed");
        return;
    }
    layout_manager_set_active(name);

    /* Trigger hot-reload via LVGL async */
    lv_async_call(_deferred_screen_reload, NULL);
    _send_ok(id);
}

/* ── layout.set ──────────────────────────────────────────────────────────── */

static void _handle_layout_set(int id, cJSON *params)
{
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name_item)) {
        _send_error(id, "Missing 'name'");
        return;
    }
    const char *name = name_item->valuestring;
    if (!_name_is_safe(name)) {
        _send_error(id, "Invalid name");
        return;
    }

    /* Handle splash screens (names prefixed with _splash_) */
    if (strncmp(name, "_splash_", 8) == 0) {
        const char *splash_name = name + 8;
        layout_manager_set_active_splash(splash_name);
        splash_screen_set_active_name(splash_name);
        if (splash_screen_is_edit_mode())
            lv_async_call(_deferred_screen_reload, NULL);
    } else {
        layout_manager_set_active(name);
        lv_async_call(_deferred_screen_reload, NULL);
    }
    _send_ok(id);
}

/* ── layout.delete ───────────────────────────────────────────────────────── */

static void _handle_layout_delete(int id, cJSON *params)
{
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name_item)) {
        _send_error(id, "Missing 'name'");
        return;
    }
    const char *name = name_item->valuestring;
    if (!_name_is_safe(name)) {
        _send_error(id, "Invalid name");
        return;
    }
    if (strcmp(name, "default") == 0) {
        _send_error(id, "Cannot delete default layout");
        return;
    }

    esp_err_t err = layout_manager_delete(name);
    if (err != ESP_OK) {
        _send_error(id, "Delete failed");
        return;
    }
    _send_ok(id);
}

/* ── layout.version ──────────────────────────────────────────────────────── */

static void _handle_layout_version(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateNumber(layout_manager_get_version());
    _send_response(id, r, NULL);
}

/* ── image.list ──────────────────────────────────────────────────────────── */

static void _handle_image_list(int id, cJSON *params)
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

static void _handle_image_delete(int id, cJSON *params)
{
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name_item) || !_name_is_safe(name_item->valuestring)) {
        _send_error(id, "Invalid name");
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

/* ── Download chunk size (raw binary frames, no base64 overhead) ─────────── */
#define DOWNLOAD_CHUNK_SIZE (32 * 1024)

/* ── download.start (returns metadata for chunked download) ─────────────── */

static void _handle_download_start(int id, cJSON *params)
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

static void _handle_download_chunk(int id, cJSON *params)
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

static void _handle_font_list(int id, cJSON *params)
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

static void _handle_font_delete(int id, cJSON *params)
{
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(name_item) || !_name_is_safe(name_item->valuestring)) {
        _send_error(id, "Invalid name");
        return;
    }
    if (!font_manager_remove_family(name_item->valuestring)) {
        _send_error(id, "Font not found");
        return;
    }
    _send_ok(id);
}

/* ── signal.values ───────────────────────────────────────────────────────── */

static void _handle_signal_values(int id, cJSON *params)
{
    (void)params;
    if (!example_lvgl_lock(500)) {
        _send_error(id, "LVGL busy");
        return;
    }

    uint16_t count = signal_get_count();
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "signals");
    for (uint16_t i = 0; i < count; i++) {
        signal_t *sig = signal_get_by_index(i);
        if (!sig || sig->name[0] == '\0') continue;
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", sig->name);
        cJSON_AddNumberToObject(obj, "value", sig->current_value);
        cJSON_AddBoolToObject(obj, "stale", sig->is_stale);
        cJSON_AddNumberToObject(obj, "can_id", sig->can_id);
        cJSON_AddItemToArray(arr, obj);
    }
    example_lvgl_unlock();
    _send_response(id, r, NULL);
}

/* ── signal.inject ───────────────────────────────────────────────────────── */

static void _handle_signal_inject(int id, cJSON *params)
{
    /* Supports single {"name":"RPM","value":3000} or batch {"signals":[...]} */
    cJSON *batch = cJSON_GetObjectItem(params, "signals");
    if (cJSON_IsArray(batch)) {
        int n = cJSON_GetArraySize(batch);
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(batch, i);
            cJSON *n_item = cJSON_GetObjectItem(item, "name");
            cJSON *v_item = cJSON_GetObjectItem(item, "value");
            if (cJSON_IsString(n_item) && cJSON_IsNumber(v_item)) {
                signal_inject_test_value(n_item->valuestring,
                                         (float)v_item->valuedouble);
            }
        }
    } else {
        cJSON *n_item = cJSON_GetObjectItem(params, "name");
        cJSON *v_item = cJSON_GetObjectItem(params, "value");
        if (cJSON_IsString(n_item) && cJSON_IsNumber(v_item)) {
            signal_inject_test_value(n_item->valuestring,
                                     (float)v_item->valuedouble);
        }
    }
    _send_ok(id);
}

/* ── signal.simulate ─────────────────────────────────────────────────────── */

static void _handle_signal_simulate(int id, cJSON *params)
{
    cJSON *enable = cJSON_GetObjectItem(params, "enable");
    if (cJSON_IsBool(enable)) {
        if (cJSON_IsTrue(enable))
            lv_async_call((lv_async_cb_t)signal_sim_start, NULL);
        else
            lv_async_call((lv_async_cb_t)signal_sim_stop, NULL);
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "active", signal_sim_is_active());
    _send_response(id, r, NULL);
}

/* ── screenshot ──────────────────────────────────────────────────────────── */

static void _handle_screenshot(int id, cJSON *params)
{
    (void)params;
    uint8_t *buf = NULL;
    size_t size = 0;

    esp_err_t err = display_capture_screenshot(&buf, &size);
    if (err != ESP_OK || !buf) {
        _send_error(id, "Screenshot failed");
        return;
    }

    /* Send size in JSON response — desktop will fetch binary via chunked read */
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "size", size);
    cJSON_AddStringToObject(r, "format", "bmp");
    _send_response(id, r, NULL);

    /* Send the binary data as a binary frame */
    /* Prefix: type tag (0x01) + screenshot data */
    size_t frame_len = 1 + size;
    uint8_t *frame = malloc(frame_len);
    if (frame) {
        frame[0] = UART_PAYLOAD_BINARY;
        memcpy(frame + 1, buf, size);
        serial_protocol_send_frame(frame, frame_len);
        free(frame);
    } else {
        ESP_LOGE(TAG, "OOM for screenshot frame (%u bytes)", (unsigned)frame_len);
    }
    display_capture_free_buffer(buf);
}

/* ── splash.list ─────────────────────────────────────────────────────────── */

static void _handle_splash_list(int id, cJSON *params)
{
    (void)params;
    char names[LAYOUT_MAX_COUNT][LAYOUT_MAX_NAME];
    int count = layout_manager_list_splash(names, LAYOUT_MAX_COUNT);
    if (count < 0) {
        _send_error(id, "Failed to list splash layouts");
        return;
    }

    char active[LAYOUT_MAX_NAME];
    if (layout_manager_get_active_splash(active, sizeof(active)) != ESP_OK)
        strcpy(active, "Default");

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "active", active);
    cJSON *arr = cJSON_AddArrayToObject(r, "splashes");
    for (int i = 0; i < count; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(names[i]));
    _send_response(id, r, NULL);
}

/* ── upload.start (image/font/OTA chunked transfer) ──────────────────────── */

static void _handle_upload_start(int id, cJSON *params)
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

    if (is_image && total_size > IMAGE_MAX_SIZE) {
        _send_error(id, "Image too large");
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

static void _handle_upload_finish(int id, cJSON *params)
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

    /* Image or font — write accumulated buffer to LittleFS */
    if (s_upload.received != s_upload.total_size) {
        ESP_LOGW(TAG, "Upload incomplete: %u/%u bytes",
                 (unsigned)s_upload.received, (unsigned)s_upload.total_size);
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

    FILE *f = fopen(path, "wb");
    if (!f) {
        free(s_upload.buffer);
        s_upload.buffer = NULL;
        s_upload.active = false;
        _send_error(id, "Cannot write file");
        return;
    }
    size_t nw = fwrite(s_upload.buffer, 1, s_upload.received, f);
    fclose(f);
    free(s_upload.buffer);
    s_upload.buffer = NULL;
    s_upload.active = false;

    if (nw != s_upload.received) {
        remove(path);
        _send_error(id, "Write incomplete");
        return;
    }

    ESP_LOGI(TAG, "Upload complete: %s '%s' (%u bytes)",
             is_image ? "image" : "font", s_upload.name,
             (unsigned)s_upload.received);
    _send_ok(id);
}

/* ── upload.abort ────────────────────────────────────────────────────────── */

static void _handle_upload_abort(int id, cJSON *params)
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

/* ── brightness.get / brightness.set ─────────────────────────────────────── */

static void _handle_brightness_get(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "brightness", (int)current_brightness);
    _send_response(id, r, NULL);
}

static void _handle_brightness_set(int id, cJSON *params)
{
    cJSON *j = cJSON_GetObjectItem(params, "brightness");
    if (!cJSON_IsNumber(j)) {
        _send_error(id, "Missing brightness");
        return;
    }
    int val = (int)j->valuedouble;
    if (val < 1) val = 1;
    if (val > 100) val = 100;
    set_display_brightness(val);
    _send_ok(id);
}

/* ── can.config.get / can.config.set ────────────────────────────────────── */

static void _handle_can_config_get(int id, cJSON *params)
{
    (void)params;
    uint8_t bitrate = 2;
    config_store_load_bitrate(&bitrate);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "bitrate", (int)bitrate);
    _send_response(id, r, NULL);
}

static void _handle_can_config_set(int id, cJSON *params)
{
    cJSON *j = cJSON_GetObjectItem(params, "bitrate");
    if (!cJSON_IsNumber(j)) {
        _send_error(id, "Missing bitrate");
        return;
    }
    uint8_t bitrate = (uint8_t)j->valuedouble;
    if (bitrate > 3) {
        _send_error(id, "Invalid bitrate index (0-3)");
        return;
    }
    config_store_save_bitrate(bitrate);
    _send_ok(id);
}

/* ── dimmer.get / dimmer.set ─────────────────────────────────────────────── */

static void _handle_dimmer_get(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "signal_name", dimmer_config.signal_name);
    cJSON_AddNumberToObject(r, "threshold", dimmer_config.threshold);
    cJSON_AddBoolToObject(r, "is_momentary", dimmer_config.is_momentary);
    cJSON_AddBoolToObject(r, "invert", dimmer_config.invert);
    cJSON_AddNumberToObject(r, "dim_brightness", dimmer_config.dim_brightness);
    cJSON_AddBoolToObject(r, "enabled", dimmer_config.enabled);
    _send_response(id, r, NULL);
}

static void _deferred_serial_dimmer_subscribe(void *arg)
{
    (void)arg;
    dimmer_subscribe();
}

static void _handle_dimmer_set(int id, cJSON *params)
{
    if (!example_lvgl_lock(1000)) {
        _send_error(id, "LVGL busy");
        return;
    }

    cJSON *j;
    if ((j = cJSON_GetObjectItem(params, "signal_name")) && cJSON_IsString(j)) {
        strncpy(dimmer_config.signal_name, j->valuestring,
                sizeof(dimmer_config.signal_name) - 1);
        dimmer_config.signal_name[sizeof(dimmer_config.signal_name) - 1] = '\0';
    }
    if ((j = cJSON_GetObjectItem(params, "threshold")) && cJSON_IsNumber(j))
        dimmer_config.threshold = (float)j->valuedouble;
    if ((j = cJSON_GetObjectItem(params, "is_momentary")))
        dimmer_config.is_momentary = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItem(params, "invert")))
        dimmer_config.invert = cJSON_IsTrue(j);
    if ((j = cJSON_GetObjectItem(params, "dim_brightness")) && cJSON_IsNumber(j))
        dimmer_config.dim_brightness = (uint8_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(params, "enabled")))
        dimmer_config.enabled = cJSON_IsTrue(j);

    example_lvgl_unlock();

    save_dimmer_config_to_nvs();
    lv_async_call(_deferred_serial_dimmer_subscribe, NULL);

    _send_ok(id);
}

/* ── system.health ──────────────────────────────────────────────────────── */

static void _handle_system_health(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "uptime_s",
        (double)(esp_timer_get_time() / 1000000ULL));
    cJSON_AddNumberToObject(r, "heap_free",
        (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(r, "heap_min_free",
        (double)esp_get_minimum_free_heap_size());
    cJSON_AddNumberToObject(r, "psram_free",
        (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    int rssi = 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        rssi = ap_info.rssi;
    }
    cJSON_AddNumberToObject(r, "wifi_rssi", rssi);
    _send_response(id, r, NULL);
}

/* ── system.reboot ──────────────────────────────────────────────────────── */

static void _handle_system_reboot(int id, cJSON *params)
{
    (void)params;
    _send_ok(id);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

/* ── Data Logger serial commands ────────────────────────────────────────── */

static void _deferred_serial_log_start(void *arg)
{
    (void)arg;
    data_logger_start();
}

static void _handle_log_start(int id, cJSON *params)
{
    (void)params;
    lv_async_call(_deferred_serial_log_start, NULL);
    _send_ok(id);
}

static void _deferred_serial_log_stop(void *arg)
{
    (void)arg;
    data_logger_stop();
}

static void _handle_log_stop(int id, cJSON *params)
{
    (void)params;
    lv_async_call(_deferred_serial_log_stop, NULL);
    _send_ok(id);
}

static void _handle_log_status(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "active", data_logger_is_active());
    cJSON_AddStringToObject(r, "file", data_logger_current_file());
    cJSON_AddNumberToObject(r, "samples", data_logger_get_sample_count());
    cJSON_AddNumberToObject(r, "elapsed_ms", data_logger_get_elapsed_ms());
    _send_response(id, r, NULL);
}

static void _handle_log_list(int id, cJSON *params)
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

static void _handle_log_delete(int id, cJSON *params)
{
    cJSON *j = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(j) || !_name_is_safe(j->valuestring)) {
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

/* ── Fuel calibration serial commands ───────────────────────────────────── */

static void _handle_fuel_status(int id, cJSON *params)
{
    (void)params;
    fuel_cal_config_t fc;
    signal_internal_get_fuel_cal(&fc);
    float voltage = signal_internal_get_fuel_voltage();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "voltage", voltage);
    cJSON_AddNumberToObject(r, "empty_v", fc.empty_v);
    cJSON_AddNumberToObject(r, "full_v", fc.full_v);
    cJSON_AddNumberToObject(r, "full_value", fc.full_value);
    cJSON_AddBoolToObject(r, "enabled", fc.enabled);
    _send_response(id, r, NULL);
}

static void _handle_fuel_set_empty(int id, cJSON *params)
{
    (void)params;
    float v = signal_internal_get_fuel_voltage();
    fuel_cal_config_t fc;
    signal_internal_get_fuel_cal(&fc);
    signal_internal_set_fuel_cal(v, fc.full_v, fc.full_value, fc.enabled);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "empty_v", v);
    _send_response(id, r, NULL);
}

static void _handle_fuel_set_full(int id, cJSON *params)
{
    (void)params;
    float v = signal_internal_get_fuel_voltage();
    fuel_cal_config_t fc;
    signal_internal_get_fuel_cal(&fc);
    signal_internal_set_fuel_cal(fc.empty_v, v, fc.full_value, true);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "full_v", v);
    _send_response(id, r, NULL);
}

/* ── WiFi config serial commands ────────────────────────────────────────── */

static void _handle_wifi_config_get(int id, cJSON *params)
{
    (void)params;
    wifi_credentials_t creds = {0};
    config_store_load_wifi(&creds);

    wifi_boot_config_t boot = {0};
    config_store_load_wifi_boot(&boot);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "ssid", creds.ssid);
    cJSON_AddStringToObject(r, "password", creds.password);
    cJSON_AddBoolToObject(r, "auto_connect", creds.auto_connect);
    cJSON_AddBoolToObject(r, "wifi_on_boot", boot.wifi_on_boot);
    _send_response(id, r, NULL);
}

static void _handle_wifi_config_set(int id, cJSON *params)
{
    wifi_credentials_t creds = {0};
    config_store_load_wifi(&creds);

    cJSON *j;
    if ((j = cJSON_GetObjectItem(params, "ssid")) && cJSON_IsString(j)) {
        strncpy(creds.ssid, j->valuestring, sizeof(creds.ssid) - 1);
        creds.ssid[sizeof(creds.ssid) - 1] = '\0';
    }
    if ((j = cJSON_GetObjectItem(params, "password")) && cJSON_IsString(j)) {
        strncpy(creds.password, j->valuestring, sizeof(creds.password) - 1);
        creds.password[sizeof(creds.password) - 1] = '\0';
    }
    if ((j = cJSON_GetObjectItem(params, "auto_connect")))
        creds.auto_connect = cJSON_IsTrue(j);
    config_store_save_wifi(&creds);

    if ((j = cJSON_GetObjectItem(params, "wifi_on_boot"))) {
        wifi_boot_config_t boot = {0};
        config_store_load_wifi_boot(&boot);
        boot.wifi_on_boot = cJSON_IsTrue(j);
        config_store_save_wifi_boot(&boot);
    }

    _send_ok(id);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Dispatch Table
 * ══════════════════════════════════════════════════════════════════════════ */

typedef void (*cmd_handler_fn)(int id, cJSON *params);

typedef struct {
    const char   *method;
    cmd_handler_fn handler;
} cmd_entry_t;

static const cmd_entry_t s_dispatch_table[] = {
    /* Device */
    { "device.info",        _handle_device_info },
    { "storage.info",       _handle_storage_info },
    /* Layouts */
    { "layout.list",        _handle_layout_list },
    { "layout.current",     _handle_layout_current },
    { "layout.raw",         _handle_layout_raw },
    { "layout.save",        _handle_layout_save },
    { "layout.set",         _handle_layout_set },
    { "layout.delete",      _handle_layout_delete },
    { "layout.version",     _handle_layout_version },
    /* Splash */
    { "splash.list",        _handle_splash_list },
    /* Images */
    { "image.list",         _handle_image_list },
    { "image.delete",       _handle_image_delete },
    /* Chunked downloads (images + fonts) */
    { "download.start",     _handle_download_start },
    { "download.chunk",     _handle_download_chunk },
    /* Fonts */
    { "font.list",          _handle_font_list },
    { "font.delete",        _handle_font_delete },
    /* Brightness */
    { "brightness.get",     _handle_brightness_get },
    { "brightness.set",     _handle_brightness_set },
    /* CAN config */
    { "can.config.get",     _handle_can_config_get },
    { "can.config.set",     _handle_can_config_set },
    /* Dimmer */
    { "dimmer.get",         _handle_dimmer_get },
    { "dimmer.set",         _handle_dimmer_set },
    /* System */
    { "system.health",      _handle_system_health },
    { "system.reboot",      _handle_system_reboot },
    /* Signals */
    { "signal.values",      _handle_signal_values },
    { "signal.inject",      _handle_signal_inject },
    { "signal.simulate",    _handle_signal_simulate },
    /* Data Logger */
    { "log.start",          _handle_log_start },
    { "log.stop",           _handle_log_stop },
    { "log.status",         _handle_log_status },
    { "log.list",           _handle_log_list },
    { "log.delete",         _handle_log_delete },
    /* Fuel Calibration */
    { "fuel.status",        _handle_fuel_status },
    { "fuel.set-empty",     _handle_fuel_set_empty },
    { "fuel.set-full",      _handle_fuel_set_full },
    /* WiFi Config */
    { "wifi.config.get",    _handle_wifi_config_get },
    { "wifi.config.set",    _handle_wifi_config_set },
    /* Screenshot */
    { "screenshot",         _handle_screenshot },
    /* Chunked uploads */
    { "upload.start",       _handle_upload_start },
    { "upload.finish",      _handle_upload_finish },
    { "upload.abort",       _handle_upload_abort },
};

#define DISPATCH_TABLE_SIZE \
    (sizeof(s_dispatch_table) / sizeof(s_dispatch_table[0]))

/* ══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════════════ */

esp_err_t serial_commands_dispatch(const char *json_str, size_t len)
{
    (void)len;
    cJSON *req = cJSON_Parse(json_str);
    if (!req) {
        ESP_LOGW(TAG, "Invalid JSON request");
        _send_error(0, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id_item = cJSON_GetObjectItem(req, "id");
    int id = cJSON_IsNumber(id_item) ? (int)id_item->valuedouble : 0;

    cJSON *method_item = cJSON_GetObjectItem(req, "method");
    if (!cJSON_IsString(method_item)) {
        cJSON_Delete(req);
        _send_error(id, "Missing 'method'");
        return ESP_FAIL;
    }
    const char *method = method_item->valuestring;

    cJSON *params = cJSON_GetObjectItem(req, "params");
    bool params_owned = false;
    if (!params) {
        params = cJSON_CreateObject(); /* empty params — owned separately */
        params_owned = true;
    }

    /* Look up handler in dispatch table */
    bool found = false;
    for (size_t i = 0; i < DISPATCH_TABLE_SIZE; i++) {
        if (strcmp(method, s_dispatch_table[i].method) == 0) {
            s_dispatch_table[i].handler(id, params);
            found = true;
            break;
        }
    }

    if (!found) {
        ESP_LOGW(TAG, "Unknown method: %s", method);
        _send_error(id, "Unknown method");
    }

    if (params_owned) cJSON_Delete(params);
    cJSON_Delete(req);
    return ESP_OK;
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
