/**
 * serial_commands_layout.c — layout + splash serial JSON-RPC handlers.
 *
 * Methods: layout.list, layout.current, layout.raw, layout.save,
 * layout.preview, layout.set, layout.delete, layout.version, splash.list.
 *
 * Also owns the deferred LVGL-task preview-apply and screen-reload helpers
 * used by layout.save/preview/set (private to this file).
 */
#include "serial_commands_internal.h"
#include "system/rdm_lv_async.h"

#include "cJSON.h"
#include "esp_log.h"
#include "layout/layout_manager.h"
#include "ui/dashboard.h"
#include "ui/screens/splash_screen.h"
#include "ui/screens/ui_Screen3.h"
#include "lvgl.h"

#include <string.h>

/* LVGL mutex (defined in main.c) */
extern bool rdm_lvgl_lock(int timeout_ms);
extern void rdm_lvgl_unlock(void);

/* Deferred preview apply — runs on LVGL task. arg is a cJSON* root that
 * this callback takes ownership of and frees. Mirrors the behavior of the
 * HTTP /api/layout/preview endpoint. */
static void _deferred_preview_apply(void *arg)
{
    cJSON *root = (cJSON *)arg;
    if (!root) return;

    if (splash_screen_is_edit_mode()) {
        splash_screen_apply_preview(root);
    } else {
        lv_obj_t *old = lv_disp_get_scr_act(lv_disp_get_default());
        ui_Screen3_preview_layout(root);
        lv_scr_load(ui_Screen3);
        if (old && old != ui_Screen3 && lv_obj_is_valid(old))
            lv_obj_del(old);
    }
    cJSON_Delete(root);
}

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

/* ── layout.list ─────────────────────────────────────────────────────────── */

void _handle_layout_list(int id, cJSON *params)
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

void _handle_layout_current(int id, cJSON *params)
{
    (void)params;
    bool is_splash = splash_screen_is_edit_mode();
    char name[LAYOUT_MAX_NAME];
    if (is_splash) {
        snprintf(name, sizeof(name), "_splash_%s",
                 splash_screen_get_active_name());
    } else if (layout_manager_get_active(name, sizeof(name)) != ESP_OK) {
        _send_error(id, "Failed to read active layout");
        return;
    }

    if (!rdm_lvgl_lock(1000)) {
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
    rdm_lvgl_unlock();

    if (!root) {
        _send_error(id, "Failed to build layout JSON");
        return;
    }
    _send_response(id, root, NULL);
}

/* ── layout.raw ──────────────────────────────────────────────────────────── */

void _handle_layout_raw(int id, cJSON *params)
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

void _handle_layout_save(int id, cJSON *params)
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
    rdm_async_call(_deferred_screen_reload, NULL);
    _send_ok(id);
}

/* ── layout.preview ──────────────────────────────────────────────────────── *
 *
 * Applies the given layout JSON live on the display without saving to
 * LittleFS. Used by the desktop editor's live-preview feature so edits
 * (drag/resize/field changes) are reflected on the device in real time.
 *
 * The layout object is duplicated and ownership transferred to an
 * lv_async_call that runs on the LVGL task. Returns immediately. */
void _handle_layout_preview(int id, cJSON *params)
{
    cJSON *data_item = cJSON_GetObjectItem(params, "data");
    if (!cJSON_IsObject(data_item)) {
        _send_error(id, "Missing 'data' object");
        return;
    }
    /* Duplicate so the async callback owns its own copy. */
    cJSON *copy = cJSON_Duplicate(data_item, 1);
    if (!copy) {
        _send_error(id, "Out of memory");
        return;
    }
    rdm_async_call(_deferred_preview_apply, copy);
    _send_ok(id);
}

/* ── layout.set ──────────────────────────────────────────────────────────── */

void _handle_layout_set(int id, cJSON *params)
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
            rdm_async_call(_deferred_screen_reload, NULL);
    } else {
        layout_manager_set_active(name);
        rdm_async_call(_deferred_screen_reload, NULL);
    }
    _send_ok(id);
}

/* ── layout.delete ───────────────────────────────────────────────────────── */

void _handle_layout_delete(int id, cJSON *params)
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

void _handle_layout_version(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateNumber(layout_manager_get_version());
    _send_response(id, r, NULL);
}

/* ── splash.list ─────────────────────────────────────────────────────────── */

void _handle_splash_list(int id, cJSON *params)
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
