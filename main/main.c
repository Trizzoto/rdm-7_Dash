#include "device_id.h"
#include "device_settings.h"
#include "display_capture.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/ledc.h"
#include "driver/twai.h"
#include "esp32s3/rom/cache.h"
#include "esp_adc/adc_oneshot.h"
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
#include "storage/sd_manager.h"
#include "storage/data_logger.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"
#include "io/wire_inputs.h"
#include "layout/layout_manager.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ota_handler.h"
#include "sdkconfig.h"
#include "ui/screens/ui_wifi.h"
#include "net/wifi_manager.h"
#include "net/mdns_service.h"
#include "net/uart_protocol.h"
// #include "net/usb_cdc_protocol.h"  // Disabled — see USB CDC note in app_main
#include "storage/config_store.h"
#include "ui/theme.h"
#include "ui/ui.h"
#include "web_server.h"
#include "widgets/signal_internal.h"
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h> // For unlink

// External declarations

// Define the LVGL mutex
SemaphoreHandle_t lvgl_mux = NULL;

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
#define GPIO_INPUT_PIN_SEL (1ULL << GPIO_INPUT_IO_4)
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
#include "screen_config.h"
#define EXAMPLE_LCD_H_RES SCREEN_W
#define EXAMPLE_LCD_V_RES SCREEN_H

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
#define EXAMPLE_LVGL_TASK_STACK_SIZE (16 * 1024)
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


/* CAN subsystem — TWAI hardware, receive task */
#include "can/can_manager.h"

// PWM configuration for GPIO16
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_MODE LEDC_LOW_SPEED_MODE
#define LEDC_OUTPUT_IO 16 // Define the output GPIO
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_DUTY_RES LEDC_TIMER_13_BIT // Set duty resolution to 13 bits
#define LEDC_FREQUENCY 5000				// Frequency in Hz (matches device_settings.c)
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

	if (lv_disp_flush_is_last(drv)) {
		signal_internal_count_frame();
	}
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
		2; // Minimal sleep — maximize CPU for LVGL rendering
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
		if (example_lvgl_lock(100)) {
			// Reset failure counter on successful lock
			consecutive_failures = 0;

			// Handle any pending LVGL tasks
			lv_timer_handler();
			// Drain any queued CAN frames while we hold the LVGL mutex so all
			// widget/UI work stays single-threaded.
			can_process_queued_frames();
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
#define FUEL_SENDER_GPIO 6
#define FUEL_SENDER_ADC_UNIT ADC_UNIT_1
#define FUEL_SENDER_ADC_CH ADC_CHANNEL_5	  // GPIO 6 on ESP32-S3
#define FUEL_SENDER_ADC_ATTEN ADC_ATTEN_DB_12 // 0-3.3 V range
#define FUEL_SENDER_ADC_BITS ADC_BITWIDTH_12  // 0-4095

static adc_oneshot_unit_handle_t s_fuel_sender_adc = NULL;

/* Initialise the ADC for GPIO 6 */
void fuel_sender_adc_init(void) {
	adc_oneshot_unit_init_cfg_t init_cfg = {
		.unit_id = FUEL_SENDER_ADC_UNIT,
		.ulp_mode = ADC_ULP_MODE_DISABLE,
	};
	esp_err_t err = adc_oneshot_new_unit(&init_cfg, &s_fuel_sender_adc);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
		return;
	}

	adc_oneshot_chan_cfg_t chan_cfg = {
		.atten = FUEL_SENDER_ADC_ATTEN,
		.bitwidth = FUEL_SENDER_ADC_BITS,
	};
	err = adc_oneshot_config_channel(s_fuel_sender_adc, FUEL_SENDER_ADC_CH,
							   &chan_cfg);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
	}
}

/* Return the current GPIO 6 voltage (0.0 – 3.3 V) */
float fuel_sender_read_voltage(void) {
	if (!s_fuel_sender_adc)
		return 0.0f;
	int raw = 0;
	if (adc_oneshot_read(s_fuel_sender_adc, FUEL_SENDER_ADC_CH, &raw) != ESP_OK)
		return 0.0f;
	return (raw / 4095.0f) * 3.3f;
}


static void _deferred_wifi_boot_cb(lv_timer_t *timer) {
	(void)timer;
	wifi_manager_start();
	wifi_manager_auto_connect();

	ESP_LOGI(TAG, "Starting web server...");
	if (web_server_start() != ESP_OK) {
		ESP_LOGE(TAG, "Web server failed to start!");
	} else {
		ESP_LOGI(TAG, "Web server started successfully!");
		ESP_LOGI(TAG, "=== WEB INTERFACE READY ===");
		ESP_LOGI(TAG, "Connect to WiFi hotspot '%s' → http://192.168.4.1",
				 wifi_get_ap_ssid());
		ESP_LOGI(TAG, "==============================");
	}
}

void app_main(void) {
	ESP_LOGW(TAG, "[mem] app_main enter  free_int=%u  free_psram=%u",
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

	// Initialize PWM for GPIO16
	init_pwm();

	init_nvs();

	/* Mount LittleFS and ensure default layout exists before any component
	 * (web server or dashboard) touches layout files. Prevents panic if a
	 * GET/POST hits the layout API before the dashboard has run. */
	esp_err_t layout_err = layout_manager_init();
	if (layout_err != ESP_OK) {
		ESP_LOGE(TAG,
				 "Early layout_manager_init failed (%s) — layout API and "
				 "dashboard may use fallback",
				 esp_err_to_name(layout_err));
	}

	// EARLY CAN DRIVER INITIALIZATION - Initialize CAN driver early but task
	// comes later
	ESP_LOGI(TAG, "Early CAN driver initialization for fast startup...");
	can_init();
	ESP_LOGI(TAG,
			 "CAN driver ready - task will start after LVGL mutex creation");

	// Initialize display brightness from saved settings
	init_display_brightness();

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
		.bounce_buffer_size_px = 20 * EXAMPLE_LCD_H_RES,
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
	// Set initial configuration for I2C device at 0x24 (CH422G mode register)
	// 0x24 = mode config, 0x38 = output register (pins 0-7)
	// USB_SEL (EXIO5) is set via 0x38 writes below (bit 5 in 0x2C = HIGH)
	uint8_t write_buf = 0x01;
	i2c_master_write_to_device(I2C_MASTER_NUM, 0x24, &write_buf, 1,
							   I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);

	// Additional configuration for SD card CS pin at address 0x38
	write_buf = 0x0A;
	i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &write_buf, 1,
							   I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
	vTaskDelay(pdMS_TO_TICKS(10));

	// Reset the touch screen as part of the initial setup
	write_buf = 0x2C;
	i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &write_buf, 1,
							   I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
	vTaskDelay(pdMS_TO_TICKS(10));

	gpio_set_level(GPIO_INPUT_IO_4, 0); // Set GPIO level for reset
	vTaskDelay(pdMS_TO_TICKS(10));

	// Continue with touch screen initialization
	write_buf = 0x2E;
	i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &write_buf, 1,
							   I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);
	vTaskDelay(pdMS_TO_TICKS(30)); // GT911 needs ~10ms after reset release

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

	/* Try internal SRAM first — much faster writes than PSRAM since
	 * the LCD DMA is constantly reading the PSRAM framebuffer.
	 * Internal SRAM avoids the SPI bus contention bottleneck.
	 *
	 * Cap the internal allocation aggressively so that:
	 *   (a) total free internal DRAM stays high enough for WiFi's
	 *       esf_buf_setup_static pool (~50 KB contiguous required),
	 *   (b) largest_free_block doesn't drop below ~32 KB — two big
	 *       48 KB chunks fragment the heap so badly that no single
	 *       contiguous WiFi allocation can succeed later.
	 * 10–20 lines is standard for an 800×480 panel on ESP32-S3. */
	void *buf1 = NULL, *buf2 = NULL;
	size_t buf_size = 0;

	/* Internal SRAM: try 20, then 16, then 10 lines */
	static const int try_lines[] = {20, 16, 10};
	for (size_t i = 0; i < sizeof(try_lines) / sizeof(try_lines[0]); i++) {
		int lines = try_lines[i];
		buf_size = (EXAMPLE_LCD_H_RES * lines * sizeof(lv_color_t));
		buf_size = (buf_size + 31) & ~31;
		buf1 = heap_caps_aligned_alloc(32, buf_size,
									   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
		if (buf1) {
			buf2 = heap_caps_aligned_alloc(32, buf_size,
										   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
			if (buf2) {
				ESP_LOGI(TAG, "Using internal SRAM draw buffers (%d lines)", lines);
				break;
			}
			heap_caps_free(buf1);
			buf1 = NULL;
		}
	}

	/* Fallback: PSRAM buffers (slower due to bus contention with LCD DMA) */
	if (!buf1 || !buf2) {
		ESP_LOGW(TAG, "Internal SRAM insufficient, falling back to PSRAM buffers");
		buf_size = (EXAMPLE_LCD_H_RES * (EXAMPLE_LCD_V_RES / 4) * sizeof(lv_color_t));
		buf_size = (buf_size + 31) & ~31;
		const size_t min_buf_size = (EXAMPLE_LCD_H_RES * 30 * sizeof(lv_color_t));

		while (buf_size >= min_buf_size) {
			buf1 = heap_caps_aligned_alloc(32, buf_size, MALLOC_CAP_SPIRAM);
			if (buf1) {
				buf2 = heap_caps_aligned_alloc(32, buf_size, MALLOC_CAP_SPIRAM);
				if (buf2) break;
				heap_caps_free(buf1);
				buf1 = NULL;
			}
			buf_size = (buf_size * 3) / 4;
			buf_size = (buf_size + 31) & ~31;
		}
	}

	if (!buf1 || !buf2) {
		ESP_LOGE(TAG, "Critical: Failed to allocate LVGL buffers");
		abort();
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

	/* Apply saved display rotation (#23). 0/90/180/270 map directly to
	 * LV_DISP_ROT_NONE..LV_DISP_ROT_270. Note: touch coords are auto-swapped
	 * by LVGL when rotation is enabled; for physical 90/270 the panel is
	 * still driven in landscape and LVGL rotates the framebuffer in software. */
	{
		uint8_t saved_rot = 0;
		(void) config_store_load_rotation(&saved_rot);
		if (saved_rot > 0 && saved_rot <= 3) {
			lv_disp_set_rotation(disp, (lv_disp_rot_t)saved_rot);
			ESP_LOGI(TAG, "Applied saved display rotation: %u (90deg steps)", (unsigned)saved_rot);
		}
	}

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
	vTaskDelay(pdMS_TO_TICKS(30));

	// Now turn on backlight since black screen is displayed
#if EXAMPLE_PIN_NUM_BK_LIGHT >= 0
	ESP_LOGI(TAG, "Turning on LCD backlight now that black screen is rendered");
	gpio_set_level(EXAMPLE_PIN_NUM_BK_LIGHT, EXAMPLE_LCD_BK_LIGHT_ON_LEVEL);
#endif

	wire_inputs_init();
	ESP_LOGI(TAG, "Indicator wire inputs (GPIO %d left, %d right) initialized",
			 WIRE_INPUT_LEFT_GPIO, WIRE_INPUT_RIGHT_GPIO);

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

	/* Start indicator wire task: reads GPIO 43/44 and drives indicators when
	 * source is Wire */
	xTaskCreatePinnedToCore(wire_inputs_task, "ind_wire", 2048, NULL, 3, NULL,
							0);
	ESP_LOGI(TAG, "Indicator wire task started");

	/* Fuel sender – ADC on GPIO 6, exposed as FUEL_SENDER_V signal */
	fuel_sender_adc_init();

	// Initialize remaining components while splash is showing
	sd_manager_init();
	data_logger_init();
	/* Initialize UART serial protocol (core 0, priority 5).
	 * Shares GPIO 43/44 with the ESP-IDF console (UART0) — the board's
	 * CH422G I/O expander selects which UART drives the USB bridge. */
	if (uart_protocol_init() != ESP_OK) {
		ESP_LOGE(TAG, "UART protocol init failed!");
	}

	/* USB CDC disabled — ESP32-S3 USB Serial/JTAG and USB OTG share the
	 * same PHY and can't coexist. To enable USB CDC in the future:
	 * 1. Disable CONFIG_USJ_ENABLE_USB_SERIAL_JTAG in sdkconfig
	 * 2. Disable CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
	 * 3. Uncomment usb_cdc_protocol_init() below
	 */
	// if (usb_cdc_protocol_init() != ESP_OK) {
	// 	ESP_LOGW(TAG, "USB CDC protocol init failed");
	// }

	ESP_LOGW(TAG, "[mem] before wifi_manager_init  free_int=%u  largest_int=%u",
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

	/* Initialize WiFi manager (creates netif, no radio start yet) */
	wifi_manager_init();
	wifi_ui_init();

	/* Initialise mDNS after the netif stack is up — the daemon attaches to
	   any netif that comes online, so hostname "rdm7" resolves on both STA
	   and SoftAP once those interfaces start. Refresh is called by
	   wifi_manager.c event handlers when IP or AP state changes. */
	rdm7_mdns_init();

	ESP_LOGW(TAG, "[mem] after wifi_ui_init  free_int=%u  largest_int=%u",
	         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
	         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

	/* Check if WiFi should start on boot */
	wifi_boot_config_t boot_cfg;
	config_store_load_wifi_boot(&boot_cfg);

	/* First-run wizard (#17): on a fresh device (no first_run_done flag in NVS),
	   auto-enable WiFi + AP so the user can reach the web UI immediately from
	   their phone — no manual menu steps required. We mark the flag after this
	   boot so subsequent boots respect the user's actual preference. */
	bool first_run_done = false;
	config_store_load_first_run_done(&first_run_done);
	if (!first_run_done) {
		ESP_LOGW(TAG, "First run detected — auto-enabling WiFi + AP for onboarding");
		boot_cfg.wifi_on_boot = true;
		boot_cfg.ap_enabled = true;
		(void) config_store_save_wifi_boot(&boot_cfg);
		(void) config_store_save_first_run_done(true);
		/* An on-screen banner with the AP SSID / password / IP is surfaced from
		   the splash-screen flow once the dashboard is visible; for now the
		   wifi info is already available under Settings > Wi-Fi. */
	}

	if (boot_cfg.wifi_on_boot) {
		ESP_LOGI(TAG, "WiFi-on-boot enabled, starting WiFi after 4s delay...");
		/* Start WiFi with a 4-second delay to let dashboard load first */
		lv_timer_t *wifi_boot_timer = lv_timer_create(_deferred_wifi_boot_cb, 4000, NULL);
		lv_timer_set_repeat_count(wifi_boot_timer, 1);
	} else {
		ESP_LOGI(TAG, "WiFi disabled at boot — enable from Settings > Wi-Fi");
	}

	ESP_LOGI(TAG, "All systems initialized - splash screen will transition to "
				  "main screen automatically");
}
