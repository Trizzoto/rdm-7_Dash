/**
 * serial_commands.c — JSON-RPC command dispatcher for UART serial protocol.
 *
 * Each method handler calls the same core logic as the HTTP web server
 * handlers, then sends a JSON response frame over UART.
 *
 * Domain handlers live in the sibling serial_commands_<domain>.c files
 * (layout, assets, system, signals, logger, sd, fuel, capture, upload).
 * This core file owns the shared response/path-safety helpers, the small
 * device/storage handlers, the dispatch table, and the dispatch loop.
 */
#include "serial_commands.h"
#include "serial_commands_internal.h"
#include "system/rdm_lv_async.h"
#include "serial_protocol.h"
#include "uart_protocol.h"
#include "sd_file_ops.h"

#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "layout/layout_manager.h"
#include "storage/sd_manager.h"
#include "system/device_id.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "serial_cmd";

/* ── Helper: path-safety check (replicates web_server.c _name_is_safe) ── */

bool _name_is_safe(const char *name)
{
    if (!name || !name[0]) return false;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '.' || *p < 0x20) return false;
    }
    return true;
}

/* Like _name_is_safe but allows dots (for filenames with extensions like .csv) */
bool _filename_is_safe(const char *name)
{
    if (!name || !name[0]) return false;
    /* Block path traversal (..) */
    if (strstr(name, "..")) return false;
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p < 0x20) return false;
    }
    return true;
}

/* ── Helper: ensure directories exist ───────────────────────────────────── */

void _ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0775);
    }
}

/* ── Helper: send JSON-RPC response ─────────────────────────────────────── */

void _send_response(int id, cJSON *result, const char *error)
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
        esp_err_t err = serial_protocol_send_json(json);
        /* Temporary: chasing replies that leave the dash but never reach the
         * phone. Says which transport carried it and whether the transport
         * accepted it, so "the dash did not answer X" can be pinned to the
         * dash, the link, or the app. */
        ESP_LOGI(TAG, "reply id=%d via %s len=%u -> %s", id,
                 serial_protocol_get_name(serial_protocol_get_active()),
                 (unsigned)strlen(json), esp_err_to_name(err));
        free(json);
    }
}

void _send_ok(int id)
{
    cJSON *r = cJSON_CreateString("ok");
    _send_response(id, r, NULL);
}

void _send_error(int id, const char *msg)
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

/* ══════════════════════════════════════════════════════════════════════════
 *  Dispatch Table
 * ══════════════════════════════════════════════════════════════════════════ */

typedef void (*cmd_handler_fn)(int id, cJSON *params);

typedef struct {
    const char   *method;
    cmd_handler_fn handler;
} cmd_entry_t;

/* Handlers defined in the domain files — see serial_commands_internal.h for
 * the shared plumbing they all rely on (response/path-safety helpers). */
void _handle_layout_list(int id, cJSON *params);
void _handle_layout_current(int id, cJSON *params);
void _handle_layout_raw(int id, cJSON *params);
void _handle_layout_save(int id, cJSON *params);
void _handle_layout_preview(int id, cJSON *params);
void _handle_layout_set(int id, cJSON *params);
void _handle_layout_delete(int id, cJSON *params);
void _handle_layout_reset(int id, cJSON *params);
void _handle_layout_version(int id, cJSON *params);
void _handle_splash_list(int id, cJSON *params);

void _handle_image_list(int id, cJSON *params);
void _handle_image_delete(int id, cJSON *params);
void _handle_download_start(int id, cJSON *params);
void _handle_download_chunk(int id, cJSON *params);
void _handle_font_list(int id, cJSON *params);
void _handle_font_delete(int id, cJSON *params);
void _handle_track_list(int id, cJSON *params);
void _handle_track_delete(int id, cJSON *params);

void _handle_brightness_get(int id, cJSON *params);
void _handle_brightness_set(int id, cJSON *params);
void _handle_can_config_get(int id, cJSON *params);
void _handle_can_config_set(int id, cJSON *params);
void _handle_system_health(int id, cJSON *params);
void _handle_system_reboot(int id, cJSON *params);
void _handle_dimmer_get(int id, cJSON *params);
void _handle_dimmer_set(int id, cJSON *params);
void _handle_wifi_config_get(int id, cJSON *params);
void _handle_wifi_config_set(int id, cJSON *params);

void _handle_signal_values(int id, cJSON *params);
void _handle_signal_inject(int id, cJSON *params);
void _handle_signal_simulate(int id, cJSON *params);

void _handle_log_start(int id, cJSON *params);
void _handle_log_stop(int id, cJSON *params);
void _handle_log_status(int id, cJSON *params);
void _handle_log_list(int id, cJSON *params);
void _handle_log_delete(int id, cJSON *params);
void _handle_log_download_start(int id, cJSON *params);
void _handle_log_download_chunk(int id, cJSON *params);
void _handle_log_config_get(int id, cJSON *params);
void _handle_log_config_set(int id, cJSON *params);
void _handle_replay_start(int id, cJSON *params);
void _handle_replay_stop(int id, cJSON *params);
void _handle_replay_status(int id, cJSON *params);

void _handle_sd_status(int id, cJSON *params);
void _handle_sd_files(int id, cJSON *params);
void _handle_sd_copy(int id, cJSON *params);
void _handle_sd_delete(int id, cJSON *params);

void _handle_fuel_status(int id, cJSON *params);
void _handle_fuel_set_empty(int id, cJSON *params);
void _handle_fuel_set_full(int id, cJSON *params);

void _handle_screenshot(int id, cJSON *params);

void _handle_upload_start(int id, cJSON *params);
void _handle_upload_finish(int id, cJSON *params);
void _handle_upload_abort(int id, cJSON *params);

static const cmd_entry_t s_dispatch_table[] = {
    /* Device */
    { "device.info",        _handle_device_info },
    { "storage.info",       _handle_storage_info },
    /* Layouts */
    { "layout.list",        _handle_layout_list },
    { "layout.current",     _handle_layout_current },
    { "layout.raw",         _handle_layout_raw },
    { "layout.save",        _handle_layout_save },
    { "layout.preview",     _handle_layout_preview },
    { "layout.set",         _handle_layout_set },
    { "layout.delete",      _handle_layout_delete },
    { "layout.reset",       _handle_layout_reset },
    { "layout.version",     _handle_layout_version },
    /* Splash */
    { "splash.list",        _handle_splash_list },
    /* Images */
    { "image.list",         _handle_image_list },
    { "image.delete",       _handle_image_delete },
    /* Track maps */
    { "track.list",         _handle_track_list },
    { "track.delete",       _handle_track_delete },
    /* Chunked downloads (images + fonts + tracks) */
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
    { "log.download.start", _handle_log_download_start },
    { "log.download.chunk", _handle_log_download_chunk },
    { "log.config.get",     _handle_log_config_get },
    { "log.config.set",     _handle_log_config_set },
    { "replay.start",       _handle_replay_start },
    { "replay.stop",        _handle_replay_stop },
    { "replay.status",      _handle_replay_status },
    /* SD Card */
    { "sd.status",          _handle_sd_status },
    { "sd.files",           _handle_sd_files },
    { "sd.copy",            _handle_sd_copy },
    { "sd.delete",          _handle_sd_delete },
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

    ESP_LOGI(TAG, "dispatch id=%d %s via %s", id, method,
             serial_protocol_get_name(serial_protocol_get_active()));

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
