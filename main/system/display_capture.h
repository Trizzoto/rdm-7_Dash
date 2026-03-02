#ifndef DISPLAY_CAPTURE_H
#define DISPLAY_CAPTURE_H

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Display capture configuration
#define CAPTURE_WIDTH  800
#define CAPTURE_HEIGHT 480
#define CAPTURE_BYTES_PER_PIXEL 2  // RGB565

// Function declarations
esp_err_t display_capture_init(void);
esp_err_t display_capture_screenshot(uint8_t **output_buffer, size_t *output_size);
void display_capture_free_buffer(uint8_t *buffer);

#endif // DISPLAY_CAPTURE_H 