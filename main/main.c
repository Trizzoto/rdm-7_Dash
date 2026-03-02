#include "device_id.h"
#include "display_capture.h"
#include "ui/theme.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include <math.h>
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/twai.h"
#include "esp32s3/rom/cache.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "storage/config_store.h"
#include "ota_handler.h"
#include "screens/ui_Screen3.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include "device_settings.h"
#include "ui/screens/ui_wifi.h"
#include "ui/ui.h"
#include "ui_Screen1.h"
#include "web_server.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h> // For unlink

// External declarations

#define EXAMPLE_MAX_CHAR_SIZE 64
#define MOUNT_POINT "/sdcard"
sdmmc_card_t *card; // Declare globally if not done already

// Define the LVGL mutex
SemaphoreHandle_t lvgl_mux = NULL;

#define LV_USE_ANIMATION 0
#define LV_USE_SHADOW 0
#define LV_USE_BLEND_MODES 0

#define I2C_MASTER_SCL_IO 9 /*!< GPIO number used for I2C master clock */
#define I2C_MASTER_SDA_IO 8 /*!< GPIO number used for I2C master data  */
#define I2C_MASTER_NUM                                                         \
	0 /*!< I2C master i2c port number, the number of i2c peripheral interfaces \
		 available will depend on the chip */
#define I2C_MASTER_FREQ_HZ 400000	/*!< I2C master clock frequency */
#define I2C_MASTER_TX_BUF_DISABLE 0 /*!< I2C master doesn't need buffer */
#define I2C_MASTER_RX_BUF_DISABLE 0 /*!< I2C master doesn't need buffer */
#define I2C_MASTER_TIMEOUT_MS 1000

#define GPIO_INPUT_IO_4 4
#define GPIO_INPUT_PIN_SEL 1
/* Indicator wire input: left = GPIO 43, right = GPIO 44 (digital inputs, high = on) */
#define INDICATOR_LEFT_GPIO  43
#define INDICATOR_RIGHT_GPIO 44
static const char *TAG = "main";

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your
/// LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define EXAMPLE_LCD_PIXEL_CLOCK_HZ (14 * 1000 * 1000)
#define EXAMPLE_LCD_BK_LIGHT_ON_LEVEL 1
#define EXAMPLE_LCD_BK_LIGHT_OFF_LEVEL !EXAMPLE_LCD_BK_LIGHT_ON_LEVEL
#define EXAMPLE_PIN_NUM_BK_LIGHT -1
#define EXAMPLE_PIN_NUM_HSYNC 46
#define EXAMPLE_PIN_NUM_VSYNC 3
#define EXAMPLE_PIN_NUM_DE 5
#define EXAMPLE_PIN_NUM_PCLK 7
#define EXAMPLE_PIN_NUM_DATA0 14  // B3
#define EXAMPLE_PIN_NUM_DATA1 38  // B4
#define EXAMPLE_PIN_NUM_DATA2 18  // B5
#define EXAMPLE_PIN_NUM_DATA3 17  // B6
#define EXAMPLE_PIN_NUM_DATA4 10  // B7
#define EXAMPLE_PIN_NUM_DATA5 39  // G2
#define EXAMPLE_PIN_NUM_DATA6 0	  // G3
#define EXAMPLE_PIN_NUM_DATA7 45  // G4
#define EXAMPLE_PIN_NUM_DATA8 48  // G5
#define EXAMPLE_PIN_NUM_DATA9 47  // G6
#define EXAMPLE_PIN_NUM_DATA10 21 // G7
#define EXAMPLE_PIN_NUM_DATA11 1  // R3
#define EXAMPLE_PIN_NUM_DATA12 2  // R4
#define EXAMPLE_PIN_NUM_DATA13 42 // R5
#define EXAMPLE_PIN_NUM_DATA14 41 // R6
#define EXAMPLE_PIN_NUM_DATA15 40 // R7
#define EXAMPLE_PIN_NUM_DISP_EN -1
#define LCD_CMD_BITS_DEFAULT 8
#define LCD_PARAM_BITS_DEFAULT 8
#define LCD_RGB_PANEL_WRITE_BYTES NULL // Use default write bytes function

// The pixel number in horizontal and vertical
#define EXAMPLE_LCD_H_RES 800
#define EXAMPLE_LCD_V_RES 480

#if CONFIG_EXAMPLE_DOUBLE_FB
#define EXAMPLE_LCD_NUM_FB 2
#else
#define EXAMPLE_LCD_NUM_FB 1
#endif // CONFIG_EXAMPLE_DOUBLE_FB

#define EXAMPLE_LVGL_TICK_PERIOD_MS 2
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS                                         \
	10 // Reduced from 30ms for 70fps responsiveness
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS                                         \
	5 // Reduced from 20ms for better performance
#define EXAMPLE_LVGL_TASK_STACK_SIZE (8 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY 8

// we use two semaphores to sync the VSYNC event and the LVGL task, to avoid
// potential tearing effect
#if CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM
SemaphoreHandle_t sem_vsync_end;
SemaphoreHandle_t sem_gui_ready;
#endif

// Declare the flush callback function
void my_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
				 lv_color_t *color_map);

lv_disp_drv_t disp_drv;
lv_disp_draw_buf_t draw_buf;

/* CAN subsystem — TWAI hardware, dispatch table, receive task */
#include "can/can_manager.h"
#include "can/can_dispatch.h"
#include "ui/screens/ui_Screen3.h"

// PWM configuration for GPIO16
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO 16 // Define the output GPIO
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_DUTY_RES LEDC_TIMER_13_BIT // Set duty resolution to 13 bits
#define LEDC_FREQUENCY 9000				// Frequency in Hz
#define LEDC_DUTY (8191)

static esp_err_t i2c_master_init(void) {
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

	return i2c_driver_install(i2c_master_port, conf.mode,
							  I2C_MASTER_RX_BUF_DISABLE,
							  I2C_MASTER_TX_BUF_DISABLE, 0);
}

static bool
example_on_vsync_event(esp_lcd_panel_handle_t panel,
					   const esp_lcd_rgb_panel_event_data_t *event_data,
					   void *user_data) {
	BaseType_t high_task_awoken = pdFALSE;
#if CONFIG_EXAMPLE_AVOID_TEAR_EFFECT_WITH_SEM
	if (xSemaphoreTakeFromISR(sem_gui_ready, &high_task_awoken) == pdTRUE) {
		xSemaphoreGiveFromISR(sem_vsync_end, &high_task_awoken);
	}
#endif
	return high_task_awoken == pdTRUE;
}

#define MOUNT_POINT "/sdcard"
#define SD_MOSI 11
#define SD_CLK 12
#define SD_MISO 13
#define SD_CS 4 // GPIO for CS

void init_sd_card(void) {
	esp_err_t ret;
	sdmmc_card_t *card;
	const char mount_point[] = MOUNT_POINT;
	ESP_LOGI("SD_CARD", "Initializing SD card");

	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
		.format_if_mount_failed = false,
		.max_files = 5,
		.allocation_unit_size = 16 * 1024};

	// Configure the SPI bus
	sdmmc_host_t host = SDSPI_HOST_DEFAULT();
	host.max_freq_khz = 10000; // Reduced frequency for compatibility

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
	ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config,
								  &mount_config, &card);
	if (ret != ESP_OK) {
		ESP_LOGE("SD_CARD", "Failed to mount filesystem. Error: %s",
				 esp_err_to_name(ret));
		return;
	}

	ESP_LOGI("SD_CARD", "SD card mounted successfully");
	sdmmc_card_print_info(stdout, card);
}

static void example_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
								  lv_color_t *color_map) {
	esp_lcd_panel_handle_t panel_handle =
		(esp_lcd_panel_handle_t)drv->user_data;
	int offsetx1 = area->x1;
	int offsetx2 = area->x2;
	int offsety1 = area->y1;
	int offsety2 = area->y2;

	// Direct transfer to display - PSRAM buffers are handled by the LCD driver

	esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1,
							  offsety2 + 1, color_map);

	lv_disp_flush_ready(drv);
}

bool example_lvgl_lock(int timeout_ms) {
	// Convert timeout in milliseconds to FreeRTOS ticks
	// If timeout_ms is set to -1, the program will block until the condition is
	// met
	const TickType_t timeout_ticks =
		(timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
	return xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks) == pdTRUE;
}

void example_lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }

TaskHandle_t lvglTaskHandle = NULL;

// Change the LVGL task function to be accessible
void example_lvgl_port_task(void *pvParameter) {
	ESP_LOGI(TAG, "Starting LVGL task");
	const uint32_t refresh_period_ms =
		14; // 70fps target (1000ms / 70fps = ~14.3ms)
	uint32_t last_error_time = 0;
	const uint32_t error_report_interval = 5000; // 5 seconds between error logs
	uint32_t consecutive_failures = 0;
	const uint32_t max_failures =
		10; // Maximum consecutive failures before task notification
	uint32_t start_time;

	while (1) {
		start_time = esp_timer_get_time() / 1000;

		// Lock the mutex with shorter timeout for better responsiveness during
		// OTA
		if (example_lvgl_lock(pdMS_TO_TICKS(100))) {
			// Reset failure counter on successful lock
			consecutive_failures = 0;

			// Handle any pending LVGL tasks
			lv_timer_handler();
			example_lvgl_unlock();

			// Calculate remaining time in the refresh period
			uint32_t elapsed = (esp_timer_get_time() / 1000) - start_time;
			uint32_t delay_ms = (elapsed >= refresh_period_ms)
									? 1
									: (refresh_period_ms - elapsed);
			vTaskDelay(pdMS_TO_TICKS(delay_ms));
		} else {
			consecutive_failures++;

			uint32_t current_time = esp_timer_get_time() / 1000;
			if (current_time - last_error_time > error_report_interval) {
				ESP_LOGW(TAG, "Failed to acquire LVGL mutex (failures: %lu)",
						 consecutive_failures);
				last_error_time = current_time;
			}

			// If we've failed too many times, notify the system
			if (consecutive_failures >= max_failures) {
				ESP_LOGE(TAG, "LVGL task experiencing persistent mutex "
							  "acquisition failures");
				consecutive_failures = 0; // Reset counter
			}

			// Shorter delay for faster retry during high contention
			vTaskDelay(pdMS_TO_TICKS(2));
		}
	}
}

void gpio_init(void) {
	// zero-initialize the config structure.
	gpio_config_t io_conf = {};
	// disable interrupt
	io_conf.intr_type = GPIO_INTR_DISABLE;
	// bit mask of the pins, use GPIO6 here
	io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
	// set as input mode
	io_conf.mode = GPIO_MODE_INPUT_OUTPUT;
	// enable pull-up mode
	io_conf.pull_up_en = 1;
	gpio_config(&io_conf);
}

// extern lv_obj_t *scr;
static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
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
	bool touchpad_pressed = esp_lcd_touch_get_coordinates(
		drv->user_data, touchpad_x, touchpad_y, NULL, &touchpad_cnt, 1);

	if (touchpad_pressed && touchpad_cnt > 0) {
		data->point.x = touchpad_x[0];
		data->point.y = touchpad_y[0];
		data->state = LV_INDEV_STATE_PR;
	}
}

void test_sd_card_write() {
	const char *file_path = MOUNT_POINT "/no.txt";
	FILE *file = fopen(file_path, "w"); // Open file for writing
	if (file == NULL) {
		ESP_LOGE("SD_CARD", "Failed to open file for writing");
		return;
	}

	// Write "Hello, World!" to the file
	fprintf(file, "you suck balls\n");
	fclose(file); // Close the file
	ESP_LOGI("SD_CARD", "File written successfully: %s", file_path);
}

extern warning_config_t warning_configs[8];
extern indicator_config_t indicator_configs[2]; // Left and Right indicators
extern value_config_t values_config[13];		// from your ui code
extern char label_texts[13][64];
extern char value_offset_texts[13][64];
extern int rpm_gauge_max;
extern int rpm_redline_value;
#define RPM_VALUE_ID 9
#define SPEED_VALUE_ID 10
#define GEAR_VALUE_ID 11
#define BAR1_VALUE_ID 12
#define BAR2_VALUE_ID 13

void init_nvs(void) {
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
		err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		nvs_flash_erase();
		nvs_flash_init();
	}

	// Initialize device ID system
	err = init_device_id();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Failed to initialize device ID system: %s",
				 esp_err_to_name(err));
	}
}

void load_values_config_from_nvs(void) {
	config_store_load_values(values_config, MAX_VALUES);
}

void save_values_config_to_nvs(void) {
	config_store_save_values(values_config, MAX_VALUES);
}

void save_warning_configs_to_nvs(void) {
	config_store_save_warnings(warning_configs, 8);
}

void load_warning_configs_from_nvs(void) {
	config_store_load_warnings(warning_configs, 8);
}

void save_indicator_configs_to_nvs(void) {
	config_store_save_indicators(indicator_configs, 2);
}

void load_indicator_configs_from_nvs(void) {
	config_store_load_indicators(indicator_configs, 2);
}

static void init_pwm(void) {
	// Prepare and then apply the LEDC PWM timer configuration
	ledc_timer_config_t ledc_timer = {.speed_mode = LEDC_MODE,
									  .timer_num = LEDC_TIMER,
									  .duty_resolution = LEDC_DUTY_RES,
									  .freq_hz = LEDC_FREQUENCY,
									  .clk_cfg = LEDC_AUTO_CLK};
	ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

	// Prepare and then apply the LEDC PWM channel configuration
	ledc_channel_config_t ledc_channel = {.speed_mode = LEDC_MODE,
										  .channel = LEDC_CHANNEL,
										  .timer_sel = LEDC_TIMER,
										  .intr_type = LEDC_INTR_DISABLE,
										  .gpio_num = LEDC_OUTPUT_IO,
										  .duty = 0, // Initially set to 0
										  .hpoint = 0};
	ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

	// Set duty to 50%
	ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, LEDC_DUTY));
	ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
}

/* -----------------------------------------------------------------------
 * Fuel Sender – GPIO 6, ADC1 Channel 5
 * -----------------------------------------------------------------------*/
#define FUEL_SENDER_GPIO      6
#define FUEL_SENDER_ADC_UNIT  ADC_UNIT_1
#define FUEL_SENDER_ADC_CH    ADC_CHANNEL_5   // GPIO 6 on ESP32-S3
#define FUEL_SENDER_ADC_ATTEN ADC_ATTEN_DB_12 // 0-3.3 V range
#define FUEL_SENDER_ADC_BITS  ADC_BITWIDTH_12 // 0-4095

static adc_oneshot_unit_handle_t s_fuel_sender_adc = NULL;

/* Initialise the ADC for GPIO 6 */
void fuel_sender_adc_init(void) {
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id  = FUEL_SENDER_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    adc_oneshot_new_unit(&init_cfg, &s_fuel_sender_adc);

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = FUEL_SENDER_ADC_ATTEN,
        .bitwidth = FUEL_SENDER_ADC_BITS,
    };
    adc_oneshot_config_channel(s_fuel_sender_adc, FUEL_SENDER_ADC_CH, &chan_cfg);
}

/* Return the current GPIO 6 voltage (0.0 – 3.3 V) */
float fuel_sender_read_voltage(void) {
    if (!s_fuel_sender_adc) return 0.0f;
    int raw = 0;
    if (adc_oneshot_read(s_fuel_sender_adc, FUEL_SENDER_ADC_CH, &raw) != ESP_OK)
        return 0.0f;
    return (raw / 4095.0f) * 3.3f;
}

/* Capture the current voltage as the "empty" calibration point */
void fuel_sender_capture_empty(uint8_t value_id) {
    if (value_id < 1 || value_id > MAX_VALUES) return;
    values_config[value_id - 1].fuel_sender_empty_v = fuel_sender_read_voltage();
    save_values_config_to_nvs();
    ESP_LOGI("FUEL", "Bar %d empty calibrated at %.3f V",
             value_id, values_config[value_id - 1].fuel_sender_empty_v);
}

/* Capture the current voltage as the "full" calibration point */
void fuel_sender_capture_full(uint8_t value_id) {
    if (value_id < 1 || value_id > MAX_VALUES) return;
    values_config[value_id - 1].fuel_sender_full_v = fuel_sender_read_voltage();
    save_values_config_to_nvs();
    ESP_LOGI("FUEL", "Bar %d full calibrated at %.3f V",
             value_id, values_config[value_id - 1].fuel_sender_full_v);
}

/* Background task: reads GPIO 6 and drives any fuel-sender-enabled bars */
static float s_fuel_filtered_v[2] = { -1.0f, -1.0f };

float fuel_sender_get_filtered_v(uint8_t bar_idx) {
    if (bar_idx > 1) return 0.0f;
    float v = s_fuel_filtered_v[bar_idx];
    return (v < 0.0f) ? fuel_sender_read_voltage() : v;
}

static void fuel_sender_task(void *pvParameters) {
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(200)); // wait for LVGL + UI init
    float *filtered_v = s_fuel_filtered_v;
    for (;;) {
        float voltage = fuel_sender_read_voltage();

        for (int i = 0; i < 2; i++) {
            int vi = (i == 0) ? BAR1_VALUE_ID - 1 : BAR2_VALUE_ID - 1;
            if (!values_config[vi].fuel_sender) continue;

            // EMA filter: slider 0 = raw; 1-100 maps alpha 0.80→0.995
            // giving time constants ~0.45 s (light) to ~20 s (car-gauge heavy)
            float alpha;
            uint8_t filt = values_config[vi].fuel_sender_filter;
            if (filt == 0) {
                alpha = 0.0f;
            } else {
                float t = (filt - 1) / 99.0f; // 0.0 → 1.0
                alpha = 0.80f + t * (0.995f - 0.80f);
            }
            if (filtered_v[i] < 0.0f) {
                filtered_v[i] = voltage; // seed on first reading
            } else {
                filtered_v[i] = alpha * filtered_v[i] + (1.0f - alpha) * voltage;
            }
            float v = filtered_v[i];

            float empty_v = values_config[vi].fuel_sender_empty_v;
            float full_v  = values_config[vi].fuel_sender_full_v;
            float range   = full_v - empty_v;

            float pct;
            if (fabsf(range) < 0.01f) {
                pct = 0.0f; // avoid div-by-zero before calibration
            } else {
                pct = (v - empty_v) / range;
                if (pct < 0.0f) pct = 0.0f;
                if (pct > 1.0f) pct = 1.0f;
            }

            int32_t bar_min = values_config[vi].bar_min;
            int32_t bar_max = values_config[vi].bar_max;
            int32_t bar_val = (int32_t)(bar_min + pct * (bar_max - bar_min));

            bar_update_t *upd = malloc(sizeof(bar_update_t));
            if (upd) {
                upd->bar_index    = i;
                upd->bar_value    = bar_val;
                upd->final_value  = bar_min + pct * (bar_max - bar_min);
                upd->config_index = vi;
                upd->is_timeout   = false;
                lv_async_call(update_bar_ui, upd);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // 10 Hz refresh
    }
}

/* Indicator wire input: GPIO 43 = left, GPIO 44 = right; high = on */
static void indicator_gpio_init(void) {
	gpio_config_t io_conf = {
		.pin_bit_mask = (1ULL << INDICATOR_LEFT_GPIO) | (1ULL << INDICATOR_RIGHT_GPIO),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_DISABLE,
		.pull_down_en = GPIO_PULLDOWN_ENABLE,
		.intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&io_conf);
	/* Ensure pins are pulled LOW when idle - small delay for stabilization */
	vTaskDelay(pdMS_TO_TICKS(10));
}

static void indicator_gpio_task(void *pvParameters) {
	(void)pvParameters;
	/* Small delay on startup to ensure GPIO is stable */
	vTaskDelay(pdMS_TO_TICKS(100));
	for (;;) {
		if (indicator_configs[0].input_source == 0 || indicator_configs[1].input_source == 0) {
			/* Read pins: LOW (0) = inactive, HIGH (1) = active */
			bool left_on = (gpio_get_level(INDICATOR_LEFT_GPIO) == 1);
			bool right_on = (gpio_get_level(INDICATOR_RIGHT_GPIO) == 1);
			if (example_lvgl_lock(pdMS_TO_TICKS(20))) {
				indicator_apply_analog_state(left_on, right_on);
				example_lvgl_unlock();
			}
		}
		vTaskDelay(pdMS_TO_TICKS(50));
	}
}

void app_main(void) {
	// Initialize PWM for GPIO16
	init_pwm();

	init_nvs();

	// EARLY CAN DRIVER INITIALIZATION - Initialize CAN driver early but task
	// comes later
	ESP_LOGI(TAG, "Early CAN driver initialization for fast startup...");
	can_init();
	ESP_LOGI(TAG,
			 "CAN driver ready - task will start after LVGL mutex creation");

	// Initialize display brightness from saved settings
	init_display_brightness();

	// Load ECU preconfig settings from NVS
	load_ecu_preconfig();

	static lv_disp_draw_buf_t
		disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
	static lv_disp_drv_t disp_drv; // contains callback functions

	ESP_LOGI(TAG, "Install RGB LCD panel driver");
	esp_lcd_panel_handle_t panel_handle = NULL;
	esp_lcd_rgb_panel_config_t panel_config = {
		.data_width = 16, // RGB565 in parallel mode, thus 16bit in width
		.psram_trans_align = 64,
		.num_fbs = EXAMPLE_LCD_NUM_FB, // Number of frame buffers
#if CONFIG_EXAMPLE_USE_BOUNCE_BUFFER
		.bounce_buffer_size_px = 10 * EXAMPLE_LCD_H_RES,
#endif
		.clk_src = LCD_CLK_SRC_DEFAULT,
		.disp_gpio_num = EXAMPLE_PIN_NUM_DISP_EN,
		.pclk_gpio_num = EXAMPLE_PIN_NUM_PCLK,
		.vsync_gpio_num = EXAMPLE_PIN_NUM_VSYNC,
		.hsync_gpio_num = EXAMPLE_PIN_NUM_HSYNC,
		.de_gpio_num = EXAMPLE_PIN_NUM_DE,
		.data_gpio_nums =
			{
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
		.timings =
			{
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
			.fb_in_psram = true,		// Allocate frame buffer in PSRAM
			.no_fb = false,				// Use frame buffer
			.refresh_on_demand = false, // Continuous refresh
#if CONFIG_EXAMPLE_USE_BOUNCE_BUFFER
			.bb_invalidate_cache =
				true, // Invalidate cache when using bounce buffer
#endif
		}};

	ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));

	ESP_LOGI(TAG, "Register event callbacks");
	esp_lcd_rgb_panel_event_callbacks_t cbs = {
		.on_vsync = example_on_vsync_event,
	};
	ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(
		panel_handle, &cbs, &disp_drv));

	ESP_LOGI(TAG, "Initialize RGB LCD panel");
	ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
	ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));

#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
	// Don't turn on backlight immediately - wait until display is ready with
	// black background
	ESP_LOGI(TAG,
			 "LCD backlight pin configured (will turn on after display setup)");
#endif
	gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
	// Initialize I2C
	ESP_ERROR_CHECK(i2c_master_init());
	ESP_LOGI(TAG, "I2C initialized successfully");
	// gpio_init();
	// Set initial configuration for I2C device at 0x24
	uint8_t write_buf = 0x01;
	i2c_master_write_to_device(I2C_MASTER_NUM, 0x24, &write_buf, 1,
							   I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

	// Additional configuration for SD card CS pin at address 0x38
	write_buf = 0x0A;
	i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &write_buf, 1,
							   I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
	vTaskDelay(pdMS_TO_TICKS(100)); // Use FreeRTOS delay instead of ROM delay

	// Reset the touch screen as part of the initial setup
	write_buf = 0x2C;
	i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &write_buf, 1,
							   I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
	vTaskDelay(pdMS_TO_TICKS(100)); // Use FreeRTOS delay instead of ROM delay

	gpio_set_level(GPIO_INPUT_IO_4, 0); // Set GPIO level for reset
	vTaskDelay(pdMS_TO_TICKS(100)); // Use FreeRTOS delay instead of ROM delay

	// Continue with touch screen initialization
	write_buf = 0x2E;
	i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &write_buf, 1,
							   I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
	vTaskDelay(pdMS_TO_TICKS(200)); // Use FreeRTOS delay instead of ROM delay

	esp_lcd_touch_handle_t tp = NULL;
	esp_lcd_panel_io_handle_t tp_io_handle = NULL;

	ESP_LOGI(TAG, "Initialize I2C");

	esp_lcd_panel_io_i2c_config_t tp_io_config =
		ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();

	ESP_LOGI(TAG, "Initialize touch IO (I2C)");
	/* Touch IO handle */
	ESP_ERROR_CHECK(
		esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)I2C_MASTER_NUM,
								 &tp_io_config, &tp_io_handle));
	esp_lcd_touch_config_t tp_cfg = {
		.x_max = EXAMPLE_LCD_V_RES,
		.y_max = EXAMPLE_LCD_H_RES,
		.rst_gpio_num = -1,
		.int_gpio_num = -1,
		.flags =
			{
				.swap_xy = 0,
				.mirror_x = 0,
				.mirror_y = 0,
			},
	};
	/* Initialize touch */
	ESP_LOGI(TAG, "Initialize touch controller GT911");
	esp_err_t touch_ret =
		esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp);
	if (touch_ret != ESP_OK) {
		ESP_LOGW(TAG,
				 "Touch controller GT911 initialization failed (0x%x), "
				 "continuing without touch...",
				 touch_ret);
		tp = NULL; // Set to NULL to indicate no touch available
	} else {
		ESP_LOGI(TAG, "Touch controller GT911 initialized successfully");
	}

	ESP_LOGI(TAG, "Initialize LVGL library");
	lv_init();

	ESP_LOGI(TAG, "Allocate separate LVGL draw buffers from PSRAM");
	// Try for larger buffer size (1/4 of the screen) for better performance
	size_t buf_size =
		(EXAMPLE_LCD_H_RES * (EXAMPLE_LCD_V_RES / 4) * sizeof(lv_color_t));
	// Align buffer size to 32 bytes
	buf_size = (buf_size + 31) & ~31;

	// Try progressively smaller buffer sizes
	void *buf1 = NULL, *buf2 = NULL;
	const size_t min_buf_size = (EXAMPLE_LCD_H_RES * 30 * sizeof(lv_color_t));

	while (buf_size >= min_buf_size) {
		buf1 = heap_caps_aligned_alloc(32, buf_size, MALLOC_CAP_SPIRAM);
		if (buf1) {
			buf2 = heap_caps_aligned_alloc(32, buf_size, MALLOC_CAP_SPIRAM);
			if (buf2)
				break;
			heap_caps_free(buf1);
		}
		buf_size = (buf_size * 3) / 4;	  // Reduce by 25%
		buf_size = (buf_size + 31) & ~31; // Keep aligned
	}

	if (!buf1 || !buf2) {
		ESP_LOGW(TAG,
				 "Failed to allocate PSRAM buffers, trying internal memory");
		// Try with internal memory
		buf_size = (EXAMPLE_LCD_H_RES * 30 * sizeof(lv_color_t));
		buf_size = (buf_size + 31) & ~31;

		buf1 = heap_caps_aligned_alloc(32, buf_size,
									   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		if (buf1) {
			buf2 = heap_caps_aligned_alloc(
				32, buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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

	ESP_LOGI(TAG, "LVGL buffers allocated successfully, size: %u bytes each",
			 buf_size);
	lv_disp_draw_buf_init(&disp_buf, buf1, buf2, buf_size / sizeof(lv_color_t));

	ESP_LOGI(TAG, "Register display driver to LVGL");
	lv_disp_drv_init(&disp_drv);
	disp_drv.hor_res = EXAMPLE_LCD_H_RES;
	disp_drv.ver_res = EXAMPLE_LCD_V_RES;
	disp_drv.flush_cb = example_lvgl_flush_cb;
	disp_drv.draw_buf = &disp_buf;
	disp_drv.user_data = panel_handle;
#if CONFIG_EXAMPLE_DOUBLE_FB
	disp_drv.full_refresh =
		false; // the full_refresh mode can maintain the synchronization between
			   // the two frame buffers
#endif
	lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

	// Set display background to black immediately to prevent white flicker
	lv_disp_set_bg_color(disp, THEME_COLOR_BG);
	ESP_LOGI(TAG, "Display background set to black to prevent white flicker");

	// Don't turn on backlight yet - wait until splash screen is ready
	ESP_LOGI(TAG, "Backlight will be turned on after splash screen loads");

	// Register touch input device only if touch controller is available
	if (tp != NULL) {
		static lv_indev_drv_t indev_drv; // Input device driver (Touch)
		lv_indev_drv_init(&indev_drv);
		indev_drv.type = LV_INDEV_TYPE_POINTER;
		indev_drv.disp = disp;
		indev_drv.read_cb = example_lvgl_touch_cb;
		indev_drv.user_data = tp;

		lv_indev_drv_register(&indev_drv);
		ESP_LOGI(TAG, "Touch input device registered successfully");
	} else {
		ESP_LOGW(
			TAG,
			"Touch controller not available, continuing without touch input");
	}

	lvgl_mux = xSemaphoreCreateRecursiveMutex();
	assert(lvgl_mux);

	// Create a black screen BEFORE starting LVGL task to prevent white flash
	ESP_LOGI(TAG, "Creating initial black screen to prevent white flash");
	lv_obj_t *black_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(black_screen, THEME_COLOR_BG, 0);
	lv_obj_set_style_bg_opa(black_screen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(black_screen, LV_OBJ_FLAG_SCROLLABLE);
	lv_scr_load(black_screen);

	ESP_LOGI(TAG, "Create LVGL task");
	xTaskCreatePinnedToCore(example_lvgl_port_task, "LVGL",
							EXAMPLE_LVGL_TASK_STACK_SIZE, NULL,
							EXAMPLE_LVGL_TASK_PRIORITY, &lvglTaskHandle, 1);

	// Give LVGL task time to start and render the black screen
	vTaskDelay(pdMS_TO_TICKS(100));

	// Now turn on backlight since black screen is displayed
#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
	ESP_LOGI(TAG, "Turning on LCD backlight now that black screen is rendered");
	gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);
#endif

	// Load indicator configs BEFORE starting CAN task so they're available when
	// CAN data arrives
	ESP_LOGI(TAG,
			 "Initializing and loading indicator configurations from NVS...");
	init_indicator_configs();
	load_indicator_configs_from_nvs();

	indicator_gpio_init();
	ESP_LOGI(TAG, "Indicator wire inputs (GPIO %d left, %d right) initialized",
			 INDICATOR_LEFT_GPIO, INDICATOR_RIGHT_GPIO);

	/* Now that the LVGL mutex exists, start the CAN receive task */
	ESP_LOGI(TAG, "Creating CAN task now that LVGL mutex is ready...");
	can_start_task();
	ESP_LOGI(TAG, "CAN task started - data will be available when UI loads");

	ESP_LOGI(TAG, "Loading splash screen for smooth boot experience");

	// Lock the mutex due to the LVGL APIs are not thread-safe
	if (example_lvgl_lock(-1)) {
		ui_init(); // This shows the splash screen which will auto-transition to
				   // main screen
		example_lvgl_unlock();
	}

	/* Start indicator wire task: reads GPIO 43/44 and drives indicators when source is Wire */
	xTaskCreatePinnedToCore(indicator_gpio_task, "ind_wire", 2048, NULL, 3, NULL, 0);
	ESP_LOGI(TAG, "Indicator wire task started");

	/* Fuel sender – ADC on GPIO 6, drives bars when fuel_sender is enabled */
	fuel_sender_adc_init();
	xTaskCreatePinnedToCore(fuel_sender_task, "fuel_sender", 3072, NULL, 3, NULL, 0);
	ESP_LOGI(TAG, "Fuel sender task started (GPIO %d)", FUEL_SENDER_GPIO);

	// Allow splash screen to render and become visible
	vTaskDelay(pdMS_TO_TICKS(200));
	ESP_LOGI(
		TAG,
		"Splash screen displayed, continuing with system initialization...");

	// Initialize remaining components while splash is showing
	// CAN bus already initialized early for fast startup
	// init_sd_card();
	init_wifi_screen();
	// test_sd_card_write();

	// Start web server (will start once WiFi is connected)
	ESP_LOGI(TAG, "Starting web server...");
	if (web_server_start() != ESP_OK) {
		ESP_LOGE(TAG, "Web server failed to start!");
	} else {
		ESP_LOGI(TAG, "Web server started successfully!");
		ESP_LOGI(TAG, "=== WEB INTERFACE READY ===");
		ESP_LOGI(TAG, "Connect to your WiFi network first, then:");
		ESP_LOGI(TAG,
				 "Open your web browser and go to: http://[ESP32_IP_ADDRESS]");
		ESP_LOGI(TAG, "You can see the IP address in the device settings or "
					  "WiFi connection logs");
		ESP_LOGI(TAG, "==============================");
	}

	ESP_LOGI(TAG, "All systems initialized - splash screen will transition to "
				  "main screen automatically");

	// CAN task created earlier for fast data loading
}
