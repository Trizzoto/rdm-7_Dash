#ifndef DEVICE_ID_H
#define DEVICE_ID_H

#include <esp_err.h>
#include <stdbool.h>

// Maximum length for serial number string (RDM-XXXX-XXXX format)
#define MAX_SERIAL_LENGTH 13

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

#endif // DEVICE_ID_H 