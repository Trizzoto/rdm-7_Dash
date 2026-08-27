#include "device_id.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <string.h>
#include <stdio.h>
#include <mbedtls/sha256.h>

static const char *TAG = "device_id";

/* Defined below; used by init_device_id to log the serial at boot. */
esp_err_t get_device_serial(char *serial);

esp_err_t init_device_id(void) {
    /* Nothing to initialise — the serial is derived from the MAC on demand.
     * It IS worth printing once, though: it is the dash's identity, it is the
     * first thing support asks for, and until now the only ways to read it
     * were the settings screen or an HTTP call — neither available on a dash
     * that has not joined a network yet. */
    char serial[MAX_SERIAL_LENGTH] = {0};
    if (get_device_serial(serial) == ESP_OK)
        ESP_LOGI(TAG, "Device serial: %s", serial);
    else
        ESP_LOGW(TAG, "Device serial unavailable (MAC read failed)");
    return ESP_OK;
}

esp_err_t get_device_serial(char *serial) {
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(err));
        return err;
    }

    /* Format: RDM-XXXX-XXXX from the LAST four MAC bytes.
     *
     * It used to take mac[0..3], which looked reasonable and was nearly
     * useless: mac[0..2] is Espressif's OUI and is byte-identical on every
     * chip in the block, so the whole serial turned on mac[3] alone — 256
     * values, and boards from one batch collide. Measured 2026-08-27: a
     * brand-new board and the bench dash both reported RDM-DCB4-D926.
     *
     * That matters most for the X-RDM-Device check in web_server_ota.c,
     * which exists to stop an update landing on the wrong dash and cannot do
     * that if two dashes answer to the same name.
     *
     * mac[3..5] vary per chip and mac[2] varies across OUIs, so this keeps
     * the same shape and length while carrying real uniqueness. Note this
     * RENAMES every dash already in the field — nothing stores the serial
     * (every caller derives it fresh) and the AP SSID uses mac[4..5]
     * independently, so nothing breaks, but a printed label will disagree
     * with the screen. */
    snprintf(serial, MAX_SERIAL_LENGTH, "RDM-%02X%02X-%02X%02X",
             mac[2], mac[3], mac[4], mac[5]);

    return ESP_OK;
}

esp_err_t get_device_ap_password(char *buf, size_t cap) {
    if (!buf || cap < DEVICE_AP_PASSWORD_LEN) return ESP_ERR_INVALID_ARG;

    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read SoftAP MAC: %s", esp_err_to_name(err));
        return err;
    }

    /* Salted hash, NOT the raw MAC: the AP beacon broadcasts the MAC, so a
     * trivial mac→password mapping would be readable off the air. The salt
     * is public in this firmware too — see device_id.h: the goal is a
     * per-unit secret good against the docs/marketing-page reader, not a
     * determined attacker with the firmware image. */
    static const char salt[] = "rdm7-ap-pass-v1";
    uint8_t seed[sizeof(mac) + sizeof(salt)];
    memcpy(seed, mac, sizeof(mac));
    memcpy(seed + sizeof(mac), salt, sizeof(salt));

    uint8_t digest[32];
    if (mbedtls_sha256(seed, sizeof(seed), digest, 0) != 0) return ESP_FAIL;

    /* No 0/O, 1/l/I — owners type this from the dash screen. */
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    const size_t alpha = sizeof(alphabet) - 1;
    for (int i = 0; i < DEVICE_AP_PASSWORD_LEN - 1; i++) {
        buf[i] = alphabet[digest[i] % alpha];
    }
    buf[DEVICE_AP_PASSWORD_LEN - 1] = '\0';
    return ESP_OK;
}