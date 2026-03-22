#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <stdint.h>
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"

// Status codes for OTA
typedef enum {
    OTA_IDLE,
    OTA_CHECKING,                // When check is in progress
    OTA_NO_UPDATE_AVAILABLE,     
    OTA_UPDATE_AVAILABLE,
    OTA_UPDATE_IN_PROGRESS,
    OTA_UPDATE_COMPLETED,
    OTA_UPDATE_FAILED
} ota_status_t;

// Function declarations
void init_ota(void);
void check_for_update(void);
esp_err_t start_ota_update(void);
void start_ota_update_task(void);
ota_status_t get_ota_status(void);
const char* get_latest_version(void);
int get_ota_progress(void);
float get_update_file_size_mb(void);
const char* get_release_notes(void);
void initialize_sntp(void);
void debug_ota_connectivity(void);

/* Set a custom firmware download URL (plain HTTP recommended).
 * If not set, defaults to OTA_DEFAULT_BASE_URL/<version>/esp32-firmware.bin */
void ota_set_firmware_url(const char *url);
const char *ota_get_firmware_url(void);

#endif