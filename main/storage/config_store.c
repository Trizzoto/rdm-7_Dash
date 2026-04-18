#include "config_store.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "config_store";

/* ── NVS namespace strings ────────────────────────────────────────────── */
#define NS_CAN      "can_config"
#define NS_DIMMER   "dimmer_cfg"

/* ═══════════════════════════════════════════════════════════════════════
 *  DIMMER
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_dimmer(const brightness_dimmer_config_t *cfg)
{
    nvs_handle_t handle;
    if (nvs_open(NS_DIMMER, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;

    esp_err_t err;
    err = nvs_set_str(handle, "sig_name", cfg->signal_name);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS set sig_name failed"); nvs_close(handle); return err; }
    err = nvs_set_u16(handle, "thresh",   (uint16_t)(cfg->threshold * 100.0f));
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS set thresh failed"); nvs_close(handle); return err; }
    err = nvs_set_u8 (handle, "is_mom",   cfg->is_momentary ? 1 : 0);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS set is_mom failed"); nvs_close(handle); return err; }
    err = nvs_set_u8 (handle, "invert",   cfg->invert        ? 1 : 0);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS set invert failed"); nvs_close(handle); return err; }
    err = nvs_set_u8 (handle, "dim_br",   cfg->dim_brightness);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS set dim_br failed"); nvs_close(handle); return err; }
    err = nvs_set_u8 (handle, "enabled",  cfg->enabled        ? 1 : 0);
    if (err != ESP_OK) { ESP_LOGW(TAG, "NVS set enabled failed"); nvs_close(handle); return err; }

    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_dimmer(brightness_dimmer_config_t *cfg)
{
    nvs_handle_t handle;
    if (nvs_open(NS_DIMMER, NVS_READONLY, &handle) != ESP_OK) return ESP_FAIL;

    size_t len = sizeof(cfg->signal_name);
    if (nvs_get_str(handle, "sig_name", cfg->signal_name, &len) != ESP_OK)
        cfg->signal_name[0] = '\0';

    uint16_t u16; uint8_t u8;
    if (nvs_get_u16(handle, "thresh",  &u16) == ESP_OK) cfg->threshold      = u16 / 100.0f;
    if (nvs_get_u8 (handle, "is_mom",  &u8)  == ESP_OK) cfg->is_momentary   = (u8 == 1);
    if (nvs_get_u8 (handle, "invert",  &u8)  == ESP_OK) cfg->invert         = (u8 == 1);
    if (nvs_get_u8 (handle, "dim_br",  &u8)  == ESP_OK) cfg->dim_brightness = u8;
    if (nvs_get_u8 (handle, "enabled", &u8)  == ESP_OK) cfg->enabled        = (u8 == 1);

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
    esp_err_t err = nvs_set_u8(handle, "can_bitrate", bitrate);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_bitrate(uint8_t *bitrate)
{
    if (!bitrate) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    if (nvs_open(NS_CAN, NVS_READONLY, &handle) != ESP_OK) return ESP_FAIL;
    nvs_get_u8(handle, "can_bitrate", bitrate); /* keeps default if key absent */
    nvs_close(handle);
    return ESP_OK;
}


/* ═══════════════════════════════════════════════════════════════════════
 *  WIFI CREDENTIALS
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_WIFI "wifi_cfg"

esp_err_t config_store_save_wifi(const wifi_credentials_t *creds)
{
    if (!creds) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_str(handle, "ssid", creds->ssid);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    err = nvs_set_str(handle, "password", creds->password);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    err = nvs_set_u8(handle, "auto_con", creds->auto_connect ? 1 : 0);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "WiFi credentials saved for '%s'", creds->ssid);
    return err;
}

esp_err_t config_store_load_wifi(wifi_credentials_t *creds)
{
    if (!creds) return ESP_ERR_INVALID_ARG;
    memset(creds, 0, sizeof(*creds));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    size_t len = sizeof(creds->ssid);
    if (nvs_get_str(handle, "ssid", creds->ssid, &len) != ESP_OK)
        creds->ssid[0] = '\0';

    len = sizeof(creds->password);
    if (nvs_get_str(handle, "password", creds->password, &len) != ESP_OK)
        creds->password[0] = '\0';

    uint8_t ac = 0;
    if (nvs_get_u8(handle, "auto_con", &ac) == ESP_OK)
        creds->auto_connect = (ac != 0);

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t config_store_clear_wifi(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_erase_all(handle);
    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "WiFi credentials cleared");
    return err;
}

/* ── Multi-SSID list (#19) ─────────────────────────────────────────────
   Layout in NVS namespace NS_WIFI:
     key                      value
     ssid / password / auto_con   legacy slot (kept for back-compat — mirrors slot 0)
     list_count                u8 number of entries in the list (0..CONFIG_STORE_WIFI_SLOT_COUNT)
     ssid_0 ... ssid_4         str SSID for each slot
     pw_0   ... pw_4           str password for each slot

   On first boot after an upgrade, the legacy single creds are migrated to
   slot 0 transparently on first call to _load_list. */

static void _wifi_slot_keys(uint8_t i, char ssid_key[16], char pw_key[16]) {
    snprintf(ssid_key, 16, "ssid_%u", (unsigned)i);
    snprintf(pw_key,   16, "pw_%u",   (unsigned)i);
}

esp_err_t config_store_load_wifi_list(wifi_credentials_t out[CONFIG_STORE_WIFI_SLOT_COUNT], uint8_t *count)
{
    if (!out || !count) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(wifi_credentials_t) * CONFIG_STORE_WIFI_SLOT_COUNT);
    *count = 0;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    uint8_t n = 0;
    if (nvs_get_u8(handle, "list_count", &n) != ESP_OK) {
        /* Legacy / no list yet — attempt to migrate the single-SSID store to slot 0 */
        size_t len = sizeof(out[0].ssid);
        if (nvs_get_str(handle, "ssid", out[0].ssid, &len) == ESP_OK && out[0].ssid[0] != '\0') {
            len = sizeof(out[0].password);
            if (nvs_get_str(handle, "password", out[0].password, &len) != ESP_OK) out[0].password[0] = '\0';
            uint8_t ac = 0;
            (void) nvs_get_u8(handle, "auto_con", &ac);
            out[0].auto_connect = (ac != 0);
            *count = 1;
        }
        nvs_close(handle);
        return ESP_OK;
    }

    if (n > CONFIG_STORE_WIFI_SLOT_COUNT) n = CONFIG_STORE_WIFI_SLOT_COUNT;
    for (uint8_t i = 0; i < n; i++) {
        char ssid_key[16], pw_key[16];
        _wifi_slot_keys(i, ssid_key, pw_key);
        size_t len = sizeof(out[i].ssid);
        if (nvs_get_str(handle, ssid_key, out[i].ssid, &len) != ESP_OK) out[i].ssid[0] = '\0';
        len = sizeof(out[i].password);
        if (nvs_get_str(handle, pw_key,   out[i].password, &len) != ESP_OK) out[i].password[0] = '\0';
        out[i].auto_connect = true;
    }
    *count = n;
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t config_store_save_wifi_list(const wifi_credentials_t *entries, uint8_t count)
{
    if (count > CONFIG_STORE_WIFI_SLOT_COUNT) count = CONFIG_STORE_WIFI_SLOT_COUNT;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    /* Write new entries */
    for (uint8_t i = 0; i < count; i++) {
        char ssid_key[16], pw_key[16];
        _wifi_slot_keys(i, ssid_key, pw_key);
        if ((err = nvs_set_str(handle, ssid_key, entries[i].ssid)) != ESP_OK) { nvs_close(handle); return err; }
        if ((err = nvs_set_str(handle, pw_key,   entries[i].password)) != ESP_OK) { nvs_close(handle); return err; }
    }
    /* Clear unused slots so stale entries don't reappear */
    for (uint8_t i = count; i < CONFIG_STORE_WIFI_SLOT_COUNT; i++) {
        char ssid_key[16], pw_key[16];
        _wifi_slot_keys(i, ssid_key, pw_key);
        nvs_erase_key(handle, ssid_key);
        nvs_erase_key(handle, pw_key);
    }

    if ((err = nvs_set_u8(handle, "list_count", count)) != ESP_OK) { nvs_close(handle); return err; }

    /* Keep the legacy single-cred keys mirroring slot 0 for backwards compat */
    if (count > 0) {
        nvs_set_str(handle, "ssid",     entries[0].ssid);
        nvs_set_str(handle, "password", entries[0].password);
        nvs_set_u8(handle,  "auto_con", entries[0].auto_connect ? 1 : 0);
    } else {
        nvs_erase_key(handle, "ssid");
        nvs_erase_key(handle, "password");
    }

    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "WiFi list saved (%u entries)", (unsigned)count);
    return err;
}

esp_err_t config_store_add_wifi(const wifi_credentials_t *entry)
{
    if (!entry || entry->ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    /* Heap-allocate the list. sys_evt task calls this path on STA connect and
     * only has ~2.5 KB of stack — a 5-entry list on-stack (~500 B) plus NVS
     * operations reliably blew that stack. */
    wifi_credentials_t *list = calloc(CONFIG_STORE_WIFI_SLOT_COUNT, sizeof(*list));
    if (!list) return ESP_ERR_NO_MEM;
    uint8_t count = 0;
    config_store_load_wifi_list(list, &count);

    esp_err_t ret;
    /* Overwrite if SSID already present */
    for (uint8_t i = 0; i < count; i++) {
        if (strncmp(list[i].ssid, entry->ssid, sizeof(list[i].ssid)) == 0) {
            list[i] = *entry;
            ret = config_store_save_wifi_list(list, count);
            free(list);
            return ret;
        }
    }

    /* Append, or evict oldest (shift left, append at end) if full */
    if (count < CONFIG_STORE_WIFI_SLOT_COUNT) {
        list[count] = *entry;
        count++;
    } else {
        for (uint8_t i = 0; i < CONFIG_STORE_WIFI_SLOT_COUNT - 1; i++) list[i] = list[i + 1];
        list[CONFIG_STORE_WIFI_SLOT_COUNT - 1] = *entry;
        count = CONFIG_STORE_WIFI_SLOT_COUNT;
    }
    ret = config_store_save_wifi_list(list, count);
    free(list);
    return ret;
}

esp_err_t config_store_remove_wifi(const char *ssid)
{
    if (!ssid) return ESP_ERR_INVALID_ARG;
    /* Heap-allocate for the same reason as config_store_add_wifi. */
    wifi_credentials_t *list = calloc(CONFIG_STORE_WIFI_SLOT_COUNT, sizeof(*list));
    if (!list) return ESP_ERR_NO_MEM;
    uint8_t count = 0;
    config_store_load_wifi_list(list, &count);

    uint8_t found = CONFIG_STORE_WIFI_SLOT_COUNT;
    for (uint8_t i = 0; i < count; i++) {
        if (strncmp(list[i].ssid, ssid, sizeof(list[i].ssid)) == 0) { found = i; break; }
    }
    if (found >= count) { free(list); return ESP_ERR_NOT_FOUND; }

    for (uint8_t i = found; i < count - 1; i++) list[i] = list[i + 1];
    memset(&list[count - 1], 0, sizeof(list[0]));
    count--;
    esp_err_t ret = config_store_save_wifi_list(list, count);
    free(list);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  WIFI AP (HOTSPOT) SETTINGS
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_WIFI_AP "wifi_ap_cfg"

esp_err_t config_store_save_ap_config(const rdm_ap_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI_AP, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_u8(handle, "enabled", cfg->enabled ? 1 : 0);
    nvs_set_str(handle, "password", cfg->password);
    err = nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "AP config saved (enabled=%d)", cfg->enabled);
    return err;
}

esp_err_t config_store_load_ap_config(rdm_ap_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* Defaults — AP disabled until user explicitly enables it */
    cfg->enabled = false;
    strncpy(cfg->password, "rdm7dash", sizeof(cfg->password) - 1);
    cfg->password[sizeof(cfg->password) - 1] = '\0';

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI_AP, NVS_READONLY, &handle);
    if (err != ESP_OK) return ESP_OK; /* use defaults if namespace missing */

    uint8_t u8;
    if (nvs_get_u8(handle, "enabled", &u8) == ESP_OK) cfg->enabled = (u8 != 0);

    size_t len = sizeof(cfg->password);
    nvs_get_str(handle, "password", cfg->password, &len);

    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  WIFI BOOT SETTINGS
 * ═══════════════════════════════════════════════════════════════════════ */
esp_err_t config_store_save_wifi_boot(const wifi_boot_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    nvs_set_u8(handle, "on_boot", cfg->wifi_on_boot ? 1 : 0);
    nvs_set_u8(handle, "ap_en", cfg->ap_enabled ? 1 : 0);
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_wifi_boot(wifi_boot_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    /* Defaults */
    cfg->wifi_on_boot = false;
    cfg->ap_enabled = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI, NVS_READONLY, &handle);
    if (err != ESP_OK) return ESP_OK; /* use defaults if namespace missing */

    uint8_t u8;
    if (nvs_get_u8(handle, "on_boot", &u8) == ESP_OK) cfg->wifi_on_boot = (u8 != 0);
    if (nvs_get_u8(handle, "ap_en", &u8) == ESP_OK) cfg->ap_enabled = (u8 != 0);

    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  SPLASH FADE
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_SPLASH "splash_cfg"

esp_err_t config_store_save_splash_fade(bool enabled)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_SPLASH, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "fade", enabled ? 1 : 0);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_splash_fade(bool *enabled)
{
    if (!enabled) return ESP_ERR_INVALID_ARG;
    *enabled = true; /* default: fade enabled */
    nvs_handle_t handle;
    if (nvs_open(NS_SPLASH, NVS_READONLY, &handle) != ESP_OK) return ESP_OK;
    uint8_t u8;
    if (nvs_get_u8(handle, "fade", &u8) == ESP_OK) *enabled = (u8 != 0);
    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  DATA LOGGER RATE
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_DATALOG "dataloggr"

esp_err_t config_store_save_log_rate_hz(uint16_t hz)
{
    if (hz > 1000) hz = 1000;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_DATALOG, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(handle, "rate_hz", hz);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_log_rate_hz(uint16_t *hz)
{
    if (!hz) return ESP_ERR_INVALID_ARG;
    *hz = 10; /* default: 10 Hz */
    nvs_handle_t handle;
    if (nvs_open(NS_DATALOG, NVS_READONLY, &handle) != ESP_OK) return ESP_OK;
    uint16_t v;
    if (nvs_get_u16(handle, "rate_hz", &v) == ESP_OK) {
        if (v > 1000) v = 1000;
        *hz = v;
    }
    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  ECU SELECTION (make + version)
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_ECU "ecu_cfg"

esp_err_t config_store_save_ecu(const char *make, const char *version)
{
    if (!make || !version) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_ECU, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "make", make);
    if (err == ESP_OK) err = nvs_set_str(handle, "version", version);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_ecu(char *make, size_t m_len,
                                char *version, size_t v_len)
{
    if (!make || !version || m_len == 0 || v_len == 0) return ESP_ERR_INVALID_ARG;
    make[0] = '\0';
    version[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(NS_ECU, NVS_READONLY, &handle) != ESP_OK) return ESP_ERR_NOT_FOUND;
    size_t n = m_len;
    esp_err_t err_m = nvs_get_str(handle, "make", make, &n);
    n = v_len;
    esp_err_t err_v = nvs_get_str(handle, "version", version, &n);
    nvs_close(handle);
    if (err_m != ESP_OK || err_v != ESP_OK) { make[0] = '\0'; version[0] = '\0'; return ESP_ERR_NOT_FOUND; }
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  FIRST-RUN FLAG (#17)
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_FIRST_RUN "first_run"

esp_err_t config_store_save_first_run_done(bool done)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_FIRST_RUN, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "done", done ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) ESP_LOGI(TAG, "first_run_done = %d", done);
    return err;
}

esp_err_t config_store_load_first_run_done(bool *done)
{
    if (!done) return ESP_ERR_INVALID_ARG;
    *done = false;
    nvs_handle_t handle;
    if (nvs_open(NS_FIRST_RUN, NVS_READONLY, &handle) != ESP_OK) return ESP_OK;
    uint8_t u8 = 0;
    if (nvs_get_u8(handle, "done", &u8) == ESP_OK) *done = (u8 != 0);
    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  DISPLAY ROTATION + NIGHT MODE (#23)
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_DISPLAY "display_cfg"

esp_err_t config_store_save_rotation(uint8_t rot)
{
    if (rot > 3) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_DISPLAY, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, "rot", rot);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) ESP_LOGI(TAG, "Display rotation saved: %u", (unsigned)rot);
    return err;
}

esp_err_t config_store_load_rotation(uint8_t *rot)
{
    if (!rot) return ESP_ERR_INVALID_ARG;
    *rot = 0;
    nvs_handle_t handle;
    if (nvs_open(NS_DISPLAY, NVS_READONLY, &handle) != ESP_OK) return ESP_OK;
    uint8_t u8 = 0;
    if (nvs_get_u8(handle, "rot", &u8) == ESP_OK && u8 <= 3) *rot = u8;
    nvs_close(handle);
    return ESP_OK;
}

esp_err_t config_store_save_night_mode(const night_mode_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_DISPLAY, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    nvs_set_u8(handle, "nm_en",       cfg->enabled ? 1 : 0);
    nvs_set_u8(handle, "nm_manual",   cfg->manual_active ? 1 : 0);
    uint8_t br = cfg->night_brightness;
    if (br < 5) br = 5;
    if (br > 100) br = 100;
    nvs_set_u8(handle, "nm_bright",   br);
    err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) ESP_LOGI(TAG, "Night mode saved (enabled=%d manual=%d bright=%u)",
                                cfg->enabled, cfg->manual_active, (unsigned)br);
    return err;
}

esp_err_t config_store_load_night_mode(night_mode_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    memset(cfg, 0, sizeof(*cfg));
    cfg->night_brightness = 25; /* sane default */
    nvs_handle_t handle;
    if (nvs_open(NS_DISPLAY, NVS_READONLY, &handle) != ESP_OK) return ESP_OK;
    uint8_t u8 = 0;
    if (nvs_get_u8(handle, "nm_en",     &u8) == ESP_OK) cfg->enabled = (u8 != 0);
    if (nvs_get_u8(handle, "nm_manual", &u8) == ESP_OK) cfg->manual_active = (u8 != 0);
    if (nvs_get_u8(handle, "nm_bright", &u8) == ESP_OK && u8 >= 5 && u8 <= 100)
        cfg->night_brightness = u8;
    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  FACTORY RESET
 * ═══════════════════════════════════════════════════════════════════════ */

void config_store_factory_reset(void)
{
    ESP_LOGW(TAG, "=== FACTORY RESET ===");

    /* ── Wipe NVS completely ─────────────────────────────────────────────
     * Erase the entire "nvs" partition in one shot. This catches every
     * namespace used anywhere in the firmware (current and future) without
     * having to maintain a hand-curated list, and is what factory-state
     * devices start with. The next call to nvs_flash_init() on boot will
     * recreate an empty NVS. */
    esp_err_t err = nvs_flash_deinit();
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "nvs_flash_deinit: %s", esp_err_to_name(err));
    }
    err = nvs_flash_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_erase FAILED: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "NVS partition erased");
    }

    /* ── Wipe LittleFS completely ────────────────────────────────────────
     * Unmount (if currently mounted) and format the partition. Formatting
     * is a single-shot wipe — far more reliable than iterating directories
     * and unlink-ing each entry. The next mount (via layout_manager_init)
     * will find an empty filesystem and regenerate /lfs/layouts/default.json
     * from compiled-in data plus reseed the embedded RDM logo via
     * boot_assets_seed_defaults(). */
    esp_err_t u = esp_vfs_littlefs_unregister("littlefs");
    if (u != ESP_OK && u != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_vfs_littlefs_unregister: %s", esp_err_to_name(u));
    }
    esp_err_t f = esp_littlefs_format("littlefs");
    if (f != ESP_OK) {
        ESP_LOGE(TAG, "esp_littlefs_format FAILED: %s", esp_err_to_name(f));
    } else {
        ESP_LOGW(TAG, "LittleFS partition formatted");
    }

    ESP_LOGW(TAG, "Factory reset complete — rebooting to apply");
}
