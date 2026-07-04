/**
 * serial_commands_sd.c — SD card serial JSON-RPC handlers.
 *
 * Methods: sd.status, sd.files, sd.copy, sd.delete.
 */
#include "serial_commands_internal.h"
#include "sd_file_ops.h"

#include "cJSON.h"
#include "esp_log.h"
#include "storage/boot_assets.h"
#include "storage/sd_manager.h"

#include <string.h>

static const char *TAG = "serial_cmd";

void _handle_sd_status(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "mounted", sd_manager_is_mounted());
    if (sd_manager_is_mounted()) {
        size_t total = 0, used = 0, avail = 0;
        if (sd_manager_get_info(&total, &used, &avail) == ESP_OK) {
            cJSON_AddNumberToObject(r, "total", total);
            cJSON_AddNumberToObject(r, "used", used);
            cJSON_AddNumberToObject(r, "free", avail);
        }
    }
    _send_response(id, r, NULL);
}

void _handle_sd_files(int id, cJSON *params)
{
    (void)params;
    if (!sd_manager_is_mounted()) {
        _send_error(id, "SD card not mounted");
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON *layouts = cJSON_AddArrayToObject(r, "layouts");
    sd_list_dir(layouts, SD_LAYOUT_DIR, ".json", 5);
    cJSON *images = cJSON_AddArrayToObject(r, "images");
    sd_list_dir(images, SD_IMAGE_DIR, ".rdmimg", 7);
    cJSON *fonts = cJSON_AddArrayToObject(r, "fonts");
    sd_list_dir(fonts, SD_FONT_DIR, ".ttf", 4);
    _send_response(id, r, NULL);
}

void _handle_sd_copy(int id, cJSON *params)
{
    cJSON *type_item = cJSON_GetObjectItem(params, "type");
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    cJSON *dir_item  = cJSON_GetObjectItem(params, "direction");
    if (!cJSON_IsString(type_item) || !cJSON_IsString(name_item)
        || !cJSON_IsString(dir_item)) {
        _send_error(id, "Missing type/name/direction");
        return;
    }
    const char *type = type_item->valuestring;
    const char *name = name_item->valuestring;
    const char *direction = dir_item->valuestring;
    if (!_name_is_safe(name)) { _send_error(id, "Invalid name"); return; }
    if (!sd_manager_is_mounted()) { _send_error(id, "SD card not mounted"); return; }

    const char *lfs_dir = NULL, *sd_dir = NULL, *ext = NULL;
    if (strcmp(type, "layout") == 0) {
        lfs_dir = "/lfs/layouts"; sd_dir = SD_LAYOUT_DIR; ext = ".json";
    } else if (strcmp(type, "image") == 0) {
        lfs_dir = LFS_IMAGE_DIR; sd_dir = SD_IMAGE_DIR; ext = ".rdmimg";
    } else if (strcmp(type, "font") == 0) {
        lfs_dir = LFS_FONT_DIR; sd_dir = SD_FONT_DIR; ext = ".ttf";
    } else {
        _send_error(id, "Invalid type"); return;
    }

    /* Built-in assets are locked — never copy them to or from SD. */
    if ((strcmp(type, "font")   == 0 && boot_assets_is_protected_font(name)) ||
        (strcmp(type, "layout") == 0 && boot_assets_is_protected_layout(name)) ||
        (strcmp(type, "image")  == 0 && boot_assets_is_protected_image(name))) {
        _send_error(id, "Built-in asset cannot be moved to/from SD");
        return;
    }

    char src[96], dst[96];
    bool to_sd = (strcmp(direction, "to_sd") == 0);
    if (to_sd) {
        snprintf(src, sizeof(src), "%s/%s%s", lfs_dir, name, ext);
        snprintf(dst, sizeof(dst), "%s/%s%s", sd_dir, name, ext);
        /* Ensure SD subdirectory exists */
        _ensure_dir(sd_dir);
    } else if (strcmp(direction, "from_sd") == 0) {
        snprintf(src, sizeof(src), "%s/%s%s", sd_dir, name, ext);
        snprintf(dst, sizeof(dst), "%s/%s%s", lfs_dir, name, ext);
    } else {
        _send_error(id, "direction must be 'to_sd' or 'from_sd'"); return;
    }

    esp_err_t err = sd_copy_file(src, dst);
    if (err == ESP_ERR_NOT_FOUND) { _send_error(id, "Source file not found"); return; }
    if (err != ESP_OK)            { _send_error(id, "Copy failed"); return; }
    ESP_LOGI(TAG, "SD copy: %s '%s' %s", type, name, to_sd ? "to SD" : "from SD");
    _send_ok(id);
}

void _handle_sd_delete(int id, cJSON *params)
{
    cJSON *type_item = cJSON_GetObjectItem(params, "type");
    cJSON *name_item = cJSON_GetObjectItem(params, "name");
    if (!cJSON_IsString(type_item) || !cJSON_IsString(name_item)) {
        _send_error(id, "Missing type/name"); return;
    }
    const char *type = type_item->valuestring;
    const char *name = name_item->valuestring;
    if (!_name_is_safe(name)) { _send_error(id, "Invalid name"); return; }
    if (!sd_manager_is_mounted()) { _send_error(id, "SD card not mounted"); return; }

    char path[96];
    if (strcmp(type, "layout") == 0)
        snprintf(path, sizeof(path), "%s/%s.json", SD_LAYOUT_DIR, name);
    else if (strcmp(type, "image") == 0)
        snprintf(path, sizeof(path), "%s/%s.rdmimg", SD_IMAGE_DIR, name);
    else if (strcmp(type, "font") == 0)
        snprintf(path, sizeof(path), "%s/%s.ttf", SD_FONT_DIR, name);
    else { _send_error(id, "Invalid type"); return; }

    if (remove(path) != 0) { _send_error(id, "File not found on SD"); return; }
    ESP_LOGI(TAG, "SD delete: %s '%s'", type, name);
    _send_ok(id);
}
