#include "config_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "config_store";

/* ── Slot-index constants (mirror of defines in ui_Screen3.c) ─────────── */
#define RPM_VALUE_IDX   8   /* RPM_VALUE_ID  9  → index 8  */
#define SPEED_VALUE_IDX 9   /* SPEED_VALUE_ID 10 → index 9  */
#define GEAR_VALUE_IDX  10  /* GEAR_VALUE_ID  11 → index 10 */
#define BAR1_VALUE_IDX  11  /* BAR1_VALUE_ID  12 → index 11 */
#define BAR2_VALUE_IDX  12  /* BAR2_VALUE_ID  13 → index 12 */

/* ── Globals that live in ui_Screen3.c, accessed here via extern ─────── */
extern char label_texts[13][64];
extern int  rpm_gauge_max;
extern int  rpm_redline_value;

/* ── NVS namespace strings ────────────────────────────────────────────── */
#define NS_CAN      "can_config"
#define NS_WARN     "warn_config"
#define NS_DIMMER   "dimmer_cfg"
#define NS_ECU      "ecu_config"

/* ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_init(void)
{
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  VALUES  —  save
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_values(const value_config_t *cfg, uint8_t count)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_CAN, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save_values: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    char key[32];
    bool ok = true;

    for (int i = 0; i < count && ok; i++) {

        /* Yield every 3 slots to keep the watchdog happy */
        if (i % 3 == 0) vTaskDelay(pdMS_TO_TICKS(1));

#define CHK(e) do { err = (e); if (err != ESP_OK) { \
    ESP_LOGE(TAG, "save_values[%d] key=%s: %s", i, key, esp_err_to_name(err)); \
    ok = false; goto loop_end; } } while(0)

        snprintf(key, sizeof(key), "enabled%d",   i); CHK(nvs_set_u8  (handle, key, cfg[i].enabled  ? 1 : 0));
        snprintf(key, sizeof(key), "can_id%d",    i); CHK(nvs_set_u32 (handle, key, cfg[i].can_id));
        snprintf(key, sizeof(key), "endian%d",    i); CHK(nvs_set_u8  (handle, key, cfg[i].endianess ? 1 : 0));
        snprintf(key, sizeof(key), "bit_st%d",    i); CHK(nvs_set_u8  (handle, key, cfg[i].bit_start));
        snprintf(key, sizeof(key), "bit_len%d",   i); CHK(nvs_set_u8  (handle, key, cfg[i].bit_length));
        snprintf(key, sizeof(key), "decimals%d",  i); CHK(nvs_set_u8  (handle, key, cfg[i].decimals));
        snprintf(key, sizeof(key), "val_off%d",   i); CHK(nvs_set_blob(handle, key, &cfg[i].value_offset, sizeof(float)));
        snprintf(key, sizeof(key), "scale%d",     i); CHK(nvs_set_blob(handle, key, &cfg[i].scale,        sizeof(float)));
        snprintf(key, sizeof(key), "is_signed%d", i); CHK(nvs_set_u8  (handle, key, cfg[i].is_signed   ? 1 : 0));

        /* RPM-specific fields */
        if (i == RPM_VALUE_IDX) {
            snprintf(key, sizeof(key), "rpm_color%d",     i); CHK(nvs_set_u32(handle, key, cfg[i].rpm_bar_color.full));
            snprintf(key, sizeof(key), "rpm_limit_eff%d", i); CHK(nvs_set_u8 (handle, key, cfg[i].rpm_limiter_effect));
            snprintf(key, sizeof(key), "rpm_limit_val%d", i); CHK(nvs_set_i32(handle, key, cfg[i].rpm_limiter_value));
            snprintf(key, sizeof(key), "rpm_limit_col%d", i); CHK(nvs_set_u32(handle, key, cfg[i].rpm_limiter_color.full));
            snprintf(key, sizeof(key), "rpm_lights_en%d", i); CHK(nvs_set_u8 (handle, key, cfg[i].rpm_lights_enabled     ? 1 : 0));
            snprintf(key, sizeof(key), "rpm_bg_en%d",     i); CHK(nvs_set_u8 (handle, key, cfg[i].rpm_background_enabled  ? 1 : 0));
            snprintf(key, sizeof(key), "rpm_bg_val%d",    i); CHK(nvs_set_i32(handle, key, cfg[i].rpm_background_value));
            snprintf(key, sizeof(key), "rpm_bg_col%d",    i); CHK(nvs_set_u32(handle, key, cfg[i].rpm_background_color.full));
        }

        /* Panel warning thresholds & colours (slots 0-7) */
        if (i < 8) {
            snprintf(key, sizeof(key), "warn_hi_th%d",  i); CHK(nvs_set_blob(handle, key, &cfg[i].warning_high_threshold, sizeof(float)));
            snprintf(key, sizeof(key), "warn_lo_th%d",  i); CHK(nvs_set_blob(handle, key, &cfg[i].warning_low_threshold,  sizeof(float)));
            snprintf(key, sizeof(key), "warn_hi_col%d", i); CHK(nvs_set_u32 (handle, key, cfg[i].warning_high_color.full));
            snprintf(key, sizeof(key), "warn_lo_col%d", i); CHK(nvs_set_u32 (handle, key, cfg[i].warning_low_color.full));
            uint8_t warn_flags = (cfg[i].warning_high_enabled ? 0x02 : 0)
                               | (cfg[i].warning_low_enabled  ? 0x01 : 0);
            snprintf(key, sizeof(key), "warn_enabled%d", i); CHK(nvs_set_u8(handle, key, warn_flags));
        }

        /* Bar-specific fields (slots 11 and 12) */
        if (i == BAR1_VALUE_IDX || i == BAR2_VALUE_IDX) {
            snprintf(key, sizeof(key), "bar_min%d",    i); CHK(nvs_set_i32(handle, key, cfg[i].bar_min));
            snprintf(key, sizeof(key), "bar_max%d",    i); CHK(nvs_set_i32(handle, key, cfg[i].bar_max));
            snprintf(key, sizeof(key), "bar_low%d",    i); CHK(nvs_set_i32(handle, key, cfg[i].bar_low));
            snprintf(key, sizeof(key), "bar_high%d",   i); CHK(nvs_set_i32(handle, key, cfg[i].bar_high));
            snprintf(key, sizeof(key), "blc%d",        i); CHK(nvs_set_u32(handle, key, cfg[i].bar_low_color.full));
            snprintf(key, sizeof(key), "bhc%d",        i); CHK(nvs_set_u32(handle, key, cfg[i].bar_high_color.full));
            snprintf(key, sizeof(key), "birc%d",       i); CHK(nvs_set_u32(handle, key, cfg[i].bar_in_range_color.full));
            snprintf(key, sizeof(key), "show_val%d",   i); CHK(nvs_set_u8 (handle, key, cfg[i].show_bar_value   ? 1 : 0));
            snprintf(key, sizeof(key), "invert_val%d", i); CHK(nvs_set_u8 (handle, key, cfg[i].invert_bar_value ? 1 : 0));
            snprintf(key, sizeof(key), "fuel_sndr%d",  i); CHK(nvs_set_u8 (handle, key, cfg[i].fuel_sender      ? 1 : 0));

            uint32_t fs_empty_bits = 0, fs_full_bits = 0;
            memcpy(&fs_empty_bits, &cfg[i].fuel_sender_empty_v, sizeof(float));
            memcpy(&fs_full_bits,  &cfg[i].fuel_sender_full_v,  sizeof(float));
            snprintf(key, sizeof(key), "fs_empty%d", i); CHK(nvs_set_u32(handle, key, fs_empty_bits));
            snprintf(key, sizeof(key), "fs_full%d",  i); CHK(nvs_set_u32(handle, key, fs_full_bits));
            snprintf(key, sizeof(key), "fs_filt%d",  i); CHK(nvs_set_u8 (handle, key, cfg[i].fuel_sender_filter));
        }

        /* Speed-specific (slot 9) */
        if (i == SPEED_VALUE_IDX) {
            snprintf(key, sizeof(key), "use_gps%d", i); CHK(nvs_set_u8(handle, key, cfg[i].use_gps_for_speed ? 1 : 0));
            snprintf(key, sizeof(key), "use_mph%d", i); CHK(nvs_set_u8(handle, key, cfg[i].use_mph           ? 1 : 0));
        }

        /* Gear-specific (slot 10) */
        if (i == GEAR_VALUE_IDX) {
            snprintf(key, sizeof(key), "gear_mode%d", i); CHK(nvs_set_u8(handle, key, cfg[i].gear_detection_mode));

            for (int j = 0; j < 14 && ok; j++) {
                snprintf(key, sizeof(key), "gear_val%d_%d", i, j);
                CHK(nvs_set_u32(handle, key, cfg[i].gear_custom_values[j]));
            }

            snprintf(key, sizeof(key), "tire_circ%d", i); CHK(nvs_set_blob(handle, key, &cfg[i].tire_circumference_mm, sizeof(float)));
            snprintf(key, sizeof(key), "final_dr%d",  i); CHK(nvs_set_blob(handle, key, &cfg[i].final_drive_ratio,     sizeof(float)));
            snprintf(key, sizeof(key), "rev_gear%d",  i); CHK(nvs_set_blob(handle, key, &cfg[i].reverse_gear_ratio,    sizeof(float)));

            for (int j = 0; j < 10 && ok; j++) {
                snprintf(key, sizeof(key), "gear_rat%d_%d", i, j);
                CHK(nvs_set_blob(handle, key, &cfg[i].gear_ratios[j], sizeof(float)));
            }

            for (int j = 0; j < 7 && ok; j++) {
                snprintf(key, sizeof(key), "icon_type%d_%d", i, j);
                CHK(nvs_set_u8(handle, key, cfg[i].custom_icon_types[j]));
                snprintf(key, sizeof(key), "icon_val%d_%d", i, j);
                CHK(nvs_set_u32(handle, key, cfg[i].custom_icon_values[j]));
            }
        }

loop_end:;
#undef CHK
    }

    /* Labels */
    for (int i = 0; i < count && ok; i++) {
        if (i % 5 == 0) vTaskDelay(pdMS_TO_TICKS(1));
        snprintf(key, sizeof(key), "label%d", i);
        err = nvs_set_str(handle, key, label_texts[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "save_values label[%d]: %s", i, esp_err_to_name(err));
            ok = false;
        }
    }

    /* Custom texts for panels (slots 0-7) */
    for (int i = 0; i < 8 && ok; i++) {
        snprintf(key, sizeof(key), "custom_text%d", i);
        err = nvs_set_str(handle, key, cfg[i].custom_text);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "save_values custom_text[%d]: %s", i, esp_err_to_name(err));
            ok = false;
        }
    }

    /* RPM gauge globals */
    if (ok) { err = nvs_set_i32(handle, "rpm_max",     rpm_gauge_max);     if (err != ESP_OK) ok = false; }
    if (ok) { err = nvs_set_i32(handle, "rpm_redline", rpm_redline_value); if (err != ESP_OK) ok = false; }

    if (ok) {
        err = nvs_commit(handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "save_values commit: %s", esp_err_to_name(err));
            ok = false;
        }
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "save_values %s", ok ? "OK" : "FAILED");
    return ok ? ESP_OK : ESP_FAIL;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  VALUES  —  load
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_load_values(value_config_t *cfg, uint8_t count)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_CAN, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load_values: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    char key[32];
    uint8_t  u8;
    uint32_t u32;
    int32_t  i32;
    float    fv;
    size_t   fsz;

    for (int i = 0; i < count; i++) {

        snprintf(key, sizeof(key), "enabled%d",   i); if (nvs_get_u8 (handle, key, &u8)  == ESP_OK) cfg[i].enabled     = (bool)u8;
        snprintf(key, sizeof(key), "can_id%d",    i); if (nvs_get_u32(handle, key, &u32) == ESP_OK) cfg[i].can_id      = u32;
        snprintf(key, sizeof(key), "endian%d",    i); if (nvs_get_u8 (handle, key, &u8)  == ESP_OK) cfg[i].endianess   = u8;
        snprintf(key, sizeof(key), "bit_st%d",    i); if (nvs_get_u8 (handle, key, &u8)  == ESP_OK) cfg[i].bit_start   = u8;
        snprintf(key, sizeof(key), "bit_len%d",   i); if (nvs_get_u8 (handle, key, &u8)  == ESP_OK) cfg[i].bit_length  = u8;
        snprintf(key, sizeof(key), "decimals%d",  i); if (nvs_get_u8 (handle, key, &u8)  == ESP_OK) cfg[i].decimals    = u8;
        fsz = sizeof(float);
        snprintf(key, sizeof(key), "val_off%d",   i); if (nvs_get_blob(handle, key, &fv, &fsz) == ESP_OK) cfg[i].value_offset = fv;
        fsz = sizeof(float);
        snprintf(key, sizeof(key), "scale%d",     i); if (nvs_get_blob(handle, key, &fv, &fsz) == ESP_OK) cfg[i].scale        = fv;
        snprintf(key, sizeof(key), "is_signed%d", i); if (nvs_get_u8 (handle, key, &u8)  == ESP_OK) cfg[i].is_signed   = (u8 == 1);

        if (i == RPM_VALUE_IDX) {
            snprintf(key, sizeof(key), "rpm_color%d",     i); if (nvs_get_u32(handle, key, &u32) == ESP_OK) cfg[i].rpm_bar_color.full = u32;
            snprintf(key, sizeof(key), "rpm_limit_eff%d", i);
            if (nvs_get_u8 (handle, key, &u8)  == ESP_OK) cfg[i].rpm_limiter_effect = u8;   else cfg[i].rpm_limiter_effect = 0;
            snprintf(key, sizeof(key), "rpm_limit_val%d", i);
            if (nvs_get_i32(handle, key, &i32) == ESP_OK) cfg[i].rpm_limiter_value = i32;   else cfg[i].rpm_limiter_value = 7000;
            snprintf(key, sizeof(key), "rpm_limit_col%d", i);
            if (nvs_get_u32(handle, key, &u32) == ESP_OK) cfg[i].rpm_limiter_color.full = u32;
            snprintf(key, sizeof(key), "rpm_lights_en%d", i);
            if (nvs_get_u8 (handle, key, &u8)  == ESP_OK) cfg[i].rpm_lights_enabled = (u8 == 1); else cfg[i].rpm_lights_enabled = false;
            snprintf(key, sizeof(key), "rpm_bg_en%d",     i);
            if (nvs_get_u8 (handle, key, &u8)  == ESP_OK) cfg[i].rpm_background_enabled = (u8 == 1); else cfg[i].rpm_background_enabled = false;
            snprintf(key, sizeof(key), "rpm_bg_val%d",    i);
            if (nvs_get_i32(handle, key, &i32) == ESP_OK) cfg[i].rpm_background_value = i32; else cfg[i].rpm_background_value = 7000;
            snprintf(key, sizeof(key), "rpm_bg_col%d",    i);
            if (nvs_get_u32(handle, key, &u32) == ESP_OK) cfg[i].rpm_background_color.full = u32;
        }

        if (i < 8) {
            float wf; size_t wfsz = sizeof(float); uint8_t wflags;
            snprintf(key, sizeof(key), "warn_hi_th%d",  i); if (nvs_get_blob(handle, key, &wf, &wfsz) == ESP_OK) cfg[i].warning_high_threshold = wf;
            wfsz = sizeof(float);
            snprintf(key, sizeof(key), "warn_lo_th%d",  i); if (nvs_get_blob(handle, key, &wf, &wfsz) == ESP_OK) cfg[i].warning_low_threshold  = wf;
            snprintf(key, sizeof(key), "warn_hi_col%d", i); if (nvs_get_u32 (handle, key, &u32) == ESP_OK) cfg[i].warning_high_color.full = u32;
            snprintf(key, sizeof(key), "warn_lo_col%d", i); if (nvs_get_u32 (handle, key, &u32) == ESP_OK) cfg[i].warning_low_color.full  = u32;
            snprintf(key, sizeof(key), "warn_enabled%d",i);
            if (nvs_get_u8(handle, key, &wflags) == ESP_OK) {
                cfg[i].warning_high_enabled = (wflags & 0x02) != 0;
                cfg[i].warning_low_enabled  = (wflags & 0x01) != 0;
            }
        }

        if (i == BAR1_VALUE_IDX || i == BAR2_VALUE_IDX) {
            snprintf(key, sizeof(key), "bar_min%d",    i); if (nvs_get_i32(handle, key, &i32) == ESP_OK) cfg[i].bar_min = i32;
            snprintf(key, sizeof(key), "bar_max%d",    i); if (nvs_get_i32(handle, key, &i32) == ESP_OK) cfg[i].bar_max = i32;
            snprintf(key, sizeof(key), "bar_low%d",    i);
            if (nvs_get_i32(handle, key, &i32) == ESP_OK) cfg[i].bar_low  = i32; else cfg[i].bar_low  = 25;
            snprintf(key, sizeof(key), "bar_high%d",   i);
            if (nvs_get_i32(handle, key, &i32) == ESP_OK) cfg[i].bar_high = i32; else cfg[i].bar_high = 75;
            snprintf(key, sizeof(key), "blc%d",        i); if (nvs_get_u32(handle, key, &u32) == ESP_OK) cfg[i].bar_low_color.full      = u32;
            snprintf(key, sizeof(key), "bhc%d",        i); if (nvs_get_u32(handle, key, &u32) == ESP_OK) cfg[i].bar_high_color.full     = u32;
            snprintf(key, sizeof(key), "birc%d",       i); if (nvs_get_u32(handle, key, &u32) == ESP_OK) cfg[i].bar_in_range_color.full = u32;
            snprintf(key, sizeof(key), "show_val%d",   i);
            if (nvs_get_u8(handle, key, &u8) == ESP_OK) cfg[i].show_bar_value   = (u8 == 1); else cfg[i].show_bar_value = true;
            snprintf(key, sizeof(key), "invert_val%d", i);
            if (nvs_get_u8(handle, key, &u8) == ESP_OK) cfg[i].invert_bar_value = (u8 == 1); else cfg[i].invert_bar_value = false;
            snprintf(key, sizeof(key), "fuel_sndr%d",  i);
            if (nvs_get_u8(handle, key, &u8) == ESP_OK) cfg[i].fuel_sender = (u8 == 1); else cfg[i].fuel_sender = false;

            uint32_t fs_bits = 0;
            snprintf(key, sizeof(key), "fs_empty%d", i);
            if (nvs_get_u32(handle, key, &fs_bits) == ESP_OK) memcpy(&cfg[i].fuel_sender_empty_v, &fs_bits, sizeof(float)); else cfg[i].fuel_sender_empty_v = 0.0f;
            fs_bits = 0;
            snprintf(key, sizeof(key), "fs_full%d",  i);
            if (nvs_get_u32(handle, key, &fs_bits) == ESP_OK) memcpy(&cfg[i].fuel_sender_full_v,  &fs_bits, sizeof(float)); else cfg[i].fuel_sender_full_v  = 3.3f;
            snprintf(key, sizeof(key), "fs_filt%d",  i);
            if (nvs_get_u8(handle, key, &u8) == ESP_OK) cfg[i].fuel_sender_filter = u8; else cfg[i].fuel_sender_filter = 0;
        }

        if (i == SPEED_VALUE_IDX) {
            snprintf(key, sizeof(key), "use_gps%d", i);
            if (nvs_get_u8(handle, key, &u8) == ESP_OK) cfg[i].use_gps_for_speed = (u8 == 1); else cfg[i].use_gps_for_speed = false;
            snprintf(key, sizeof(key), "use_mph%d", i);
            if (nvs_get_u8(handle, key, &u8) == ESP_OK) cfg[i].use_mph = (u8 == 1);           else cfg[i].use_mph = false;
        }

        if (i == GEAR_VALUE_IDX) {
            snprintf(key, sizeof(key), "gear_mode%d", i);
            if (nvs_get_u8(handle, key, &u8) == ESP_OK) cfg[i].gear_detection_mode = u8; else cfg[i].gear_detection_mode = 1;

            for (int j = 0; j < 14; j++) {
                snprintf(key, sizeof(key), "gear_val%d_%d", i, j);
                if (nvs_get_u32(handle, key, &u32) == ESP_OK) cfg[i].gear_custom_values[j] = u32;
                else                                           cfg[i].gear_custom_values[j] = UINT32_MAX;
            }

            fsz = sizeof(float);
            snprintf(key, sizeof(key), "tire_circ%d", i); if (nvs_get_blob(handle, key, &fv, &fsz) == ESP_OK) { cfg[i].tire_circumference_mm = fv; }
            fsz = sizeof(float);
            snprintf(key, sizeof(key), "final_dr%d",  i); if (nvs_get_blob(handle, key, &fv, &fsz) == ESP_OK) { cfg[i].final_drive_ratio     = fv; }
            fsz = sizeof(float);
            snprintf(key, sizeof(key), "rev_gear%d",  i); if (nvs_get_blob(handle, key, &fv, &fsz) == ESP_OK) { cfg[i].reverse_gear_ratio    = fv; }

            for (int j = 0; j < 10; j++) {
                fsz = sizeof(float);
                snprintf(key, sizeof(key), "gear_rat%d_%d", i, j);
                if (nvs_get_blob(handle, key, &fv, &fsz) == ESP_OK) cfg[i].gear_ratios[j] = fv;
            }

            for (int j = 0; j < 7; j++) {
                snprintf(key, sizeof(key), "icon_type%d_%d", i, j);
                if (nvs_get_u8(handle, key, &u8) == ESP_OK) cfg[i].custom_icon_types[j]  = u8;  else cfg[i].custom_icon_types[j]  = 1;
                snprintf(key, sizeof(key), "icon_val%d_%d",  i, j);
                if (nvs_get_u32(handle, key, &u32) == ESP_OK) cfg[i].custom_icon_values[j] = u32; else cfg[i].custom_icon_values[j] = UINT32_MAX;
            }
        }
    }

    /* Labels */
    for (int i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "label%d", i);
        size_t sz = sizeof(label_texts[i]);
        nvs_get_str(handle, key, label_texts[i], &sz);
    }

    /* Custom texts */
    for (int i = 0; i < 8; i++) {
        snprintf(key, sizeof(key), "custom_text%d", i);
        size_t sz = sizeof(cfg[i].custom_text);
        if (nvs_get_str(handle, key, cfg[i].custom_text, &sz) != ESP_OK)
            cfg[i].custom_text[0] = '\0';
    }

    /* RPM gauge globals */
    if (nvs_get_i32(handle, "rpm_max",     &i32) == ESP_OK) rpm_gauge_max     = i32;
    if (nvs_get_i32(handle, "rpm_redline", &i32) == ESP_OK) rpm_redline_value = i32;

    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  WARNINGS  —  save / load
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_warnings(const warning_config_t *cfg, uint8_t count)
{
    nvs_handle_t handle;
    if (nvs_open(NS_WARN, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;

    char key[32];
    for (int i = 0; i < count; i++) {
        vTaskDelay(pdMS_TO_TICKS(1));
        snprintf(key, sizeof(key), "warn_can_id%d",  i); nvs_set_u32(handle, key, cfg[i].can_id);
        snprintf(key, sizeof(key), "warn_bit_pos%d", i); nvs_set_u8 (handle, key, cfg[i].bit_position);
        snprintf(key, sizeof(key), "warn_color%d",   i); nvs_set_u32(handle, key, cfg[i].active_color.full);
        snprintf(key, sizeof(key), "warn_label%d",   i); nvs_set_str(handle, key, cfg[i].label);
        snprintf(key, sizeof(key), "warn_inv%d",     i); nvs_set_u8 (handle, key, cfg[i].invert_toggle  ? 1 : 0);
        snprintf(key, sizeof(key), "warn_is_mom%d",  i); nvs_set_u8 (handle, key, cfg[i].is_momentary   ? 1 : 0);
        snprintf(key, sizeof(key), "warn_state%d",   i); nvs_set_u8 (handle, key, cfg[i].current_state  ? 1 : 0);
    }

    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_warnings(warning_config_t *cfg, uint8_t count)
{
    nvs_handle_t handle;
    if (nvs_open(NS_WARN, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;

    char key[32];
    uint8_t  u8;
    uint32_t u32;

    for (int i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "warn_can_id%d",  i); if (nvs_get_u32(handle, key, &u32) == ESP_OK) cfg[i].can_id          = u32;
        snprintf(key, sizeof(key), "warn_bit_pos%d", i); if (nvs_get_u8 (handle, key, &u8)  == ESP_OK) cfg[i].bit_position    = u8;
        snprintf(key, sizeof(key), "warn_color%d",   i); if (nvs_get_u32(handle, key, &u32) == ESP_OK) cfg[i].active_color.full = u32;

        snprintf(key, sizeof(key), "warn_label%d", i);
        size_t lsz = sizeof(cfg[i].label);
        nvs_get_str(handle, key, cfg[i].label, &lsz);

        snprintf(key, sizeof(key), "warn_is_mom%d", i);
        if (nvs_get_u8(handle, key, &u8) == ESP_OK) cfg[i].is_momentary   = (u8 == 1);
        else                                         cfg[i].is_momentary   = true;

        snprintf(key, sizeof(key), "warn_state%d", i);
        if (nvs_get_u8(handle, key, &u8) == ESP_OK) cfg[i].current_state  = (u8 == 1);
        else                                         cfg[i].current_state  = false;

        snprintf(key, sizeof(key), "warn_inv%d", i);
        if (nvs_get_u8(handle, key, &u8) == ESP_OK) {
            cfg[i].invert_toggle = (u8 == 1);
            if (cfg[i].invert_toggle)
                cfg[i].current_state = true; /* inverted warnings start active */
        } else {
            cfg[i].invert_toggle = false;
        }
    }

    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  INDICATORS  —  save / load
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_indicators(const indicator_config_t *cfg, uint8_t count)
{
    nvs_handle_t handle;
    if (nvs_open(NS_CAN, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;

    char key[32];
    for (int i = 0; i < count; i++) {
        vTaskDelay(pdMS_TO_TICKS(1));
        snprintf(key, sizeof(key), "ind_can_id%d",   i); nvs_set_u32(handle, key, cfg[i].can_id);
        snprintf(key, sizeof(key), "ind_bit_pos%d",  i); nvs_set_u8 (handle, key, cfg[i].bit_position);
        snprintf(key, sizeof(key), "ind_is_mom%d",   i); nvs_set_u8 (handle, key, cfg[i].is_momentary       ? 1 : 0);
        snprintf(key, sizeof(key), "ind_anim%d",     i); nvs_set_u8 (handle, key, cfg[i].animation_enabled  ? 1 : 0);
        snprintf(key, sizeof(key), "ind_input_src%d",i); nvs_set_u8 (handle, key, cfg[i].input_source);
    }

    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_indicators(indicator_config_t *cfg, uint8_t count)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_CAN, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    char key[32];
    uint8_t  u8;
    uint32_t u32;

    for (int i = 0; i < count; i++) {
        snprintf(key, sizeof(key), "ind_can_id%d",    i); if (nvs_get_u32(handle, key, &u32) == ESP_OK) cfg[i].can_id        = u32;
        snprintf(key, sizeof(key), "ind_bit_pos%d",   i); if (nvs_get_u8 (handle, key, &u8)  == ESP_OK) cfg[i].bit_position  = u8;

        snprintf(key, sizeof(key), "ind_is_mom%d", i);
        if (nvs_get_u8(handle, key, &u8) == ESP_OK) cfg[i].is_momentary      = (u8 == 1);
        else                                         cfg[i].is_momentary      = true;

        cfg[i].current_state = false; /* always reset to OFF on boot */

        snprintf(key, sizeof(key), "ind_anim%d", i);
        if (nvs_get_u8(handle, key, &u8) == ESP_OK) cfg[i].animation_enabled = (u8 == 1);
        else                                         cfg[i].animation_enabled = true;

        snprintf(key, sizeof(key), "ind_input_src%d", i);
        if (nvs_get_u8(handle, key, &u8) == ESP_OK && u8 <= 1) cfg[i].input_source = u8;
        else                                                     cfg[i].input_source = 0;
    }

    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  DIMMER
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_dimmer(const brightness_dimmer_config_t *cfg)
{
    nvs_handle_t handle;
    if (nvs_open(NS_DIMMER, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;

    nvs_set_u32(handle, "can_id",  cfg->can_id);
    nvs_set_u8 (handle, "bit_pos", cfg->bit_position);
    nvs_set_u8 (handle, "is_mom",  cfg->is_momentary   ? 1 : 0);
    nvs_set_u8 (handle, "invert",  cfg->invert_toggle   ? 1 : 0);
    nvs_set_u8 (handle, "bright",  cfg->brightness_value);
    nvs_set_u8 (handle, "enabled", cfg->enabled          ? 1 : 0);

    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_dimmer(brightness_dimmer_config_t *cfg)
{
    nvs_handle_t handle;
    if (nvs_open(NS_DIMMER, NVS_READONLY, &handle) != ESP_OK) return ESP_FAIL;

    uint32_t u32; uint8_t u8;
    if (nvs_get_u32(handle, "can_id",  &u32) == ESP_OK) cfg->can_id           = u32;
    if (nvs_get_u8 (handle, "bit_pos", &u8)  == ESP_OK) cfg->bit_position      = u8;
    if (nvs_get_u8 (handle, "is_mom",  &u8)  == ESP_OK) cfg->is_momentary      = (u8 == 1);
    if (nvs_get_u8 (handle, "invert",  &u8)  == ESP_OK) cfg->invert_toggle     = (u8 == 1);
    if (nvs_get_u8 (handle, "bright",  &u8)  == ESP_OK) cfg->brightness_value  = u8;
    if (nvs_get_u8 (handle, "enabled", &u8)  == ESP_OK) cfg->enabled           = (u8 == 1);

    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  CAN BITRATE
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_bitrate(uint8_t bitrate)
{
    nvs_handle_t handle;
    if (nvs_open(NS_CAN, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;
    nvs_set_u8(handle, "can_bitrate", bitrate);
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_bitrate(uint8_t *bitrate)
{
    if (!bitrate) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    if (nvs_open(NS_CAN, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;
    nvs_get_u8(handle, "can_bitrate", bitrate); /* keeps default if key absent */
    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ECU PRESET
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_ecu_preset(uint8_t preconfig, uint8_t version)
{
    nvs_handle_t handle;
    if (nvs_open(NS_ECU, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;
    nvs_set_u8(handle, "ecu_preconfig", preconfig);
    nvs_set_u8(handle, "ecu_version",   version);
    esp_err_t err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_ecu_preset(uint8_t *preconfig, uint8_t *version)
{
    if (!preconfig || !version) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    if (nvs_open(NS_ECU, NVS_READONLY, &handle) != ESP_OK) {
        *preconfig = 0;
        *version   = 0;
        return ESP_FAIL;
    }
    if (nvs_get_u8(handle, "ecu_preconfig", preconfig) != ESP_OK) *preconfig = 0;
    if (nvs_get_u8(handle, "ecu_version",   version)   != ESP_OK) *version   = 0;
    nvs_close(handle);
    return ESP_OK;
}
