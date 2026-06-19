#ifndef DEVICE_ID_H
#define DEVICE_ID_H

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>

// Maximum length for serial number string (RDM-XXXX-XXXX format)
// "RDM-XXXX-XXXX" = 13 chars + null terminator = 14 bytes minimum; 16 for alignment
#define MAX_SERIAL_LENGTH 16

/**
 * @brief Initialize the device ID system
 * 
 * This function initializes the device ID system. Since the serial number
 * is generated from the MAC address, no storage is needed.
 * 
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t init_device_id(void);

/**
 * @brief Get the device's serial number
 *
 * This function generates the device's serial number from the MAC address.
 * The serial number will always be the same for each device.
 *
 * @param serial Buffer to store the serial number (must be at least MAX_SERIAL_LENGTH bytes)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t get_device_serial(char *serial);

/* 10 chars + NUL */
#define DEVICE_AP_PASSWORD_LEN 11

/**
 * @brief Per-device default WiFi hotspot password.
 *
 * Derived from the SoftAP MAC (SHA-256 + salt) so every unit ships with a
 * UNIQUE WPA2 password instead of a fleet-wide default anyone can read out
 * of the docs or firmware. Stable across reboots and reflashes; shown to
 * the owner on the dash (first-run wizard + WiFi screen). A user-saved
 * password in NVS overrides this default (config_store_load_ap_config).
 *
 * Alphabet avoids ambiguous glyphs (0/O, 1/l/I) since owners type it from
 * the screen. 10 chars over a 31-char alphabet ≈ 49 bits.
 *
 * @param buf Output buffer, at least DEVICE_AP_PASSWORD_LEN bytes.
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t get_device_ap_password(char *buf, size_t cap);

#endif // DEVICE_ID_H