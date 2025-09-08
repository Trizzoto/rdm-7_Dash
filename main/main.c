#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_pm.h"
#include "hal/gpio_types.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lcd_touch_gt911.h"
#include "lvgl.h"
#include "driver/twai.h"
#include "ui_Screen1.h"
#include "driver/i2c.h"
#include "ui/ui.h"
#include "lvgl_helpers.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include <unistd.h>       // For unlink
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>
#include "screens/ui_Screen3.h"
#include "esp32s3/rom/cache.h"
#include "driver/ledc.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "ui/screens/ui_wifi.h"
#include "esp_system.h"
#include "ota_handler.h"
#include "gps/gps.h"
#include "device_id.h"
#include "ui/device_settings.h"
#include "web_server.h"
#include "display_capture.h"
#include "fuel_input.h"

// External declarations

#define EXAMPLE_MAX_CHAR_SIZE    64
#define MOUNT_POINT "/sdcard"
sdmmc_card_t *card;  // Declare globally if not done already

// Define the LVGL mutex
SemaphoreHandle_t lvgl_mux = NULL;

#define LV_USE_ANIMATION 0
#define LV_USE_SHADOW 0
#define LV_USE_BLEND_MODES 0


#define I2C_MASTER_SCL_IO           9       /*!< GPIO number used for I2C master clock */
#define I2C_MASTER_SDA_IO           8       /*!< GPIO number used for I2C master data  */
#define I2C_MASTER_NUM              0       /*!< I2C master i2c port number, the number of i2c peripheral interfaces available will depend on the chip */
#define I2C_MASTER_FREQ_HZ          400000                     /*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE   0                          /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE   0                          /*!< I2C master doesn't need buffer */
#define I2C_MASTER_TIMEOUT_MS       1000

#define GPIO_INPUT_IO_4    4
#define GPIO_INPUT_PIN_SEL  1
static const char *TAG = "main";

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ     (18 * 1000 * 1000)
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL  1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
#define EXAMPLE_PIN_NUM_BK_LIGHT       -1
#define EXAMPLE_PIN_NUM_HSYNC          46
#define EXAMPLE_PIN_NUM_VSYNC          3
#define EXAMPLE_PIN_NUM_DE             5
#define EXAMPLE_PIN_NUM_PCLK           7
#define EXAMPLE_PIN_NUM_DATA0          14 // B3
#define EXAMPLE_PIN_NUM_DATA1          38 // B4
#define EXAMPLE_PIN_NUM_DATA2          18 // B5
#define EXAMPLE_PIN_NUM_DATA3          17 // B6
#define EXAMPLE_PIN_NUM_DATA4          10 // B7
#define EXAMPLE_PIN_NUM_DATA5          39 // G2
#define EXAMPLE_PIN_NUM_DATA6          0 // G3
#define EXAMPLE_PIN_NUM_DATA7          45 // G4
#define EXAMPLE_PIN_NUM_DATA8          48 // G5
#define EXAMPLE_PIN_NUM_DATA9          47 // G6
#define EXAMPLE_PIN_NUM_DATA10         21 // G7
#define EXAMPLE_PIN_NUM_DATA11         1  // R3
#define EXAMPLE_PIN_NUM_DATA12         2  // R4
#define EXAMPLE_PIN_NUM_DATA13         42 // R5
#define EXAMPLE_PIN_NUM_DATA14         41 // R6
#define EXAMPLE_PIN_NUM_DATA15         40 // R7
#define EXAMPLE_PIN_NUM_DISP_EN        -1
#define LCD_CMD_BITS_DEFAULT          8
#define LCD_PARAM_BITS_DEFAULT        8
#define LCD_RGB_PANEL_WRITE_BYTES     NULL  // Use default write bytes function

// The pixel number in horizontal and vertical
#define EXAMPLE_LCD_H_RES              800
#define EXAMPLE_LCD_V_RES              480

#if CONFIG_EXAMPLE_DOUBLE_FB
#define EXAMPLE_LCD_NUM_FB             2
#else
#define EXAMPLE_LCD_NUM_FB             1
#endif // CONFIG_EXAMPLE_DOUBLE_FB

#define EXAMPLE_LVGL_TICK_PERIOD_MS    2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 30
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 20
#define EXAMPLE_LVGL_TASK_STACK_SIZE   (8 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY     8

// we use two semaphores to sync the VSYNC event and the LVGL task, to avoid potential tearing effect
#if CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM
SemaphoreHandle_t sem_vsync_end;
SemaphoreHandle_t sem_gui_ready;
#endif

// Declare the flush callback function
void my_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);

lv_disp_drv_t disp_drv;
lv_disp_draw_buf_t draw_buf;

// Configuration for the CAN bus - make them global
// Initialize with default 500 kbps, will be loaded from NVS during startup
twai_timing_config_t g_t_config = TWAI_TIMING_CONFIG_500KBITS();
twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(20,19, TWAI_MODE_NORMAL);
twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

// Forward declarations for filter building and reconfiguration
#include "ui/screens/ui_Screen3.h"

// Forward references to CAN task symbols defined later/in other units
extern TaskHandle_t canTaskHandle;
extern volatile bool can_task_should_stop;
extern void can_receive_task(void *pvParameter);

static void build_twai_filter_from_configs(twai_filter_config_t *out_filter)
{
    // Collect all relevant standard 11-bit IDs from configs
    uint32_t ids[8 + 2 + 13];
    int count = 0;

    for (int i = 0; i < 8; i++) {
        if (warning_configs[i].can_id != 0) {
            ids[count++] = (warning_configs[i].can_id & 0x7FF);
        }
    }
    for (int i = 0; i < 2; i++) {
        if (indicator_configs[i].can_id != 0) {
            ids[count++] = (indicator_configs[i].can_id & 0x7FF);
        }
    }
    for (int i = 0; i < 13; i++) {
        if (values_config[i].enabled && values_config[i].can_id != 0) {
            ids[count++] = (values_config[i].can_id & 0x7FF);
        }
    }

    // Default to accept all if no IDs are configured
    if (count == 0) {
        *out_filter = (twai_filter_config_t)TWAI_FILTER_CONFIG_ACCEPT_ALL();
        return;
    }

    // Compute common pattern across all IDs to derive a mask-based filter
    uint32_t ref = ids[0] & 0x7FF;
    uint32_t varying = 0;
    for (int i = 1; i < count; i++) {
        varying |= (ref ^ (ids[i] & 0x7FF));
    }
    uint32_t compare_bits = (~varying) & 0x7FF; // bits that are equal across all IDs
    uint32_t code_bits = ref & compare_bits;

    // In TWAI, mask bit 1 = don't care, 0 = compare. Place ID in MSBs (bits 31..21)
    uint32_t acceptance_code = (code_bits << 21);
    uint32_t acceptance_mask = 0xFFFFFFFF; // start with don't care everywhere
    acceptance_mask &= ~(compare_bits << 21); // set 0 where we compare

    out_filter->acceptance_code = acceptance_code;
    out_filter->acceptance_mask = acceptance_mask;
    out_filter->single_filter = true; // single filter mode for standard IDs
}

void reconfigure_can_filter(void)
{
    // Stop CAN task
    if (canTaskHandle != NULL) {
        can_task_should_stop = true;
        for (int i = 0; i < 200; i++) {
            if (eTaskGetState(canTaskHandle) == eDeleted) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (eTaskGetState(canTaskHandle) != eDeleted) {
            vTaskDelete(canTaskHandle);
        }
        canTaskHandle = NULL;
    }

    // Rebuild filter from current configuration
    build_twai_filter_from_configs(&f_config);

    // Restart TWAI driver with new filter
    twai_stop();
    vTaskDelay(pdMS_TO_TICKS(50));
    twai_driver_uninstall();
    vTaskDelay(pdMS_TO_TICKS(50));
    if (twai_driver_install(&g_config, &g_t_config, &f_config) == ESP_OK) {
        twai_start();
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    // Recreate CAN receive task
    xTaskCreatePinnedToCore(can_receive_task, "can_receive_task", 4096, NULL, 7, &canTaskHandle, 0);
}

// PWM configuration for GPIO16
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO          16 // Define the output GPIO
#define LEDC_CHANNEL           LEDC_CHANNEL_0
#define LEDC_DUTY_RES          LEDC_TIMER_13_BIT // Set duty resolution to 13 bits
#define LEDC_FREQUENCY         9000 // Frequency in Hz
#define LEDC_DUTY             (8191) 

static esp_err_t i2c_master_init(void)
{
    int i2c_master_port = I2C_MASTER_NUM;

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };


    i2c_param_config(i2c_master_port, &conf);

    return i2c_driver_install(i2c_master_port, conf.mode, I2C_MASTER_RX_BUF_DISABLE, I2C_MASTER_TX_BUF_DISABLE, 0);
}

static bool example_on_vsync_event(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *event_data, void *user_data)
{
    BaseType_t high_task_awoken = pdFALSE;
#if CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM
    if (xSemaphoreTakeFromISR(sem_gui_ready, &high_task_awoken) == pdTRUE) {
        xSemaphoreGiveFromISR(sem_vsync_end, &high_task_awoken);
    }
#endif
    return high_task_awoken == pdTRUE;
}

#define MOUNT_POINT "/sdcard"
#define SD_MOSI     11
#define SD_CLK      12
#define SD_MISO     13
#define SD_CS       4  // GPIO for CS

void init_sd_card(void) {
    esp_err_t ret;
    sdmmc_card_t *card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI("SD_CARD", "Initializing SD card");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    // Configure the SPI bus
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = 10000;  // Reduced frequency for compatibility

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI,
        .miso_io_num = SD_MISO,
        .sclk_io_num = SD_CLK,
        .max_transfer_sz = 4000,
    };
    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE("SD_CARD", "Failed to initialize bus.");
        return;
    }

    // Configure the SD card slot
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS;
    slot_config.host_id = host.slot;

    // Mount the filesystem
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE("SD_CARD", "Failed to mount filesystem. Error: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI("SD_CARD", "SD card mounted successfully");
    sdmmc_card_print_info(stdout, card);
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
   
    // Direct transfer to display - PSRAM buffers are handled by the LCD driver
    
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
    
    lv_disp_flush_ready(drv);
}

bool example_lvgl_lock(int timeout_ms)
{
    // Convert timeout in milliseconds to FreeRTOS ticks
    // If timeout_ms is set to -1, the program will block until the condition is met
    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks) == pdTRUE;
}

void example_lvgl_unlock(void)
{
    xSemaphoreGiveRecursive(lvgl_mux);
}

// At the top with other declarations
TaskHandle_t lvglTaskHandle = NULL;
TaskHandle_t canTaskHandle = NULL;

// Add a flag to signal CAN task to stop gracefully
volatile bool can_task_should_stop = false;

// Change the LVGL task function to be accessible
void example_lvgl_port_task(void *pvParameter) {
    ESP_LOGI(TAG, "Starting LVGL task");
    const uint32_t refresh_period_ms = 25;  // Align with LVGL refresh period for consistent timing
    uint32_t last_error_time = 0;
    const uint32_t error_report_interval = 5000;  // 5 seconds between error logs
    uint32_t consecutive_failures = 0;
    const uint32_t max_failures = 10;  // Maximum consecutive failures before task notification
    uint32_t start_time;

    while (1) {
        start_time = esp_timer_get_time() / 1000;
        
        // Lock the mutex with shorter timeout for better responsiveness during OTA
        if (example_lvgl_lock(pdMS_TO_TICKS(100))) {
            // Reset failure counter on successful lock
            consecutive_failures = 0;
            
            // Handle any pending LVGL tasks
            lv_timer_handler();
            example_lvgl_unlock();
            
            // Calculate remaining time in the refresh period
            uint32_t elapsed = (esp_timer_get_time() / 1000) - start_time;
            uint32_t delay_ms = (elapsed >= refresh_period_ms) ? 1 : (refresh_period_ms - elapsed);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        } else {
            consecutive_failures++;
            
            uint32_t current_time = esp_timer_get_time() / 1000;
            if (current_time - last_error_time > error_report_interval) {
                ESP_LOGW(TAG, "Failed to acquire LVGL mutex (failures: %lu)", consecutive_failures);
                last_error_time = current_time;
            }

            // If we've failed too many times, notify the system
            if (consecutive_failures >= max_failures) {
                ESP_LOGE(TAG, "LVGL task experiencing persistent mutex acquisition failures");
                consecutive_failures = 0;  // Reset counter
            }
            
            // Shorter delay for faster retry during high contention
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
}

void gpio_init(void)
{
    //zero-initialize the config structure.
    gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //bit mask of the pins, use GPIO6 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
    //enable pull-up mode
     io_conf.pull_up_en = 1;
    gpio_config(&io_conf);
}

// extern lv_obj_t *scr;
static void example_lvgl_touch_cb(lv_indev_drv_t * drv, lv_indev_data_t * data)
{
    // Always set to released state first
    data->state = LV_INDEV_STATE_REL;
    
    // If no touch controller is available, just return with released state
    if (drv->user_data == NULL) {
        return;
    }
    
    uint16_t touchpad_x[1] = {0};
    uint16_t touchpad_y[1] = {0};
    uint8_t touchpad_cnt = 0;

    /* Read touch controller data */
    esp_lcd_touch_read_data(drv->user_data);

    /* Get coordinates */
    bool touchpad_pressed = esp_lcd_touch_get_coordinates(drv->user_data, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);

    if (touchpad_pressed && touchpad_cnt > 0) {
        data->point.x = touchpad_x[0];
        data->point.y = touchpad_y[0];
        data->state = LV_INDEV_STATE_PR;
    }
}

void can_init() {
    // Load saved bitrate setting from NVS
    nvs_handle_t handle;
    uint8_t saved_bitrate = 2; // Default to 500 kbps (index 2)
    if (nvs_open("can_config", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_get_u8(handle, "can_bitrate", &saved_bitrate);
        nvs_close(handle);
    }
    
    // Configure timing based on saved bitrate
    switch(saved_bitrate) {
        case 0: // 125 kbps
            g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
            ESP_LOGI(TAG, "CAN bitrate set to 125 kbps");
            break;
        case 1: // 250 kbps
            g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
            ESP_LOGI(TAG, "CAN bitrate set to 250 kbps");
            break;
        case 2: // 500 kbps
            g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
            ESP_LOGI(TAG, "CAN bitrate set to 500 kbps");
            break;
        case 3: // 1 Mbps
            g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
            ESP_LOGI(TAG, "CAN bitrate set to 1 Mbps");
            break;
        default: // Default to 500 kbps
            g_t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
            ESP_LOGI(TAG, "CAN bitrate set to default 500 kbps");
            break;
    }

    // Build dispatch and filter based on current configuration
    rebuild_can_dispatch();
    build_twai_filter_from_configs(&f_config);

    // Install the CAN driver
    if (twai_driver_install(&g_config, &g_t_config, &f_config) == ESP_OK) {
        ESP_LOGI(TAG, "CAN driver installed successfully");
    } else {
        ESP_LOGE(TAG, "Failed to install CAN driver");
    }

    // Start the CAN driver
    if (twai_start() == ESP_OK) {
        ESP_LOGI(TAG, "CAN driver started");
    } else {
        ESP_LOGE(TAG, "Failed to start CAN driver");
    }
}

void can_receive_task(void *pvParameter) {
    const TickType_t xDelay = pdMS_TO_TICKS(1); // Small delay to prevent tight loop
    uint32_t last_warning_time = 0;
    const uint32_t warning_interval = 30000; // Increased to 30 seconds to reduce spam
    uint32_t consecutive_failures = 0;
    const uint32_t failure_threshold = 50; // Higher threshold before warning
    
    while (1) {
        // Check if we should stop the task
        if (can_task_should_stop) {
            ESP_LOGI(TAG, "CAN task stopping gracefully");
            break;
        }
        
        twai_message_t message;
        esp_err_t ret = twai_receive(&message, pdMS_TO_TICKS(5)); // 5ms timeout
        
        if (ret == ESP_OK) {
            // Check if this is a priority message (wideband or BAR2)
            bool is_priority = false;
            for (int i = 0; i < 13; i++) {
                // Check for panel 7 (wideband) or panel 13 (BAR2)
                if ((i == 6 || i == 12) && values_config[i].enabled && values_config[i].can_id == message.identifier) {
                    is_priority = true;
                    break;
                }
            }

            bool mutex_acquired = false;
            int retry_count = 0;
            const int max_retries = is_priority ? 3 : 1; // More retries for priority messages

            // Try multiple times for priority messages
            while (!mutex_acquired && retry_count < max_retries) {
                if (example_lvgl_lock(pdMS_TO_TICKS(is_priority ? 2 : 5))) {
                    mutex_acquired = true;
                    consecutive_failures = 0;
                    process_can_message(&message);
                    example_lvgl_unlock();
                    break;
                }
                retry_count++;
                // Small delay between retries
                if (!mutex_acquired && retry_count < max_retries) {
                    vTaskDelay(pdMS_TO_TICKS(0));
                }
            }

            if (!mutex_acquired) {
                consecutive_failures++;
                
                // Only log warning if we've had many consecutive failures
                if (consecutive_failures >= failure_threshold) {
                    uint32_t current_time = esp_timer_get_time() / 1000;
                    if (current_time - last_warning_time > warning_interval) {
                        ESP_LOGW(TAG, "LVGL mutex contention (%lu consecutive failures)", consecutive_failures);
                        last_warning_time = current_time;
                        consecutive_failures = 0; // Reset after warning
                    }
                }
                
                // Dynamic delay based on contention
                if (consecutive_failures > 100) {
                    vTaskDelay(pdMS_TO_TICKS(3));
                } else if (consecutive_failures > 50) {
                    vTaskDelay(pdMS_TO_TICKS(2));
                } else if (!is_priority) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
        } else if (ret == ESP_ERR_TIMEOUT) {
            // No CAN messages received, yield to other tasks
            vTaskDelay(xDelay);
            consecutive_failures = 0; // Reset on timeout as it's a natural pause
        } else if (ret == ESP_ERR_INVALID_STATE) {
            // CAN bus is likely disconnected, try to recover
            ESP_LOGW(TAG, "CAN bus error, attempting recovery...");
            twai_stop();
            vTaskDelay(pdMS_TO_TICKS(100)); // Wait 100ms
            twai_start();
            vTaskDelay(pdMS_TO_TICKS(100)); // Wait another 100ms
            consecutive_failures = 0; // Reset after recovery attempt
        } else {
            ESP_LOGW(TAG, "CAN receive error: %s", esp_err_to_name(ret));
            vTaskDelay(xDelay);
        }
    }
    
    ESP_LOGI(TAG, "CAN task exited gracefully");
    // Reset the flag when task exits
    can_task_should_stop = false;
    vTaskDelete(NULL);
}

void test_sd_card_write() {
    const char *file_path = MOUNT_POINT"/no.txt";
    FILE *file = fopen(file_path, "w");  // Open file for writing
    if (file == NULL) {
        ESP_LOGE("SD_CARD", "Failed to open file for writing");
        return;
    }
    
    // Write "Hello, World!" to the file
    fprintf(file, "you suck balls\n");
    fclose(file);  // Close the file
    ESP_LOGI("SD_CARD", "File written successfully: %s", file_path);
}

extern warning_config_t warning_configs[8];
extern indicator_config_t indicator_configs[2];  // Left and Right indicators
extern value_config_t values_config[13];  // from your ui code
extern void save_indicator_configs_to_nvs();
extern void load_indicator_configs_from_nvs();
extern char label_texts[13][64];
extern char value_offset_texts[13][64];
extern int rpm_gauge_max;
extern int rpm_redline_value;
#define RPM_VALUE_ID 9
#define SPEED_VALUE_ID 10
#define GEAR_VALUE_ID 11
#define BAR1_VALUE_ID 12
#define BAR2_VALUE_ID 13

void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    // Initialize device ID system
    err = init_device_id();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize device ID system: %s", esp_err_to_name(err));
    }
}

void load_values_config_from_nvs(void) {
    nvs_handle_t handle;
    if (nvs_open("can_config", NVS_READWRITE, &handle) == ESP_OK) {
        for (int i = 0; i < 13; i++) {
            char key[32];
            uint8_t temp_u8;
            float temp_f;
            uint32_t temp_u32;

            // enabled
            snprintf(key, sizeof(key), "enabled%d", i);
            if (nvs_get_u8(handle, key, &temp_u8) == ESP_OK) {
                values_config[i].enabled = (bool)temp_u8;
            }

            // can_id
            snprintf(key, sizeof(key), "can_id%d", i);
            if (nvs_get_u32(handle, key, &temp_u32) == ESP_OK) {
                values_config[i].can_id = temp_u32;
            }

            // endianess
            snprintf(key, sizeof(key), "endian%d", i);
            if (nvs_get_u8(handle, key, &temp_u8) == ESP_OK) {
                values_config[i].endianess = temp_u8;
            }

            // bit_start
            snprintf(key, sizeof(key), "bit_st%d", i);
            if (nvs_get_u8(handle, key, &temp_u8) == ESP_OK) {
                values_config[i].bit_start = temp_u8;
            }

            // bit_length
            snprintf(key, sizeof(key), "bit_len%d", i);
            if (nvs_get_u8(handle, key, &temp_u8) == ESP_OK) {
                values_config[i].bit_length = temp_u8;
            }

            // decimals
            snprintf(key, sizeof(key), "decimals%d", i);
            if (nvs_get_u8(handle, key, &temp_u8) == ESP_OK) {
                values_config[i].decimals = temp_u8;
            }

            // value_offset
            size_t len_f = sizeof(temp_f);
            snprintf(key, sizeof(key), "val_off%d", i);
            if (nvs_get_blob(handle, key, &temp_f, &len_f) == ESP_OK) {
                values_config[i].value_offset = temp_f;
            }

            // scale
            len_f = sizeof(temp_f);
            snprintf(key, sizeof(key), "scale%d", i);
            if (nvs_get_blob(handle, key, &temp_f, &len_f) == ESP_OK) {
                values_config[i].scale = temp_f;
            }

            // is_signed
            snprintf(key, sizeof(key), "is_signed%d", i);
            uint8_t signed_val;
            if (nvs_get_u8(handle, key, &signed_val) == ESP_OK) {
                values_config[i].is_signed = (signed_val == 1);
            }

            // Load RPM bar color for RPM value
            if (i == RPM_VALUE_ID - 1) {
                uint32_t color_value;
                snprintf(key, sizeof(key), "rpm_color%d", i);
                if (nvs_get_u32(handle, key, &color_value) == ESP_OK) {
                    values_config[i].rpm_bar_color.full = color_value;
                }
                
                // Load RPM limiter effect
                snprintf(key, sizeof(key), "rpm_limit_eff%d", i);
                uint8_t limiter_effect;
                if (nvs_get_u8(handle, key, &limiter_effect) == ESP_OK) {
                    values_config[i].rpm_limiter_effect = limiter_effect;
                } else {
                    // Default to None if not found in NVS
                    values_config[i].rpm_limiter_effect = 0;
                }
                
                // Load RPM limiter value
                snprintf(key, sizeof(key), "rpm_limit_val%d", i);
                int32_t limiter_value;
                if (nvs_get_i32(handle, key, &limiter_value) == ESP_OK) {
                    values_config[i].rpm_limiter_value = limiter_value;
                } else {
                    // Default to 7000 RPM if not found in NVS
                    values_config[i].rpm_limiter_value = 7000;
                }
                
                // Load RPM limiter color
                snprintf(key, sizeof(key), "rpm_limit_col%d", i);
                uint32_t limiter_color;
                if (nvs_get_u32(handle, key, &limiter_color) == ESP_OK) {
                    values_config[i].rpm_limiter_color.full = limiter_color;
                } else {
                    // Default to red if not found in NVS
                    values_config[i].rpm_limiter_color = lv_color_hex(0xFF0000);
                }
                
                // Load RPM lights enabled
                snprintf(key, sizeof(key), "rpm_lights_en%d", i);
                uint8_t rpm_lights_enabled;
                if (nvs_get_u8(handle, key, &rpm_lights_enabled) == ESP_OK) {
                    values_config[i].rpm_lights_enabled = (rpm_lights_enabled == 1);
                } else {
                    // Default to disabled if not found in NVS
                    values_config[i].rpm_lights_enabled = false;
                }
                
                // Load RPM gradient enabled
                snprintf(key, sizeof(key), "rpm_grad_en%d", i);
                uint8_t rpm_gradient_enabled;
                if (nvs_get_u8(handle, key, &rpm_gradient_enabled) == ESP_OK) {
                    values_config[i].rpm_gradient_enabled = (rpm_gradient_enabled == 1);
                } else {
                    // Default to disabled if not found in NVS
                    values_config[i].rpm_gradient_enabled = false;
                }
            }

            // Load warning settings for panels (indices 0-7)
            if (i < 8) {
                float temp_warn_f;
                uint32_t temp_color;
                uint8_t warn_flags;
                size_t len_warn = sizeof(float);

                // Warning High Threshold
                snprintf(key, sizeof(key), "warn_hi_th%d", i);
                if (nvs_get_blob(handle, key, &temp_warn_f, &len_warn) == ESP_OK) {
                    values_config[i].warning_high_threshold = temp_warn_f;
                }

                // Warning Low Threshold
                snprintf(key, sizeof(key), "warn_lo_th%d", i);
                if (nvs_get_blob(handle, key, &temp_warn_f, &len_warn) == ESP_OK) {
                    values_config[i].warning_low_threshold = temp_warn_f;
                }

                // Warning High Color
                snprintf(key, sizeof(key), "warn_hi_col%d", i);
                if (nvs_get_u32(handle, key, &temp_color) == ESP_OK) {
                    values_config[i].warning_high_color.full = temp_color;
                }

                // Warning Low Color
                snprintf(key, sizeof(key), "warn_lo_col%d", i);
                if (nvs_get_u32(handle, key, &temp_color) == ESP_OK) {
                    values_config[i].warning_low_color.full = temp_color;
                }

                // Warning Enabled Flags
                snprintf(key, sizeof(key), "warn_enabled%d", i);
                if (nvs_get_u8(handle, key, &warn_flags) == ESP_OK) {
                    values_config[i].warning_high_enabled = (warn_flags & 0x02) != 0;
                    values_config[i].warning_low_enabled = (warn_flags & 0x01) != 0;
                }
            }

            // Load bar min/max for bar values
            if (i == BAR1_VALUE_ID - 1 || i == BAR2_VALUE_ID - 1) {
                int32_t temp_val;
                
                snprintf(key, sizeof(key), "bar_min%d", i);
                if (nvs_get_i32(handle, key, &temp_val) == ESP_OK) {
                    values_config[i].bar_min = temp_val;
                }
                
                snprintf(key, sizeof(key), "bar_max%d", i);
                if (nvs_get_i32(handle, key, &temp_val) == ESP_OK) {
                    values_config[i].bar_max = temp_val;
                }
                
                // Load bar_low value; if not found, default to 25
                snprintf(key, sizeof(key), "bar_low%d", i);
                if (nvs_get_i32(handle, key, &temp_val) == ESP_OK) {
                    values_config[i].bar_low = temp_val;
                } else {
                    values_config[i].bar_low = 25;
                }
                
                // Load bar_high value; if not found, default to 75
                snprintf(key, sizeof(key), "bar_high%d", i);
                if (nvs_get_i32(handle, key, &temp_val) == ESP_OK) {
                    values_config[i].bar_high = temp_val;
                } else {
                    values_config[i].bar_high = 75;
                }
                
                // Load fuel input configuration
                snprintf(key, sizeof(key), "fuel_en%d", i);
                uint8_t fuel_enabled;
                if (nvs_get_u8(handle, key, &fuel_enabled) == ESP_OK) {
                    values_config[i].use_fuel_input = (fuel_enabled == 1);
                } else {
                    values_config[i].use_fuel_input = false; // Default to disabled
                }
                
                snprintf(key, sizeof(key), "fuel_empty%d", i);
                float fuel_voltage;
                size_t len_fuel = sizeof(fuel_voltage);
                if (nvs_get_blob(handle, key, &fuel_voltage, &len_fuel) == ESP_OK) {
                    values_config[i].fuel_empty_voltage = fuel_voltage;
                } else {
                    values_config[i].fuel_empty_voltage = 0.5f; // Default empty voltage
                }
                
                snprintf(key, sizeof(key), "fuel_full%d", i);
                len_fuel = sizeof(fuel_voltage);
                if (nvs_get_blob(handle, key, &fuel_voltage, &len_fuel) == ESP_OK) {
                    values_config[i].fuel_full_voltage = fuel_voltage;
                } else {
                    values_config[i].fuel_full_voltage = 3.0f; // Default full voltage
                }
                
                // Load bar colors
                uint32_t color_value;
                snprintf(key, sizeof(key), "blc%d", i);
                if (nvs_get_u32(handle, key, &color_value) == ESP_OK) {
                    values_config[i].bar_low_color.full = color_value;
                }
                
                snprintf(key, sizeof(key), "bhc%d", i);
                if (nvs_get_u32(handle, key, &color_value) == ESP_OK) {
                    values_config[i].bar_high_color.full = color_value;
                }
                
                snprintf(key, sizeof(key), "birc%d", i);
                if (nvs_get_u32(handle, key, &color_value) == ESP_OK) {
                    values_config[i].bar_in_range_color.full = color_value;
                }
            }
            
            // Load GPS speed source setting for speed value (SPEED_VALUE_ID = 10, index 9)
            if (i == SPEED_VALUE_ID - 1) {
                snprintf(key, sizeof(key), "use_gps%d", i);
                uint8_t use_gps;
                if (nvs_get_u8(handle, key, &use_gps) == ESP_OK) {
                    values_config[i].use_gps_for_speed = (use_gps == 1);
                } else {
                    // Default to CAN ID if not found in NVS
                    values_config[i].use_gps_for_speed = false;
                }
                
                // Load speed units setting (MPH/KMH)
                snprintf(key, sizeof(key), "use_mph%d", i);
                uint8_t use_mph;
                if (nvs_get_u8(handle, key, &use_mph) == ESP_OK) {
                    values_config[i].use_mph = (use_mph == 1);
                } else {
                    // Default to KMH if not found in NVS
                    values_config[i].use_mph = false;
                }
            }
            
            // Load gear detection mode for gear value (GEAR_VALUE_ID = 11, index 10)
            if (i == GEAR_VALUE_ID - 1) {
                snprintf(key, sizeof(key), "gear_mode%d", i);
                uint8_t gear_mode;
                if (nvs_get_u8(handle, key, &gear_mode) == ESP_OK) {
                    values_config[i].gear_detection_mode = gear_mode;
                } else {
                    // Default to MaxxECU if not found in NVS
                    values_config[i].gear_detection_mode = 1;
                }
                
                // Load custom gear CAN IDs
                for (int j = 0; j < 12; j++) {
                    snprintf(key, sizeof(key), "gear_can%d_%d", i, j);
                    uint32_t gear_can_id;
                    if (nvs_get_u32(handle, key, &gear_can_id) == ESP_OK) {
                        values_config[i].gear_custom_can_ids[j] = gear_can_id;
                    } else {
                        // Default to 0 (disabled) if not found in NVS
                        values_config[i].gear_custom_can_ids[j] = 0;
                    }
                }
            }
        }

        // Load labels
        for (int i = 0; i < 13; i++) {
            char key[32];
            size_t required_size = sizeof(label_texts[i]);
            snprintf(key, sizeof(key), "label%d", i);
            nvs_get_str(handle, key, label_texts[i], &required_size);
        }

        // Load RPM gauge max
        int32_t temp_rpm;
        if (nvs_get_i32(handle, "rpm_max", &temp_rpm) == ESP_OK) {
            rpm_gauge_max = temp_rpm;
        }

        // Load RPM redline value
        extern int rpm_redline_value;
        if (nvs_get_i32(handle, "rpm_redline", &temp_rpm) == ESP_OK) {
            rpm_redline_value = temp_rpm;
        }

        nvs_close(handle);
    }
}

void save_values_config_to_nvs(void) {
    ESP_LOGI(TAG, "Starting NVS save operation");
    
    // Take LVGL mutex with shorter timeout to prevent deadlocks
    ESP_LOGI(TAG, "Attempting to acquire LVGL mutex");
    if (!example_lvgl_lock(pdMS_TO_TICKS(50))) {
        ESP_LOGE(TAG, "Failed to acquire LVGL mutex for NVS save - timeout");
        return;
    }
    ESP_LOGI(TAG, "Successfully acquired LVGL mutex");

    // Copy current configuration to local variables while holding mutex
    // This minimizes the time we hold the mutex
    value_config_t local_config[13];
    char local_labels[13][64];
    int32_t local_rpm_max;
    
    // Quick copy under mutex protection
    memcpy(local_config, values_config, sizeof(values_config));
    memcpy(local_labels, label_texts, sizeof(label_texts));
    local_rpm_max = rpm_gauge_max;
    
    // Release LVGL mutex early
    ESP_LOGI(TAG, "Releasing LVGL mutex after data copy");
    example_lvgl_unlock();

    // Now do NVS operations without holding LVGL mutex
    nvs_handle_t handle;
    ESP_LOGI(TAG, "Opening NVS handle");
    esp_err_t err = nvs_open("can_config", NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS handle: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Successfully opened NVS handle");
    
    // Create a temporary buffer for string operations
    char key[32];
    bool save_success = true;
    
    for (int i = 0; i < 13; i++) {
        ESP_LOGI(TAG, "Saving configuration for value %d", i);

        // Add periodic task yields to prevent watchdog issues
        if (i % 3 == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }

        // enabled
        snprintf(key, sizeof(key), "enabled%d", i);
        uint8_t en = local_config[i].enabled ? 1 : 0;
        err = nvs_set_u8(handle, key, en);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save enabled state for value %d: %s", i, esp_err_to_name(err));
            save_success = false;
            break;
        }

        // can_id
        snprintf(key, sizeof(key), "can_id%d", i);
        err = nvs_set_u32(handle, key, local_config[i].can_id);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save CAN ID for value %d: %s", i, esp_err_to_name(err));
            save_success = false;
            break;
        }

        // endianess
        snprintf(key, sizeof(key), "endian%d", i);
        uint8_t end = (local_config[i].endianess == 0) ? 0 : 1;
        err = nvs_set_u8(handle, key, end);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save endianess for value %d: %s", i, esp_err_to_name(err));
            save_success = false;
            break;
        }

        // bit_start
        snprintf(key, sizeof(key), "bit_st%d", i);
        err = nvs_set_u8(handle, key, local_config[i].bit_start);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save bit_start for value %d: %s", i, esp_err_to_name(err));
            save_success = false;
            break;
        }

        // bit_length
        snprintf(key, sizeof(key), "bit_len%d", i);
        err = nvs_set_u8(handle, key, local_config[i].bit_length);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save bit_length for value %d: %s", i, esp_err_to_name(err));
            save_success = false;
            break;
        }

        // decimals
        snprintf(key, sizeof(key), "decimals%d", i);
        err = nvs_set_u8(handle, key, local_config[i].decimals);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save decimals for value %d: %s", i, esp_err_to_name(err));
            save_success = false;
            break;
        }

        // value_offset
        snprintf(key, sizeof(key), "val_off%d", i);
        err = nvs_set_blob(handle, key, &local_config[i].value_offset, sizeof(local_config[i].value_offset));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save value_offset for value %d: %s", i, esp_err_to_name(err));
            save_success = false;
            break;
        }

        // scale
        snprintf(key, sizeof(key), "scale%d", i);
        err = nvs_set_blob(handle, key, &local_config[i].scale, sizeof(local_config[i].scale));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save scale for value %d: %s", i, esp_err_to_name(err));
            save_success = false;
            break;
        }

        // is_signed
        snprintf(key, sizeof(key), "is_signed%d", i);
        uint8_t signed_val = local_config[i].is_signed ? 1 : 0;
        err = nvs_set_u8(handle, key, signed_val);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save is_signed for value %d: %s", i, esp_err_to_name(err));
            save_success = false;
            break;
        }

        // Save RPM bar color for RPM value
        if (i == RPM_VALUE_ID - 1) {
            snprintf(key, sizeof(key), "rpm_color%d", i);
            uint32_t color_value = local_config[i].rpm_bar_color.full;
            err = nvs_set_u32(handle, key, color_value);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save RPM color for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save RPM limiter effect
            snprintf(key, sizeof(key), "rpm_limit_eff%d", i);
            err = nvs_set_u8(handle, key, local_config[i].rpm_limiter_effect);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save RPM limiter effect for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save RPM limiter value
            snprintf(key, sizeof(key), "rpm_limit_val%d", i);
            err = nvs_set_i32(handle, key, local_config[i].rpm_limiter_value);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save RPM limiter value for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save RPM limiter color
            snprintf(key, sizeof(key), "rpm_limit_col%d", i);
            err = nvs_set_u32(handle, key, local_config[i].rpm_limiter_color.full);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save RPM limiter color for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save RPM lights enabled
            snprintf(key, sizeof(key), "rpm_lights_en%d", i);
            err = nvs_set_u8(handle, key, local_config[i].rpm_lights_enabled ? 1 : 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save RPM lights enabled for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save RPM gradient enabled
            snprintf(key, sizeof(key), "rpm_grad_en%d", i);
            err = nvs_set_u8(handle, key, local_config[i].rpm_gradient_enabled ? 1 : 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save RPM gradient enabled for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
        }

        // Save warning settings for panels (indices 0-7)
        if (i < 8) {
            // Warning High Threshold
            snprintf(key, sizeof(key), "warn_hi_th%d", i);
            err = nvs_set_blob(handle, key, &local_config[i].warning_high_threshold, 
                           sizeof(local_config[i].warning_high_threshold));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save warning high threshold for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }

            // Warning Low Threshold
            snprintf(key, sizeof(key), "warn_lo_th%d", i);
            err = nvs_set_blob(handle, key, &local_config[i].warning_low_threshold,
                           sizeof(local_config[i].warning_low_threshold));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save warning low threshold for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }

            // Warning High Color
            snprintf(key, sizeof(key), "warn_hi_col%d", i);
            uint32_t hi_color = local_config[i].warning_high_color.full;
            err = nvs_set_u32(handle, key, hi_color);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save warning high color for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }

            // Warning Low Color
            snprintf(key, sizeof(key), "warn_lo_col%d", i);
            uint32_t lo_color = local_config[i].warning_low_color.full;
            err = nvs_set_u32(handle, key, lo_color);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save warning low color for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }

            // Warning Enabled Flags
            snprintf(key, sizeof(key), "warn_enabled%d", i);
            uint8_t warn_flags = (local_config[i].warning_high_enabled ? 0x02 : 0) |
                               (local_config[i].warning_low_enabled ? 0x01 : 0);
            err = nvs_set_u8(handle, key, warn_flags);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save warning flags for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
        }

        // Save bar min/max for bar values
        if (i == BAR1_VALUE_ID - 1 || i == BAR2_VALUE_ID - 1) {
            snprintf(key, sizeof(key), "bar_min%d", i);
            err = nvs_set_i32(handle, key, local_config[i].bar_min);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save bar_min for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            snprintf(key, sizeof(key), "bar_max%d", i);
            err = nvs_set_i32(handle, key, local_config[i].bar_max);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save bar_max for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save bar_low value
            snprintf(key, sizeof(key), "bar_low%d", i);
            err = nvs_set_i32(handle, key, local_config[i].bar_low);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save bar_low for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save bar_high value
            snprintf(key, sizeof(key), "bar_high%d", i);
            err = nvs_set_i32(handle, key, local_config[i].bar_high);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save bar_high for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save fuel input configuration
            snprintf(key, sizeof(key), "fuel_en%d", i);
            uint8_t fuel_enabled = local_config[i].use_fuel_input ? 1 : 0;
            err = nvs_set_u8(handle, key, fuel_enabled);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save fuel input enabled for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save fuel empty voltage
            snprintf(key, sizeof(key), "fuel_empty%d", i);
            err = nvs_set_blob(handle, key, &local_config[i].fuel_empty_voltage, sizeof(local_config[i].fuel_empty_voltage));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save fuel empty voltage for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save fuel full voltage
            snprintf(key, sizeof(key), "fuel_full%d", i);
            err = nvs_set_blob(handle, key, &local_config[i].fuel_full_voltage, sizeof(local_config[i].fuel_full_voltage));
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save fuel full voltage for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save bar colors
            snprintf(key, sizeof(key), "blc%d", i);
            err = nvs_set_u32(handle, key, local_config[i].bar_low_color.full);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save bar low color for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            snprintf(key, sizeof(key), "bhc%d", i);
            err = nvs_set_u32(handle, key, local_config[i].bar_high_color.full);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save bar high color for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            snprintf(key, sizeof(key), "birc%d", i);
            err = nvs_set_u32(handle, key, local_config[i].bar_in_range_color.full);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save bar in range color for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
        }
        
        // Save GPS speed source setting for speed value (SPEED_VALUE_ID = 10, index 9)
        if (i == SPEED_VALUE_ID - 1) {
            snprintf(key, sizeof(key), "use_gps%d", i);
            uint8_t use_gps = local_config[i].use_gps_for_speed ? 1 : 0;
            err = nvs_set_u8(handle, key, use_gps);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save GPS speed source for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save speed units setting (MPH/KMH)
            snprintf(key, sizeof(key), "use_mph%d", i);
            uint8_t use_mph = local_config[i].use_mph ? 1 : 0;
            err = nvs_set_u8(handle, key, use_mph);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save speed units for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
        }
        
        // Save gear detection mode for gear value (GEAR_VALUE_ID = 11, index 10)
        if (i == GEAR_VALUE_ID - 1) {
            snprintf(key, sizeof(key), "gear_mode%d", i);
            uint8_t gear_mode = local_config[i].gear_detection_mode;
            err = nvs_set_u8(handle, key, gear_mode);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save gear detection mode for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Save custom gear CAN IDs
            for (int j = 0; j < 12; j++) {
                snprintf(key, sizeof(key), "gear_can%d_%d", i, j);
                uint32_t gear_can_id = local_config[i].gear_custom_can_ids[j];
                err = nvs_set_u32(handle, key, gear_can_id);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "Failed to save gear CAN ID %d for value %d: %s", j, i, esp_err_to_name(err));
                    save_success = false;
                    break;
                }
            }
        }
    }

    if (save_success) {
        // Save labels
        ESP_LOGI(TAG, "Saving labels");
        for (int i = 0; i < 13; i++) {
            snprintf(key, sizeof(key), "label%d", i);
            err = nvs_set_str(handle, key, local_labels[i]);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save label for value %d: %s", i, esp_err_to_name(err));
                save_success = false;
                break;
            }
            
            // Yield every few iterations
            if (i % 5 == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }

        if (save_success) {
            // Save RPM gauge max
            ESP_LOGI(TAG, "Saving RPM gauge max");
            err = nvs_set_i32(handle, "rpm_max", local_rpm_max);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save RPM gauge max: %s", esp_err_to_name(err));
                save_success = false;
            }
        }

        if (save_success) {
            // Save RPM redline value
            extern int rpm_redline_value;
            ESP_LOGI(TAG, "Saving RPM redline value");
            err = nvs_set_i32(handle, "rpm_redline", rpm_redline_value);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save RPM redline value: %s", esp_err_to_name(err));
                save_success = false;
            }
        }

        if (save_success) {
            ESP_LOGI(TAG, "Committing NVS changes");
            err = nvs_commit(handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to commit NVS changes: %s", esp_err_to_name(err));
                save_success = false;
            }
        }
    }

    ESP_LOGI(TAG, "Closing NVS handle");
    nvs_close(handle);

    ESP_LOGI(TAG, "NVS save operation %s", save_success ? "completed successfully" : "failed");
}

void save_warning_configs_to_nvs(void) {
    nvs_handle_t handle;
    if (nvs_open("warn_config", NVS_READWRITE, &handle) == ESP_OK) {
        for (int i = 0; i < 8; i++) {
            char key[32];
            
            // Save CAN ID
            snprintf(key, sizeof(key), "warn_can_id%d", i);
            esp_err_t err = nvs_set_u32(handle, key, warning_configs[i].can_id);
            if (err != ESP_OK) {
                printf("Error saving CAN ID for warning %d: %s\n", i, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            
            // Save bit position
            snprintf(key, sizeof(key), "warn_bit_pos%d", i);
            err = nvs_set_u8(handle, key, warning_configs[i].bit_position);
            if (err != ESP_OK) {
                printf("Error saving bit position for warning %d: %s\n", i, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            
            // Save active color
            snprintf(key, sizeof(key), "warn_color%d", i);
            uint32_t color_value = warning_configs[i].active_color.full;
            err = nvs_set_u32(handle, key, color_value);
            if (err != ESP_OK) {
                printf("Error saving color for warning %d: %s\n", i, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            
            // Save label
            snprintf(key, sizeof(key), "warn_label%d", i);
            err = nvs_set_str(handle, key, warning_configs[i].label);
            if (err != ESP_OK) {
                printf("Error saving label for warning %d: %s\n", i, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(1));

            // Save toggle mode setting
            snprintf(key, sizeof(key), "warn_is_mom%d", i);
            uint8_t is_momentary = warning_configs[i].is_momentary ? 1 : 0;
            err = nvs_set_u8(handle, key, is_momentary);
            if (err != ESP_OK) {
                printf("Error saving toggle mode for warning %d: %s\n", i, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(1));

            // Save current state
            snprintf(key, sizeof(key), "warn_state%d", i);
            uint8_t current_state = warning_configs[i].current_state ? 1 : 0;
            err = nvs_set_u8(handle, key, current_state);
            if (err != ESP_OK) {
                printf("Error saving current state for warning %d: %s\n", i, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        
        esp_err_t err = nvs_commit(handle);
        if (err != ESP_OK) {
            printf("Error committing warning configs to NVS: %s\n", esp_err_to_name(err));
        }
        nvs_close(handle);
    }
}

void load_warning_configs_from_nvs(void) {
    nvs_handle_t handle;
    if (nvs_open("warn_config", NVS_READWRITE, &handle) == ESP_OK) {
        for (int i = 0; i < 8; i++) {
            char key[32];
            
            // Load CAN ID
            snprintf(key, sizeof(key), "warn_can_id%d", i);
            uint32_t can_id;
            if (nvs_get_u32(handle, key, &can_id) == ESP_OK) {
                warning_configs[i].can_id = can_id;
                printf("Loaded warning %d CAN ID: 0x%X\n", i, can_id);
            }
            
            // Load bit position
            snprintf(key, sizeof(key), "warn_bit_pos%d", i);
            uint8_t bit_pos;
            if (nvs_get_u8(handle, key, &bit_pos) == ESP_OK) {
                warning_configs[i].bit_position = bit_pos;
                printf("Loaded warning %d bit position: %d\n", i, bit_pos);
            }
            
            // Load active color
            snprintf(key, sizeof(key), "warn_color%d", i);
            uint32_t color_value;
            if (nvs_get_u32(handle, key, &color_value) == ESP_OK) {
                warning_configs[i].active_color.full = color_value;
                printf("Loaded warning %d color: 0x%X\n", i, color_value);
            }
            
            // Load label
            snprintf(key, sizeof(key), "warn_label%d", i);
            size_t required_size = sizeof(warning_configs[i].label);
            if (nvs_get_str(handle, key, warning_configs[i].label, &required_size) == ESP_OK) {
                printf("Loaded warning %d label: %s\n", i, warning_configs[i].label);
            }

            // Load toggle mode setting
            snprintf(key, sizeof(key), "warn_is_mom%d", i);
            uint8_t is_momentary;
            if (nvs_get_u8(handle, key, &is_momentary) == ESP_OK) {
                warning_configs[i].is_momentary = (is_momentary == 1);
                printf("Loaded warning %d toggle mode: %s\n", i, 
                    warning_configs[i].is_momentary ? "Momentary" : "Toggle");
            } else {
                warning_configs[i].is_momentary = true; // Default to momentary if not found
            }

            // Load current state
            snprintf(key, sizeof(key), "warn_state%d", i);
            uint8_t current_state;
            if (nvs_get_u8(handle, key, &current_state) == ESP_OK) {
                warning_configs[i].current_state = (current_state == 1);
                printf("Loaded warning %d current state: %d\n", i, current_state);
            } else {
                warning_configs[i].current_state = false; // Default to off if not found
            }
        }
        nvs_close(handle);
    }
}

void save_indicator_configs_to_nvs(void) {
    printf("Saving indicator configurations to NVS...\n");
    nvs_handle_t handle;
    if (nvs_open("can_config", NVS_READWRITE, &handle) == ESP_OK) {
        for (int i = 0; i < 2; i++) {
            char key[32];
            
            printf("Saving indicator %d: CAN=0x%X, Bit=%d, Momentary=%d, State=%d\n", 
                   i, indicator_configs[i].can_id, indicator_configs[i].bit_position, 
                   indicator_configs[i].is_momentary, indicator_configs[i].current_state);
            
            // Save CAN ID
            snprintf(key, sizeof(key), "ind_can_id%d", i);
            esp_err_t err = nvs_set_u32(handle, key, indicator_configs[i].can_id);
            if (err != ESP_OK) {
                printf("Error saving CAN ID for indicator %d: %s\n", i, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(1));
            
            // Save bit position
            snprintf(key, sizeof(key), "ind_bit_pos%d", i);
            err = nvs_set_u8(handle, key, indicator_configs[i].bit_position);
            if (err != ESP_OK) {
                printf("Error saving bit position for indicator %d: %s\n", i, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(1));

            // Save toggle mode setting
            snprintf(key, sizeof(key), "ind_is_mom%d", i);
            uint8_t is_momentary = indicator_configs[i].is_momentary ? 1 : 0;
            err = nvs_set_u8(handle, key, is_momentary);
            if (err != ESP_OK) {
                printf("Error saving toggle mode for indicator %d: %s\n", i, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(1));

            // Note: current_state is not saved as it represents real-time CAN bus data, not a persistent setting

            // Save animation setting
            snprintf(key, sizeof(key), "ind_anim%d", i);
            uint8_t animation_enabled = indicator_configs[i].animation_enabled ? 1 : 0;
            err = nvs_set_u8(handle, key, animation_enabled);
            if (err != ESP_OK) {
                printf("Error saving animation setting for indicator %d: %s\n", i, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        
        esp_err_t err = nvs_commit(handle);
        if (err != ESP_OK) {
            printf("Error committing indicator configs to NVS: %s\n", esp_err_to_name(err));
        } else {
            printf("Indicator configurations committed to NVS successfully\n");
        }
        nvs_close(handle);
    } else {
        printf("Failed to open can_config NVS namespace\n");
    }
}

void load_indicator_configs_from_nvs(void) {
    printf("Starting to load indicator configurations from NVS...\n");
    nvs_handle_t handle;
    esp_err_t err = nvs_open("can_config", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        printf("Successfully opened can_config NVS namespace\n");
        for (int i = 0; i < 2; i++) {
            char key[32];
            
            // Load CAN ID
            snprintf(key, sizeof(key), "ind_can_id%d", i);
            uint32_t can_id;
            err = nvs_get_u32(handle, key, &can_id);
            if (err == ESP_OK) {
                indicator_configs[i].can_id = can_id;
                printf("Loaded indicator %d CAN ID: 0x%X\n", i, can_id);
            } else {
                printf("Failed to load indicator %d CAN ID (key: %s): %s\n", i, key, esp_err_to_name(err));
            }
            
            // Load bit position
            snprintf(key, sizeof(key), "ind_bit_pos%d", i);
            uint8_t bit_pos;
            err = nvs_get_u8(handle, key, &bit_pos);
            if (err == ESP_OK) {
                indicator_configs[i].bit_position = bit_pos;
                printf("Loaded indicator %d bit position: %d\n", i, bit_pos);
            } else {
                printf("Failed to load indicator %d bit position (key: %s): %s\n", i, key, esp_err_to_name(err));
            }

            // Load toggle mode setting
            snprintf(key, sizeof(key), "ind_is_mom%d", i);
            uint8_t is_momentary;
            err = nvs_get_u8(handle, key, &is_momentary);
            if (err == ESP_OK) {
                indicator_configs[i].is_momentary = (is_momentary == 1);
                printf("Loaded indicator %d toggle mode: %s\n", i, 
                    indicator_configs[i].is_momentary ? "Momentary" : "Toggle");
            } else {
                printf("Failed to load indicator %d toggle mode (key: %s): %s - using default (momentary)\n", i, key, esp_err_to_name(err));
                indicator_configs[i].is_momentary = true; // Default to momentary if not found
            }

            // Always reset current state to false on boot - indicators should start OFF regardless of saved state
            // The current_state represents real-time CAN bus data, not a persistent setting
            indicator_configs[i].current_state = false;
            printf("Reset indicator %d current state to OFF on boot\n", i);

            // Load animation setting
            snprintf(key, sizeof(key), "ind_anim%d", i);
            uint8_t animation_enabled;
            err = nvs_get_u8(handle, key, &animation_enabled);
            if (err == ESP_OK) {
                indicator_configs[i].animation_enabled = (animation_enabled == 1);
                printf("Loaded indicator %d animation: %s\n", i, indicator_configs[i].animation_enabled ? "Enabled" : "Disabled");
            } else {
                printf("Failed to load indicator %d animation (key: %s): %s - using default (enabled)\n", i, key, esp_err_to_name(err));
                indicator_configs[i].animation_enabled = true; // Default to animated if not found
            }
        }
        nvs_close(handle);
        printf("Finished loading indicator configurations from NVS\n");
    } else {
        printf("Failed to open can_config NVS namespace: %s\n", esp_err_to_name(err));
    }
}

static void init_pwm(void) {
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz         = LEDC_FREQUENCY,
        .clk_cfg         = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LEDC_OUTPUT_IO,
        .duty          = 0, // Initially set to 0
        .hpoint        = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
    
    // Set duty to 50%
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, LEDC_DUTY));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
}



void app_main(void)
{
    // Initialize PWM for GPIO16
    init_pwm();
    
    init_nvs();
    
    // EARLY CAN DRIVER INITIALIZATION - Initialize CAN driver early but task comes later
    ESP_LOGI(TAG, "Early CAN driver initialization for fast startup...");
    can_init();
    ESP_LOGI(TAG, "CAN driver ready - task will start after LVGL mutex creation");
    
    // Initialize display brightness from saved settings
    init_display_brightness();
    
    // Load ECU preconfig settings from NVS
    load_ecu_preconfig();

    static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
    static lv_disp_drv_t disp_drv;      // contains callback functions


ESP_LOGI(TAG, "Install RGB LCD panel driver");
esp_lcd_panel_handle_t panel_handle = NULL;
esp_lcd_rgb_panel_config_t panel_config = {
    .data_width = 16,                            // RGB565 in parallel mode, thus 16bit in width
    .psram_trans_align = 64,
    .num_fbs = EXAMPLE_LCD_NUM_FB,               // Number of frame buffers
#if CONFIG_EXAMPLE_USE_BOUNCE_BUFFER
    .bounce_buffer_size_px = 10 * EXAMPLE_LCD_H_RES,
#endif
    .clk_src = LCD_CLK_SRC_DEFAULT,
    .disp_gpio_num = EXAMPLE_PIN_NUM_DISP_EN,
    .pclk_gpio_num = EXAMPLE_PIN_NUM_PCLK,
    .vsync_gpio_num = EXAMPLE_PIN_NUM_VSYNC,
    .hsync_gpio_num = EXAMPLE_PIN_NUM_HSYNC,
    .de_gpio_num = EXAMPLE_PIN_NUM_DE,
    .data_gpio_nums = {
        EXAMPLE_PIN_NUM_DATA0,
        EXAMPLE_PIN_NUM_DATA1,
        EXAMPLE_PIN_NUM_DATA2,
        EXAMPLE_PIN_NUM_DATA3,
        EXAMPLE_PIN_NUM_DATA4,
        EXAMPLE_PIN_NUM_DATA5,
        EXAMPLE_PIN_NUM_DATA6,
        EXAMPLE_PIN_NUM_DATA7,
        EXAMPLE_PIN_NUM_DATA8,
        EXAMPLE_PIN_NUM_DATA9,
        EXAMPLE_PIN_NUM_DATA10,
        EXAMPLE_PIN_NUM_DATA11,
        EXAMPLE_PIN_NUM_DATA12,
        EXAMPLE_PIN_NUM_DATA13,
        EXAMPLE_PIN_NUM_DATA14,
        EXAMPLE_PIN_NUM_DATA15,
    },
    .timings = {
        .pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ,
        .h_res = EXAMPLE_LCD_H_RES,
        .v_res = EXAMPLE_LCD_V_RES,
        .hsync_back_porch = 8,
        .hsync_front_porch = 8,
        .hsync_pulse_width = 4,
        .vsync_back_porch = 16,
        .vsync_front_porch = 16,
        .vsync_pulse_width = 4,
        .flags.pclk_active_neg = true,
    },
    .flags = {
        .fb_in_psram = true,                     // Allocate frame buffer in PSRAM
        .no_fb = false,                          // Use frame buffer
        .refresh_on_demand = false,              // Continuous refresh
#if CONFIG_EXAMPLE_USE_BOUNCE_BUFFER
        .bb_invalidate_cache = true,             // Invalidate cache when using bounce buffer
#endif
    }
};

ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));

    ESP_LOGI(TAG, "Register event callbacks");
    esp_lcd_rgb_panel_event_callbacks_t cbs = {
        .on_vsync = example_on_vsync_event,
    };
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, &disp_drv));

    ESP_LOGI(TAG, "Initialize RGB LCD panel");
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
    // Don't turn on backlight immediately - wait until display is ready with black background
    ESP_LOGI(TAG, "LCD backlight pin configured (will turn on after display setup)");
#endif
gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    // Initialize I2C
    ESP_ERROR_CHECK(i2c_master_init());
    ESP_LOGI(TAG, "I2C initialized successfully");
   // gpio_init();
    // Set initial configuration for I2C device at 0x24
    uint8_t write_buf = 0x01;
    i2c_master_write_to_device(I2C_MASTER_NUM, 0x24, &write_buf, 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

    // Additional configuration for SD card CS pin at address 0x38
    write_buf = 0x0A;
    i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &write_buf, 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    vTaskDelay(pdMS_TO_TICKS(100));  // Use FreeRTOS delay instead of ROM delay


    // Reset the touch screen as part of the initial setup
    write_buf = 0x2C;
    i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &write_buf, 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    vTaskDelay(pdMS_TO_TICKS(100));  // Use FreeRTOS delay instead of ROM delay

    gpio_set_level(GPIO_INPUT_IO_4, 0); // Set GPIO level for reset
    vTaskDelay(pdMS_TO_TICKS(100));  // Use FreeRTOS delay instead of ROM delay

    // Continue with touch screen initialization
    write_buf = 0x2E;
    i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &write_buf, 1, I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
    vTaskDelay(pdMS_TO_TICKS(200));  // Use FreeRTOS delay instead of ROM delay

    esp_lcd_touch_handle_t tp = NULL;
    esp_lcd_panel_io_handle_t tp_io_handle = NULL;

    ESP_LOGI(TAG, "Initialize I2C");

    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();

    ESP_LOGI(TAG, "Initialize touch IO (I2C)");
    /* Touch IO handle */
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_MASTER_NUM, &tp_io_config, &tp_io_handle));
    esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_V_RES,
        .y_max = EXAMPLE_LCD_H_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    /* Initialize touch */
    ESP_LOGI(TAG, "Initialize touch controller GT911");
    esp_err_t touch_ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp);
    if (touch_ret != ESP_OK) {
        ESP_LOGW(TAG, "Touch controller GT911 initialization failed (0x%x), continuing without touch...", touch_ret);
        tp = NULL; // Set to NULL to indicate no touch available
    } else {
        ESP_LOGI(TAG, "Touch controller GT911 initialized successfully");
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

ESP_LOGI(TAG, "Allocate separate LVGL draw buffers from PSRAM");
// Try for larger buffer size (1/4 of the screen) for better performance
size_t buf_size = (EXAMPLE_LCD_H_RES * (EXAMPLE_LCD_V_RES / 4) * sizeof(lv_color_t));
// Align buffer size to 32 bytes
buf_size = (buf_size + 31) & ~31;

// Try progressively smaller buffer sizes
void *buf1 = NULL, *buf2 = NULL;
const size_t min_buf_size = (EXAMPLE_LCD_H_RES * 30 * sizeof(lv_color_t));

while (buf_size >= min_buf_size) {
    buf1 = heap_caps_aligned_alloc(32, buf_size, MALLOC_CAP_SPIRAM);
    if (buf1) {
        buf2 = heap_caps_aligned_alloc(32, buf_size, MALLOC_CAP_SPIRAM);
        if (buf2) break;
        heap_caps_free(buf1);
    }
    buf_size = (buf_size * 3) / 4; // Reduce by 25%
    buf_size = (buf_size + 31) & ~31; // Keep aligned
}

if (!buf1 || !buf2) {
    ESP_LOGW(TAG, "Failed to allocate PSRAM buffers, trying internal memory");
    // Try with internal memory
    buf_size = (EXAMPLE_LCD_H_RES * 30 * sizeof(lv_color_t));
    buf_size = (buf_size + 31) & ~31;
    
    buf1 = heap_caps_aligned_alloc(32, buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buf1) {
        buf2 = heap_caps_aligned_alloc(32, buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!buf2) {
            heap_caps_free(buf1);
            buf1 = NULL;
        }
    }
    
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Critical: Failed to allocate LVGL buffers");
        abort();
    }
}

ESP_LOGI(TAG, "LVGL buffers allocated successfully, size: %u bytes each", buf_size);
lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_size / sizeof(lv_color_t));
	

    ESP_LOGI(TAG, "Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb = example_lvgl_flush_cb;
    disp_drv.draw_buf = &disp_buf;
    disp_drv.user_data = panel_handle;
#if CONFIG_EXAMPLE_DOUBLE_FB
    disp_drv.full_refresh = false; // the full_refresh mode can maintain the synchronization between the two frame buffers
#endif
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

    // Set display background to black immediately to prevent white flicker
    lv_disp_set_bg_color(disp, lv_color_hex(0x000000));
    ESP_LOGI(TAG, "Display background set to black to prevent white flicker");
    
    // Don't turn on backlight yet - wait until splash screen is ready
    ESP_LOGI(TAG, "Backlight will be turned on after splash screen loads");

    // Register touch input device only if touch controller is available
    if (tp != NULL) {
        static lv_indev_drv_t indev_drv;    // Input device driver (Touch)
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.disp = disp;
        indev_drv.read_cb = example_lvgl_touch_cb;
        indev_drv.user_data = tp;

        lv_indev_drv_register(&indev_drv);
        ESP_LOGI(TAG, "Touch input device registered successfully");
    } else {
        ESP_LOGW(TAG, "Touch controller not available, continuing without touch input");
    }

    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    assert(lvgl_mux);
    
    // Create a black screen BEFORE starting LVGL task to prevent white flash
    ESP_LOGI(TAG, "Creating initial black screen to prevent white flash");
    lv_obj_t * black_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(black_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(black_screen, LV_OPA_COVER, 0);
    lv_obj_clear_flag(black_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_scr_load(black_screen);
    
    ESP_LOGI(TAG, "Create LVGL task");
    xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, &lvglTaskHandle, 1);
    
    // Give LVGL task time to start and render the black screen
    vTaskDelay(pdMS_TO_TICKS(100));
    
    // Now turn on backlight since black screen is displayed
#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
    ESP_LOGI(TAG, "Turning on LCD backlight now that black screen is rendered");
    gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);
#endif

    // Load indicator configs BEFORE starting CAN task so they're available when CAN data arrives
    ESP_LOGI(TAG, "Initializing and loading indicator configurations from NVS...");
    init_indicator_configs();
    load_indicator_configs_from_nvs();

    // Now that LVGL mutex exists and configs are loaded, start CAN task for fast data reception
    ESP_LOGI(TAG, "Creating CAN task now that LVGL mutex is ready...");
    xTaskCreatePinnedToCore(can_receive_task, "can_receive_task", 
                           4096, NULL, 4, &canTaskHandle, 0);
    ESP_LOGI(TAG, "CAN task started - data will be available when UI loads");

        ESP_LOGI(TAG, "Loading splash screen for smooth boot experience");

    // Lock the mutex due to the LVGL APIs are not thread-safe
    if (example_lvgl_lock(-1)) {
        ui_init();  // This shows the splash screen which will auto-transition to main screen
        example_lvgl_unlock();
    }

    // Allow splash screen to render and become visible
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_LOGI(TAG, "Splash screen displayed, continuing with system initialization...");

    // Initialize remaining components while splash is showing
    // CAN bus already initialized early for fast startup
    //init_sd_card();
    init_wifi_screen();
   // test_sd_card_write();

    // Initialize GPS
    ESP_LOGI(TAG, "Initializing GPS...");
    if (gps_init() != ESP_OK) {
        ESP_LOGE(TAG, "GPS initialization failed!");
    } else {
        ESP_LOGI(TAG, "GPS initialized successfully!");
    }

    // Initialize fuel input ADC
    ESP_LOGI(TAG, "Initializing fuel input (GPIO6/ADC1_CH5)...");
    if (fuel_input_init() != ESP_OK) {
        ESP_LOGE(TAG, "Fuel input initialization failed!");
    } else {
        ESP_LOGI(TAG, "Fuel input initialized successfully!");
    }

    // Start web server (will start once WiFi is connected)
    ESP_LOGI(TAG, "Starting web server...");
    if (web_server_start() != ESP_OK) {
        ESP_LOGE(TAG, "Web server failed to start!");
    } else {
        ESP_LOGI(TAG, "Web server started successfully!");
        ESP_LOGI(TAG, "=== WEB INTERFACE READY ===");
        ESP_LOGI(TAG, "Connect to your WiFi network first, then:");
        ESP_LOGI(TAG, "Open your web browser and go to: http://[ESP32_IP_ADDRESS]");
        ESP_LOGI(TAG, "You can see the IP address in the device settings or WiFi connection logs");
        ESP_LOGI(TAG, "==============================");
    }

    ESP_LOGI(TAG, "All systems initialized - splash screen will transition to main screen automatically");

    // CAN task created earlier for fast data loading

}
