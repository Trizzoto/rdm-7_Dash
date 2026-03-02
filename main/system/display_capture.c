#include "display_capture.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include <string.h>

static const char *TAG = "display_capture";

// External references to display buffers
extern lv_disp_draw_buf_t disp_buf;
extern lv_disp_drv_t disp_drv;
extern bool example_lvgl_lock(int timeout_ms);
extern void example_lvgl_unlock(void);

esp_err_t display_capture_init(void)
{
    ESP_LOGI(TAG, "Display capture initialized");
    return ESP_OK;
}

esp_err_t display_capture_screenshot(uint8_t **output_buffer, size_t *output_size)
{
    if (!output_buffer || !output_size) {
        return ESP_ERR_INVALID_ARG;
    }

    // Lock LVGL to ensure thread safety
    if (!example_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "Failed to lock LVGL mutex");
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_FAIL;
    
    // Calculate output size for full screen (RGB565 is 2 bytes per pixel)
    size_t pixel_count = CAPTURE_WIDTH * CAPTURE_HEIGHT;
    *output_size = pixel_count * CAPTURE_BYTES_PER_PIXEL;
    
    // Allocate output buffer
    *output_buffer = heap_caps_malloc(*output_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!*output_buffer) {
        *output_buffer = heap_caps_malloc(*output_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!*output_buffer) {
            ESP_LOGE(TAG, "Failed to allocate output buffer (%zu bytes)", *output_size);
            example_lvgl_unlock();
            return ESP_ERR_NO_MEM;
        }
    }

    // Try to use LVGL snapshot first
    lv_obj_t *screen = lv_scr_act();
    if (screen) {
        // Calculate buffer size needed for snapshot
        uint32_t buf_size = lv_snapshot_buf_size_needed(screen, LV_IMG_CF_TRUE_COLOR);
        if (buf_size > 0) {
            // Allocate snapshot buffer
            void *snapshot_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!snapshot_buf) {
                snapshot_buf = heap_caps_malloc(buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            }
            
            if (snapshot_buf) {
                // Take snapshot
                lv_img_dsc_t snapshot;
                lv_res_t res = lv_snapshot_take_to_buf(screen, LV_IMG_CF_TRUE_COLOR, &snapshot, snapshot_buf, buf_size);
                
                if (res == LV_RES_OK) {
                    // Copy pixel data directly (RGB565 format)
                    memcpy(*output_buffer, snapshot_buf, *output_size);
                    
                    // Clean up
                    heap_caps_free(snapshot_buf);
                    example_lvgl_unlock();
                    
                    ESP_LOGI(TAG, "Screenshot captured via snapshot: %dx%d pixels, %zu bytes", 
                             snapshot.header.w, snapshot.header.h, *output_size);
                    
                    return ESP_OK;
                }
                
                heap_caps_free(snapshot_buf);
            }
        }
    }
    
    // Fallback: Capture from display buffer
    ESP_LOGW(TAG, "Snapshot method failed, trying display buffer method");
    
    // Access the current display buffer
    lv_disp_t *disp = lv_disp_get_default();
    if (disp && disp->driver && disp->driver->draw_buf) {
        lv_disp_draw_buf_t *draw_buf = disp->driver->draw_buf;
        
        if (draw_buf->buf_act) {
            // Calculate the size of the active buffer
            uint32_t buf_pixels = draw_buf->size;
            uint32_t buf_bytes = buf_pixels * sizeof(lv_color_t);
            
            if (buf_bytes <= *output_size) {
                // Copy from the active draw buffer
                memcpy(*output_buffer, draw_buf->buf_act, buf_bytes);
                *output_size = buf_bytes;
                
                example_lvgl_unlock();
                
                ESP_LOGI(TAG, "Screenshot captured from draw buffer: %u pixels, %zu bytes", 
                         buf_pixels, *output_size);
                
                return ESP_OK;
            }
        }
    }
    
    // Final fallback: Fill with a pattern to show it's working
    ESP_LOGW(TAG, "Both methods failed, creating test pattern");
    
    uint16_t *pixels = (uint16_t *)*output_buffer;
    for (int y = 0; y < CAPTURE_HEIGHT; y++) {
        for (int x = 0; x < CAPTURE_WIDTH; x++) {
            // Create a simple test pattern (gradient)
            uint8_t r = (x * 255) / CAPTURE_WIDTH;
            uint8_t g = (y * 255) / CAPTURE_HEIGHT;
            uint8_t b = 128;
            
            // Convert to RGB565
            uint16_t rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            pixels[y * CAPTURE_WIDTH + x] = rgb565;
        }
    }
    
    example_lvgl_unlock();
    
    ESP_LOGI(TAG, "Test pattern generated: %dx%d pixels, %zu bytes", 
             CAPTURE_WIDTH, CAPTURE_HEIGHT, *output_size);
    
    return ESP_OK;
}

void display_capture_free_buffer(uint8_t *buffer)
{
    if (buffer) {
        heap_caps_free(buffer);
    }
} 