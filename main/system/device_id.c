#include "device_id.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <string.h>
#include <stdio.h>
#include <mbedtls/sha256.h>

static const char *TAG = "device_id";

esp_err_t init_device_id(void) {
    // Nothing to initialize since we read MAC directly
    ESP_LOGI(TAG, "Device ID system initialized");
    return ESP_OK;
}

esp_err_t get_device_serial(char *serial) {
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(err));
        return err;
    }

    // Format: RDM-XXXX-XXXX where XXXX is derived from MAC address
    snprintf(serial, MAX_SERIAL_LENGTH, "RDM-%02X%02X-%02X%02X",
             mac[0], mac[1], mac[2], mac[3]);

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