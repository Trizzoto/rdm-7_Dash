#include "config_store.h"
#include "system/device_id.h"   /* get_device_ap_password — per-unit AP default */
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>
#include <stdbool.h>

static const char *TAG = "config_store";

/* ── Small NVS ceremony helpers ───────────────────────────────────────────
 * Many domains below are a single scalar value: open rw -> set -> commit ->
 * close (save), or open ro -> get -> close (load), differing only in the
 * NVS width used. These collapse just that ceremony. Return codes pass
 * through verbatim (raw nvs_open/nvs_set/nvs_get results) so each call
 * site can keep its own contract for what to do with them — some callers
 * propagate the open failure, others always report ESP_OK and fall back to
 * a pre-set default regardless. That difference in *intent* stays in the
 * wrapper; only the ceremony is shared. Domains with per-field custom error
 * messages, multiple fields, blob storage, or a non-raw return convention
 * (e.g. normalizing NVS's specific NOT_FOUND to a generic one) are NOT
 * forced through these — they have real reasons to differ, not just
 * copy-paste drift, and are left as their own hand-written ceremony. */
static esp_err_t chs_save_u8(const char *ns, const char *key, uint8_t val) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t chs_save_u16(const char *ns, const char *key, uint16_t val) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u16(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t chs_save_i8(const char *ns, const char *key, int8_t val) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i8(h, key, val);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* Load helpers don't touch *out on failure (nvs_get_uN's own contract),
 * so callers that pre-set a default before calling get that default back
 * on any failure — open or get — exactly as if they'd checked each step
 * themselves. */
static esp_err_t chs_load_u8(const char *ns, const char *key, uint8_t *out) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_u8(h, key, out);
    nvs_close(h);
    return err;
}

static esp_err_t chs_load_u16(const char *ns, const char *key, uint16_t *out) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_u16(h, key, out);
    nvs_close(h);
    return err;
}

static esp_err_t chs_load_i8(const char *ns, const char *key, int8_t *out) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    err = nvs_get_i8(h, key, out);
    nvs_close(h);
    return err;
}

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

esp_err_t config_store_save_obd_extended(uint8_t extended)
{
    nvs_handle_t handle;
    if (nvs_open(NS_CAN, NVS_READWRITE, &handle) != ESP_OK) return ESP_FAIL;
    esp_err_t err = nvs_set_u8(handle, "obd_ext", extended ? 1 : 0);
    if (err != ESP_OK) { nvs_close(handle); return err; }
    err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_obd_extended(uint8_t *extended)
{
    if (!extended) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    if (nvs_open(NS_CAN, NVS_READONLY, &handle) != ESP_OK) return ESP_FAIL;
    nvs_get_u8(handle, "obd_ext", extended); /* keeps default if key absent */
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

    /* Erase credential keys only — preserve boot config (on_boot, ap_en)
     * so that the user's WiFi/hotspot-on-boot preferences survive a
     * "Forget network" operation. nvs_erase_all would wipe those too. */
    nvs_erase_key(handle, "ssid");
    nvs_erase_key(handle, "password");
    nvs_erase_key(handle, "auto_con");
    nvs_erase_key(handle, "list_count");
    char sk[16], pk[16];
    for (uint8_t i = 0; i < CONFIG_STORE_WIFI_SLOT_COUNT; i++) {
        snprintf(sk, sizeof(sk), "ssid_%u", (unsigned)i);
        snprintf(pk, sizeof(pk), "pw_%u",   (unsigned)i);
        nvs_erase_key(handle, sk);
        nvs_erase_key(handle, pk);
    }

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

/* The hotspot password every unit shipped with before per-device passwords
 * (2026-06-12). A stored value equal to this is the old fleet-wide default
 * that the WiFi screen happened to persist, not something an owner chose —
 * upgrade it to the per-device password on load. */
#define LEGACY_FLEET_AP_PASSWORD "rdm7dash"

esp_err_t config_store_load_ap_config(rdm_ap_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    /* Defaults — AP disabled until user explicitly enables it. Password
     * defaults to the per-device derivation (unique per unit, shown on the
     * dash WiFi screen + first-run wizard) so consumer units never share a
     * guessable fleet-wide hotspot password. Falls back to the legacy
     * constant only if the MAC read itself fails (never seen in practice). */
    cfg->enabled = false;
    if (get_device_ap_password(cfg->password, sizeof(cfg->password)) != ESP_OK) {
        strncpy(cfg->password, LEGACY_FLEET_AP_PASSWORD, sizeof(cfg->password) - 1);
        cfg->password[sizeof(cfg->password) - 1] = '\0';
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_WIFI_AP, NVS_READONLY, &handle);
    if (err != ESP_OK) return ESP_OK; /* use defaults if namespace missing */

    uint8_t u8;
    if (nvs_get_u8(handle, "enabled", &u8) == ESP_OK) cfg->enabled = (u8 != 0);

    char stored[sizeof(cfg->password)];
    size_t len = sizeof(stored);
    if (nvs_get_str(handle, "password", stored, &len) == ESP_OK &&
        strcmp(stored, LEGACY_FLEET_AP_PASSWORD) != 0) {
        /* Owner-chosen password wins over the derived default. */
        memcpy(cfg->password, stored, sizeof(stored));
    }

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
    /* Defaults — WiFi on by default; AP off unless explicitly enabled */
    cfg->wifi_on_boot = true;
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
    return chs_save_u8(NS_SPLASH, "fade", enabled ? 1 : 0);
}

esp_err_t config_store_load_splash_fade(bool *enabled)
{
    if (!enabled) return ESP_ERR_INVALID_ARG;
    *enabled = true; /* default: fade enabled */
    uint8_t u8;
    if (chs_load_u8(NS_SPLASH, "fade", &u8) == ESP_OK) *enabled = (u8 != 0);
    return ESP_OK;
}

esp_err_t config_store_save_splash_enabled(bool enabled)
{
    return chs_save_u8(NS_SPLASH, "enabled", enabled ? 1 : 0);
}

esp_err_t config_store_load_splash_enabled(bool *enabled)
{
    if (!enabled) return ESP_ERR_INVALID_ARG;
    *enabled = true; /* default: splash enabled */
    uint8_t u8;
    if (chs_load_u8(NS_SPLASH, "enabled", &u8) == ESP_OK) *enabled = (u8 != 0);
    return ESP_OK;
}

esp_err_t config_store_save_boot_anim(bool enabled)
{
    return chs_save_u8(NS_SPLASH, "bootanim", enabled ? 1 : 0);
}

esp_err_t config_store_load_boot_anim(bool *enabled)
{
    if (!enabled) return ESP_ERR_INVALID_ARG;
    *enabled = true; /* default: animation on */
    uint8_t u8;
    if (chs_load_u8(NS_SPLASH, "bootanim", &u8) == ESP_OK) *enabled = (u8 != 0);
    return ESP_OK;
}

esp_err_t config_store_save_boot_anim_style(uint8_t style)
{
    return chs_save_u8(NS_SPLASH, "bootanimstyle", style);
}

esp_err_t config_store_load_boot_anim_style(uint8_t *style)
{
    if (!style) return ESP_ERR_INVALID_ARG;
    *style = BOOT_ANIM_STYLE_FADE; /* default: individual top-to-bottom fade */
    uint8_t u8;
    if (chs_load_u8(NS_SPLASH, "bootanimstyle", &u8) == ESP_OK &&
        u8 <= BOOT_ANIM_STYLE_CURTAIN)
        *style = u8;
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  DATA LOGGER RATE
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_DATALOG "dataloggr"

esp_err_t config_store_save_log_rate_hz(uint16_t hz)
{
    if (hz > 1000) hz = 1000;
    return chs_save_u16(NS_DATALOG, "rate_hz", hz);
}

esp_err_t config_store_load_log_rate_hz(uint16_t *hz)
{
    if (!hz) return ESP_ERR_INVALID_ARG;
    *hz = 10; /* default: 10 Hz */
    uint16_t v;
    if (chs_load_u16(NS_DATALOG, "rate_hz", &v) == ESP_OK) {
        if (v > 1000) v = 1000;
        *hz = v;
    }
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  EDITOR SETTINGS
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_EDITOR "editor"

esp_err_t config_store_save_edit_step_px(int8_t step)
{
    if (step != 1 && step != 5 && step != 10) step = 5;
    return chs_save_i8(NS_EDITOR, "step_px", step);
}

esp_err_t config_store_load_edit_step_px(int8_t *step)
{
    if (!step) return ESP_ERR_INVALID_ARG;
    *step = 5; /* default: 5 px */
    int8_t v;
    if (chs_load_i8(NS_EDITOR, "step_px", &v) == ESP_OK) {
        if (v == 1 || v == 5 || v == 10) *step = v;
    }
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

/* Base CAN id the active ECU preset's stream was rebased to, or 0 for
 * stock ids. Lives beside make/version because the three "regenerate the
 * default layout and re-apply the stored ECU" paths need it: without it a
 * reset silently drops the user back to the stock base and every widget
 * reads "--". */
esp_err_t config_store_save_ecu_base_id(uint32_t base_id)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_ECU, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if (base_id == 0) {
        /* Absent means stock — erase rather than store a sentinel so an
         * older firmware reading this namespace sees what it expects. */
        err = nvs_erase_key(handle, "base_id");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        err = nvs_set_u32(handle, "base_id", base_id);
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

uint32_t config_store_load_ecu_base_id(void)
{
    nvs_handle_t handle;
    if (nvs_open(NS_ECU, NVS_READONLY, &handle) != ESP_OK) return 0;
    uint32_t v = 0;
    if (nvs_get_u32(handle, "base_id", &v) != ESP_OK) v = 0;
    nvs_close(handle);
    return v;
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
 *  DASHBOARD SWITCHER (ordered pinned-layout cycle for the dash arrows)
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_LAYOUT_SWITCHER "layswit"

esp_err_t config_store_save_layout_switcher(const char *csv)
{
    if (!csv) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS_LAYOUT_SWITCHER, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    /* Empty string deletes the key — caller's "use default cycle" signal. */
    if (csv[0] == '\0') {
        esp_err_t del = nvs_erase_key(h, "csv");
        if (del == ESP_ERR_NVS_NOT_FOUND) del = ESP_OK;
        if (del == ESP_OK) err = nvs_commit(h); else err = del;
    } else {
        err = nvs_set_str(h, "csv", csv);
        if (err == ESP_OK) err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t config_store_load_layout_switcher(char *buf, size_t cap)
{
    if (!buf || cap == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NS_LAYOUT_SWITCHER, NVS_READONLY, &h) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    size_t n = cap;
    esp_err_t err = nvs_get_str(h, "csv", buf, &n);
    nvs_close(h);
    if (err != ESP_OK) buf[0] = '\0';
    return err;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  GEAR CALCULATION (ratios + wheel circumference + final drive)
 *  Stored as a single blob. Used by signal_internal to compute
 *  CALCULATED_GEAR from RPM / VEHICLE_SPEED each LVGL tick.
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_GEAR_CAL "gear_cal"

esp_err_t config_store_save_gear_cal(const gear_cal_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_GEAR_CAL, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "cfg", cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "gear cal saved: wheel=%.3fm fd=%.3f n=%u en=%d",
                 cfg->wheel_circumference_m, cfg->final_drive,
                 (unsigned)cfg->ratio_count, (int)cfg->enabled);
    }
    return err;
}

esp_err_t config_store_load_gear_cal(gear_cal_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    /* Sensible defaults — generic manual 5-speed, 1.95 m tyre, 4.11 diff.
     * enabled=false so nothing publishes until the user confirms setup. */
    memset(cfg, 0, sizeof(*cfg));
    cfg->wheel_circumference_m = 1.95f;
    cfg->final_drive           = 4.11f;
    cfg->ratio_count           = 6;   /* N + 5 forward */
    cfg->ratios[0] = 0.0f;
    cfg->ratios[1] = 3.321f;
    cfg->ratios[2] = 1.902f;
    cfg->ratios[3] = 1.308f;
    cfg->ratios[4] = 1.000f;
    cfg->ratios[5] = 0.759f;
    strncpy(cfg->rpm_signal,   "RPM",           sizeof(cfg->rpm_signal)   - 1);
    strncpy(cfg->speed_signal, "VEHICLE_SPEED", sizeof(cfg->speed_signal) - 1);
    cfg->enabled = false;

    nvs_handle_t handle;
    if (nvs_open(NS_GEAR_CAL, NVS_READONLY, &handle) != ESP_OK) return ESP_OK;
    size_t len = sizeof(*cfg);
    gear_cal_config_t loaded;
    if (nvs_get_blob(handle, "cfg", &loaded, &len) == ESP_OK &&
        len == sizeof(*cfg)) {
        /* Sanity-clamp in case NVS was corrupted or a build bumped fields. */
        if (loaded.ratio_count > GEAR_CAL_MAX_GEARS) loaded.ratio_count = GEAR_CAL_MAX_GEARS;
        if (loaded.wheel_circumference_m < 0.1f ||
            loaded.wheel_circumference_m > 5.0f) loaded.wheel_circumference_m = 1.95f;
        if (loaded.final_drive < 1.0f || loaded.final_drive > 10.0f) loaded.final_drive = 4.11f;
        if (loaded.rpm_signal[0]   == '\0') strncpy(loaded.rpm_signal,   "RPM",           sizeof(loaded.rpm_signal)   - 1);
        if (loaded.speed_signal[0] == '\0') strncpy(loaded.speed_signal, "VEHICLE_SPEED", sizeof(loaded.speed_signal) - 1);
        *cfg = loaded;
    }
    nvs_close(handle);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  FIRST-RUN FLAG (#17)
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_FIRST_RUN "first_run"

esp_err_t config_store_save_first_run_done(bool done)
{
    esp_err_t err = chs_save_u8(NS_FIRST_RUN, "done", done ? 1 : 0);
    if (err == ESP_OK) ESP_LOGI(TAG, "first_run_done = %d", done);
    return err;
}

esp_err_t config_store_load_first_run_done(bool *done)
{
    if (!done) return ESP_ERR_INVALID_ARG;
    *done = false;
    uint8_t u8 = 0;
    if (chs_load_u8(NS_FIRST_RUN, "done", &u8) == ESP_OK) *done = (u8 != 0);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  WIRE INPUT MODE (GPIO 43/44 repurposed from UART1)
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_WIRE_INPUT "wire_input"

esp_err_t config_store_save_wire_input_mode(bool enabled)
{
    return chs_save_u8(NS_WIRE_INPUT, "enabled", enabled ? 1 : 0);
}

esp_err_t config_store_load_wire_input_mode(bool *enabled)
{
    if (!enabled) return ESP_ERR_INVALID_ARG;
    *enabled = false; /* default: UART1 active, wire inputs off */
    uint8_t u8 = 0;
    if (chs_load_u8(NS_WIRE_INPUT, "enabled", &u8) == ESP_OK) *enabled = (u8 != 0);
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  DISPLAY ROTATION + NIGHT MODE (#23)
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_DISPLAY "display_cfg"

esp_err_t config_store_save_rotation(uint8_t rot)
{
    if (rot > 3) return ESP_ERR_INVALID_ARG;
    esp_err_t err = chs_save_u8(NS_DISPLAY, "rot", rot);
    if (err == ESP_OK) ESP_LOGI(TAG, "Display rotation saved: %u", (unsigned)rot);
    return err;
}

esp_err_t config_store_load_rotation(uint8_t *rot)
{
    if (!rot) return ESP_ERR_INVALID_ARG;
    *rot = 0;
    uint8_t u8 = 0;
    if (chs_load_u8(NS_DISPLAY, "rot", &u8) == ESP_OK && u8 <= 3) *rot = u8;
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
 *  OTA SKIP-VERSION (one short string, no schema)
 *  Set when the user dismisses an offered firmware version via the
 *  "Skip this version" button. Auto-OTA-check consults this on every
 *  boot before showing the popup; a match silences the dialog.
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_OTA "ota_cfg"

esp_err_t config_store_save_ota_skip_version(const char *version)
{
    if (!version) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_OTA, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "skip_ver", version);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) ESP_LOGI(TAG, "OTA skip-version saved: %s", version);
    return err;
}

esp_err_t config_store_load_ota_skip_version(char *out, size_t out_len)
{
    if (!out || out_len == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open(NS_OTA, NVS_READONLY, &handle) != ESP_OK) return ESP_ERR_NOT_FOUND;
    size_t n = out_len;
    esp_err_t err = nvs_get_str(handle, "skip_ver", out, &n);
    nvs_close(handle);
    if (err != ESP_OK) out[0] = '\0';
    return err;
}

/* ── Vehicle odometer ──────────────────────────────────────────────────── */

#define NS_VEHICLE "vehicle"

esp_err_t config_store_save_odometer_km(float km)
{
    if (!isfinite(km) || km < 0.0f) km = 0.0f;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_VEHICLE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "odo_km", &km, sizeof(km));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t config_store_load_odometer_km(float *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = 0.0f;
    nvs_handle_t handle;
    if (nvs_open(NS_VEHICLE, NVS_READONLY, &handle) != ESP_OK) return ESP_ERR_NOT_FOUND;
    float km = 0.0f;
    size_t sz = sizeof(km);
    esp_err_t err = nvs_get_blob(handle, "odo_km", &km, &sz);
    nvs_close(handle);
    if (err == ESP_OK && sz == sizeof(km) && isfinite(km) && km >= 0.0f) {
        *out = km;
    }
    return err;
}

/* ── Fuel-over-CAN forward ─────────────────────────────────────────────── */

#define NS_FUELFWD "fuelfwd"

esp_err_t config_store_save_fuel_forward(const fuel_forward_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NS_FUELFWD, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, "cfg", cfg, sizeof(*cfg));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "fuel forward saved: %s id=0x%lX rate=%uHz",
                 cfg->enabled ? "ON" : "off",
                 (unsigned long)cfg->can_id, (unsigned)cfg->rate_hz);
    }
    return err;
}

esp_err_t config_store_load_fuel_forward(fuel_forward_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    /* Defaults for a never-configured device: disabled, sane framing. */
    out->can_id = 0x6F0;
    out->endian = 1;      /* Intel LE */
    out->bit_length = 16;
    out->rate_hz = 10;
    out->scale = 1.0f;
    nvs_handle_t handle;
    if (nvs_open(NS_FUELFWD, NVS_READONLY, &handle) != ESP_OK)
        return ESP_ERR_NOT_FOUND;
    fuel_forward_config_t cfg;
    size_t sz = sizeof(cfg);
    esp_err_t err = nvs_get_blob(handle, "cfg", &cfg, &sz);
    nvs_close(handle);
    if (err == ESP_OK && sz == sizeof(cfg)) *out = cfg;
    return err;
}

/* ── ECU preset-picker Auto-vs-Manual mode ─────────────────────────────── */

#define NS_ECU_PICKER "ecu_pick"

esp_err_t config_store_save_ecu_picker_auto(bool auto_mode)
{
    esp_err_t err = chs_save_u8(NS_ECU_PICKER, "auto", auto_mode ? 1u : 0u);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ECU picker mode saved: %s", auto_mode ? "Auto" : "Manual");
    }
    return err;
}

bool config_store_load_ecu_picker_auto(void)
{
    /* Default to Auto on any read failure — the safer choice. Manual hides
     * presets, and a missing/corrupt NVS value should never cause a UI to
     * silently drop options the user might need. chs_load_u8 leaves *out
     * untouched on failure (open or get), so v stays at its 1 default. */
    uint8_t v = 1;
    chs_load_u8(NS_ECU_PICKER, "auto", &v);
    return v != 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  WIDGET LATCH STATE (button / toggle "remember state")
 * ═══════════════════════════════════════════════════════════════════════ */
#define NS_WDGLATCH "wdglatch"

/* Build the NVS key for a given output. can_id is masked to the 11-bit TX
 * range (can_transmit_frame only emits standard frames) and the bit to 0-63,
 * so the key is at most "7ff_63" (6 chars) — well under the 15-char limit. */
static void _widget_latch_key(uint32_t can_id, uint8_t bit, char *out, size_t cap)
{
    snprintf(out, cap, "%03lx_%u",
             (unsigned long)(can_id & 0x7FFu), (unsigned)(bit & 0x3Fu));
}

esp_err_t config_store_save_widget_latch(uint32_t tx_can_id, uint8_t tx_bit, bool on)
{
    if (tx_can_id == 0) return ESP_ERR_INVALID_ARG;
    char key[16];
    _widget_latch_key(tx_can_id, tx_bit, key, sizeof key);
    return chs_save_u8(NS_WDGLATCH, key, on ? 1 : 0);
}

esp_err_t config_store_load_widget_latch(uint32_t tx_can_id, uint8_t tx_bit, bool *on)
{
    if (!on) return ESP_ERR_INVALID_ARG;
    *on = false;
    if (tx_can_id == 0) return ESP_ERR_NOT_FOUND;
    char key[16];
    _widget_latch_key(tx_can_id, tx_bit, key, sizeof key);

    nvs_handle_t handle;
    if (nvs_open(NS_WDGLATCH, NVS_READONLY, &handle) != ESP_OK) return ESP_ERR_NOT_FOUND;
    uint8_t u8;
    esp_err_t err = nvs_get_u8(handle, key, &u8);
    if (err == ESP_OK) *on = (u8 != 0);
    nvs_close(handle);
    return err;
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
