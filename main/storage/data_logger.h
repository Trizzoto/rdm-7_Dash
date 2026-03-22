#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* Initialize the data logger (call once at boot) */
void data_logger_init(void);

/* Start logging to a new CSV file on SD card */
esp_err_t data_logger_start(void);

/* Stop logging and close the file */
esp_err_t data_logger_stop(void);

/* Check if currently logging */
bool data_logger_is_active(void);

/* Get current log filename (or empty string if not logging) */
const char *data_logger_current_file(void);

/* Get log statistics */
uint32_t data_logger_get_sample_count(void);
uint32_t data_logger_get_elapsed_ms(void);

#ifdef __cplusplus
}
#endif
