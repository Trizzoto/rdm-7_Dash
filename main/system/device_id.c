#include "device_id.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <string.h>
#include <stdio.h>

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