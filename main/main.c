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
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_types.h"
#include "io/wire_inputs.h"
#include "layout/layout_manager.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "net/uart_protocol.h"
#include "net/wifi_manager.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ota_handler.h"
#include "sdkconfig.h"
#include "storage/data_logger.h"
#include "storage/sd_manager.h"
#include "storage/user_signals.h"
#include "system/crash_log.h"
#include "system/heap_monitor.h"
#include "system/night_mode.h"
#include "system/remote_touch.h"
#include "ui/screens/ui_wifi.h"


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
  0 /*!< I2C master i2c port number, the number of i2c peripheral interfaces   \
           available will depend on the chip */
#define I2C_MASTER_FREQ_HZ 400000   /*!< I2C master clock frequency */
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
#define EXAMPLE_PIN_NUM_DATA6 0   // G3
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
#define LEDC_FREQUENCY 5000 // Frequency in Hz (matches device_settings.c)
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

/* Dirty-rect-topology diagnostic. LVGL calls this once per refresh where
 * anything was drawn; `px_num` is the sum of unjoined dirty-rect areas for
 * the frame (see lv_refr.c:620), i.e. the real on-screen invalidation load
 * *after* the join cascade has run. Aggregate over ~1s and log average
 * px/frame as a % of the full screen: sustained near-100% with sim on
 * confirms the join cascade is merging everything to near-full-screen,
 * while a drop to low double digits when a couple of widgets stop ticking
 * would confirm the topology cliff.
 * Safe to leave in: one ESP_LOGI per second, no work on hot path. */
static void rdm7_lvgl_monitor_cb(lv_disp_drv_t *drv, uint32_t elaps_ms,
                                 uint32_t px_num) {
  static uint32_t s_start_ms = 0;
  static uint32_t s_frame_cnt = 0;
  static uint64_t s_px_sum = 0;
  static uint64_t s_elaps_sum = 0;

  uint32_t now = lv_tick_get();
  if (s_start_ms == 0)
    s_start_ms = now;

  s_frame_cnt++;
  s_px_sum += px_num;
  s_elaps_sum += elaps_ms;

  uint32_t window = now - s_start_ms;
  if (window >= 1000 && s_frame_cnt > 0) {
    const uint32_t screen_px = (uint32_t)drv->hor_res * drv->ver_res;
    uint32_t avg_px = (uint32_t)(s_px_sum / s_frame_cnt);
    uint32_t avg_elaps = (uint32_t)(s_elaps_sum / s_frame_cnt);
    uint32_t avg_pct = screen_px
                           ? (uint32_t)((s_px_sum * 100ULL) /
                                        ((uint64_t)screen_px * s_frame_cnt))
                           : 0;
    uint32_t fps_x10 = (s_frame_cnt * 10000U) / window;
    ESP_LOGD("refr_diag",
             "fps=%lu.%lu frames=%lu avg_px=%lu (%lu%% of screen) "
             "avg_render=%lu ms",
             (unsigned long)(fps_x10 / 10), (unsigned long)(fps_x10 % 10),
             (unsigned long)s_frame_cnt, (unsigned long)avg_px,
             (unsigned long)avg_pct, (unsigned long)avg_elaps);
    s_start_ms = now;
    s_frame_cnt = 0;
    s_px_sum = 0;
    s_elaps_sum = 0;
  }
}

/* ── Capture source: LCD panel framebuffer ────────────────────────────────
 * We read directly from the LCD panel's own framebuffer (via
 * esp_lcd_rgb_panel_get_frame_buffer) rather than maintaining a separate
 * shadow copy. Saves ~768 KB of PSRAM permanently AND removes the per-flush
 * memcpy that was adding to the LVGL task's frame budget — which was tipping
 * the task watchdog when SIM + live preview ran concurrently.
 *
 * Tearing trade-off: the panel DMA is reading whichever FB is "front" while
 * LVGL writes to "back". If our capture read races a flush, we get a torn
 * frame for one iteration. Invisible at 2-3 fps preview, and the old shadow
 * path tore in exactly the same way (we held no LVGL lock during shadow
 * read). We always read from FB0; the panel alternates fb0↔fb1 at 70 Hz so
 * FB0 is alternately front and back — either way it contains a complete
 * frame's worth of pixels.
 *
 * s_shadow_seq is still bumped per flush so the MJPEG loop can detect
 * "nothing changed" and reuse its cached JPEG (near-zero CPU when idle). */
static void *s_panel_fb = NULL;       /* &RGB565 pixels in PSRAM */
static bool s_panel_fb_ready = false; /* flips true after first flush */
static volatile uint32_t s_shadow_seq = 0;

/* Public getters (legacy names kept so display_capture.c doesn't churn). */
uint16_t *display_capture_shadow_fb(void) { return (uint16_t *)s_panel_fb; }
bool display_capture_shadow_ready(void) { return s_panel_fb_ready; }
uint32_t display_capture_shadow_seq(void) { return s_shadow_seq; }

static void rdm_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                              lv_color_t *color_map) {
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
  int offsetx1 = area->x1;
  int offsetx2 = area->x2;
  int offsety1 = area->y1;
  int offsety2 = area->y2;

  // Direct transfer to display - PSRAM buffers are handled by the LCD driver

  esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1,
                            offsety2 + 1, color_map);

  /* Lazily cache the panel FB pointer on first flush. Use fb_num=1 so the
   * variadic call only needs one pointer argument — we always read the
   * same FB for capture (accepting an occasional torn frame) and don't
   * need fb1. fb_num=2 (two-pointer) form was disabled temporarily during
   * a debugging session; this single-pointer form is the safer API shape. */
  if (!s_panel_fb) {
    void *fb0 = NULL;
    if (esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 1, &fb0) == ESP_OK) {
      s_panel_fb = fb0;
    }
  }
  s_panel_fb_ready = true;

  /* Bump the change counter so the MJPEG stream loop can tell a new frame
   * is available. Single 32-bit write — atomic on ESP32-S3. */
  s_shadow_seq++;

  if (lv_disp_flush_is_last(drv)) {
    signal_internal_count_frame();
  }
  lv_disp_flush_ready(drv);
}

bool rdm_lvgl_lock(int timeout_ms) {
  // Convert timeout in milliseconds to FreeRTOS ticks
  // If timeout_ms is set to -1, the program will block until the condition is
  // met
  const TickType_t timeout_ticks =
      (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks) == pdTRUE;
}

void rdm_lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }

TaskHandle_t lvglTaskHandle = NULL;

// Change the LVGL task function to be accessible
void rdm_lvgl_port_task(void *pvParameter) {
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
    if (rdm_lvgl_lock(100)) {
      // Reset failure counter on successful lock
      consecutive_failures = 0;

      // Handle any pending LVGL tasks
      lv_timer_handler();
      // Drain any queued CAN frames while we hold the LVGL mutex so all
      // widget/UI work stays single-threaded.
      can_process_queued_frames();
      rdm_lvgl_unlock();

      // Calculate remaining time in the refresh period
      uint32_t elapsed = (esp_timer_get_time() / 1000) - start_time;
      uint32_t delay_ms =
          (elapsed >= refresh_period_ms) ? 1 : (refresh_period_ms - elapsed);
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
static void rdm_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
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

  /* Rising edge on the physical panel — clear any latched virtual press
   * from CONTROL mode. A real finger always wins over a possibly-stuck
   * remote press (e.g. browser dropped a pointerup). Without this, a
   * stuck virtual press can hold a widget captured and make the device
   * feel dead until the 350 ms watchdog fires. */
  static bool s_prev_phys_pressed = false;
  bool now_pressed = (touchpad_pressed && touchpad_cnt > 0);
  if (now_pressed && !s_prev_phys_pressed) {
    remote_touch_force_release();
  }
  s_prev_phys_pressed = now_pressed;

  if (now_pressed) {
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
#define FUEL_SENDER_ADC_CH ADC_CHANNEL_5      // GPIO 6 on ESP32-S3
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

  /* Honour boot config for AP mode — on first-run we explicitly set
     ap_enabled=true in app_main so the hotspot is discoverable immediately.
     On subsequent boots, this reflects the user's saved preference. */
  wifi_boot_config_t cfg;
  if (config_store_load_wifi_boot(&cfg) == ESP_OK && cfg.ap_enabled) {
    wifi_manager_enable_ap(true);
    ESP_LOGI(TAG, "AP mode enabled by boot config");
  }

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

  /* Diagnostic: periodic heap snapshot to surface the ongoing PHY-leak
   * crash. Started LAST so the first sample baselines the steady-state
   * after all heavy allocators (WiFi, web server, LVGL) have settled. */
  heap_monitor_start();
}

void app_main(void) {
  // Initialize PWM for GPIO16
  init_pwm();

  init_nvs();

  /* Read and persist the previous-boot reset reason so panics are visible
     across reboots. Logs a one-line summary and increments a lifetime
     panic counter on crash-class reasons. */
  crash_log_init();

  /* Suppress routine `httpd_sock_err: error in recv : 104` warnings.
   * errno 104 = ECONNRESET — a client (usually the browser) closed an
   * HTTP connection mid-recv. Completely benign; happens every time the
   * user reloads Studio, toggles CONTROL, switches tabs, etc. Keeps
   * genuine ERROR-level httpd issues visible. */
  esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);

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

  /* Night-mode subsystem: must init before any code can subscribe (i.e.
   * before dashboard_init). Idempotent — safe to call again. */
  night_mode_init();

  // EARLY CAN DRIVER INITIALIZATION - Initialize CAN driver early but task
  // comes later
  ESP_LOGI(TAG, "Early CAN driver initialization for fast startup...");
  can_init();
  ESP_LOGI(TAG, "CAN driver ready - task will start after LVGL mutex creation");

  // Initialize display brightness from saved settings
  init_display_brightness();

  static lv_disp_draw_buf_t
      disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
  static lv_disp_drv_t disp_drv; // contains callback functions

  ESP_LOGI(TAG, "Install RGB LCD panel driver");
  esp_lcd_panel_handle_t panel_handle = NULL;
  esp_lcd_rgb_panel_config_t panel_config =
  {.data_width = 16, // RGB565 in parallel mode, thus 16bit in width
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
       .fb_in_psram = true,        // Allocate frame buffer in PSRAM
       .no_fb = false,             // Use frame buffer
       .refresh_on_demand = false, // Continuous refresh
#if CONFIG_EXAMPLE_USE_BOUNCE_BUFFER
       .bb_invalidate_cache = true, // Invalidate cache when using bounce buffer
#endif
   } };

  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &panel_handle));

  ESP_LOGI(TAG, "Register event callbacks");
  esp_lcd_rgb_panel_event_callbacks_t cbs = {
      .on_vsync = example_on_vsync_event,
  };
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs,
                                                             &disp_drv));

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

  /* GPIO4 carries the GT911 INT line. We drive it LOW during the RST
   * rising edge so the GT911 latches I2C address 0x5D (which is what
   * ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG expects). Without this explicit
   * output config, the pin floats in input mode, an external pull-up
   * dominates, and the chip lands at 0x14 → driver NAKs and "touch not
   * working" looks intermittent because address latch happens every
   * reset. Observed directly via the 0x5D=NAK, 0x14=ACK probe lines. */
  gpio_config_t int_cfg = {
      .pin_bit_mask = (1ULL << GPIO_INPUT_IO_4),
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&int_cfg);

  /* Hardware reset sequence for the GT911 touch controller.
   *
   * The GT911 is pin-mux'd through the CH422G I2C expander at 0x38.
   * Writing 0x2C drives the expander output so RST goes LOW (chip in
   * reset); writing 0x2E releases RST. INT is driven LOW directly
   * through GPIO4 during the RST-release edge — the GT911 samples INT
   * at that moment to latch its I2C address (INT low → 0x5D, which is
   * what ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG expects by default).
   *
   * Timing per the GT911 datasheet:
   *   - Hold RST low ≥ 100 μs (we use 10 ms for margin)
   *   - Keep INT state stable ≥ 55 ms AFTER RST release before any I2C
   *   - After that 55 ms window, INT can become an input again
   *
   * Moved into a helper so the retry path can re-run the full sequence
   * on failure — just retrying I2C can't un-stick a GT911 that missed
   * its boot-window, only a fresh reset can. */
  static const int GT911_POST_RST_DELAY_MS = 80; /* datasheet min 55 ms */
#define GT911_RESET_SEQ()                                                      \
  do {                                                                         \
    uint8_t _b;                                                                \
    _b = 0x2C; /* assert RST low via CH422G */                                 \
    i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &_b, 1,                   \
                               I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);    \
    vTaskDelay(pdMS_TO_TICKS(10));                                             \
    gpio_set_level(GPIO_INPUT_IO_4, 0); /* INT low → addr 0x5D */            \
    vTaskDelay(pdMS_TO_TICKS(10));                                             \
    _b = 0x2E; /* release RST */                                               \
    i2c_master_write_to_device(I2C_MASTER_NUM, 0x38, &_b, 1,                   \
                               I2C_MASTER_TIMEOUT_MS / portTICK_PERIOD_MS);    \
    vTaskDelay(pdMS_TO_TICKS(GT911_POST_RST_DELAY_MS));                        \
  } while (0)

  GT911_RESET_SEQ();

  esp_lcd_touch_handle_t tp = NULL;
  esp_lcd_panel_io_handle_t tp_io_handle = NULL;

  ESP_LOGI(TAG, "Initialize I2C");

  esp_lcd_panel_io_i2c_config_t tp_io_config =
      ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();

  ESP_LOGI(TAG, "Initialize touch IO (I2C)");
  /* Touch IO handle */
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(
      (esp_lcd_i2c_bus_handle_t)I2C_MASTER_NUM, &tp_io_config, &tp_io_handle));
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
  /* Diagnostic: ACK-probe the two possible GT911 addresses before every
   * init attempt. Tells us instantly whether the chip answered at all —
   * if neither address ACKs, it's a chip-state issue (stuck, wrong
   * reset sequence, power ramp); if one ACKs but init still fails, the
   * chip is present but the driver's config read is timing out. */
  ESP_LOGI(TAG, "Initialize touch controller GT911");
  esp_err_t touch_ret = ESP_FAIL;
  for (int attempt = 1; attempt <= 3; ++attempt) {
    uint8_t probe_byte = 0;
    esp_err_t ack_5d = i2c_master_read_from_device(
        I2C_MASTER_NUM, 0x5D, &probe_byte, 1, 50 / portTICK_PERIOD_MS);
    esp_err_t ack_14 = i2c_master_read_from_device(
        I2C_MASTER_NUM, 0x14, &probe_byte, 1, 50 / portTICK_PERIOD_MS);
    /* Kept at DEBUG level so production logs stay clean; flip to INFO
     * (or raise the global log level) if a field unit fails this step
     * and we need the address-latch evidence again. */
    ESP_LOGD(TAG, "GT911 probe attempt %d: 0x5D=%s, 0x14=%s", attempt,
             ack_5d == ESP_OK ? "ACK" : "NAK",
             ack_14 == ESP_OK ? "ACK" : "NAK");

    touch_ret = esp_lcd_touch_new_i2c_gt911(tp_io_handle, &tp_cfg, &tp);
    if (touch_ret == ESP_OK) {
      if (attempt > 1)
        ESP_LOGI(TAG, "Touch init succeeded on attempt %d", attempt);
      break;
    }
    ESP_LOGW(TAG,
             "Touch init attempt %d failed (0x%x), "
             "re-pulsing reset and retrying...",
             attempt, touch_ret);
    /* Re-pulse RST before the next attempt — pure I2C retries can't
     * recover a GT911 that missed its boot window, only a fresh reset
     * can. This is the key difference from the old retry loop. */
    GT911_RESET_SEQ();
  }
  if (touch_ret != ESP_OK) {
    ESP_LOGE(TAG,
             "Touch controller GT911 initialization failed (0x%x) after 3 "
             "attempts, "
             "continuing without touch...",
             touch_ret);
    tp = NULL; // Set to NULL to indicate no touch available
  } else {
    ESP_LOGI(TAG, "Touch controller GT911 initialized successfully");
  }

  /* Address latch is done — reconfigure GPIO4 as high-impedance input so
   * we don't keep pulling INT low and blocking the GT911 from signalling
   * touch events (the chip drives INT low when it has data). */
  gpio_config_t int_hi_z = {
      .pin_bit_mask = (1ULL << GPIO_INPUT_IO_4),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&int_hi_z);

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
    buf_size =
        (EXAMPLE_LCD_H_RES * (EXAMPLE_LCD_V_RES / 4) * sizeof(lv_color_t));
    buf_size = (buf_size + 31) & ~31;
    const size_t min_buf_size = (EXAMPLE_LCD_H_RES * 30 * sizeof(lv_color_t));

    while (buf_size >= min_buf_size) {
      buf1 = heap_caps_aligned_alloc(32, buf_size, MALLOC_CAP_SPIRAM);
      if (buf1) {
        buf2 = heap_caps_aligned_alloc(32, buf_size, MALLOC_CAP_SPIRAM);
        if (buf2)
          break;
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
  /* Capture reads directly from the panel's own framebuffer — see the
   * comment above rdm_lvgl_flush_cb. No separate shadow allocation
   * needed; the panel FB pointer is cached lazily on the first flush. */

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = EXAMPLE_LCD_H_RES;
  disp_drv.ver_res = EXAMPLE_LCD_V_RES;
  disp_drv.flush_cb = rdm_lvgl_flush_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.user_data = panel_handle;
  disp_drv.monitor_cb = rdm7_lvgl_monitor_cb;
#if CONFIG_EXAMPLE_DOUBLE_FB
  disp_drv.full_refresh =
      false; // the full_refresh mode can maintain the synchronization between
             // the two frame buffers
#endif
  lv_disp_t *disp = lv_disp_drv_register(&disp_drv);

  // Set display background to black immediately to prevent white flicker
  lv_disp_set_bg_color(disp, THEME_COLOR_BG);
  ESP_LOGI(TAG, "Display background set to black to prevent white flicker");

  /* NOTE: Display rotation at boot is intentionally disabled for now.
   *
   * Calling lv_disp_set_rotation() on the RGB panel configuration used here
   * causes esp_cache_msync / rgb_panel_draw_bitmap failures ("wrong size,
   * total size overflow") because LVGL's software rotation path requires
   * disp_drv.sw_rotate = 1 AND a full-frame buffer pair in PSRAM — our
   * current buffers are sized smaller to fit memory.
   *
   * Re-enabling rotation requires:
   *   1. disp_drv.sw_rotate = 1 on the disp_drv before lv_disp_drv_register
   *   2. Both framebuffers sized (H_RES * V_RES * sizeof(lv_color_t))
   *   3. Verification on-device that no cache alignment faults appear
   *
   * Until then the rotation persistence layer
   * (config_store_save/load_rotation) stays in place, but the UI button is
   * hidden. See #23. */
  {
    uint8_t saved_rot = 0;
    (void)config_store_load_rotation(&saved_rot);
    if (saved_rot > 0 && saved_rot <= 3) {
      ESP_LOGW(TAG,
               "Display rotation %u deg saved but NOT applied — "
               "rotation temporarily disabled, see comment at %s:%d",
               (unsigned)(saved_rot * 90), __FILE__, __LINE__);
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
    indev_drv.read_cb = rdm_lvgl_touch_cb;
    indev_drv.user_data = tp;

    lv_indev_drv_register(&indev_drv);
    ESP_LOGI(TAG, "Touch input device registered successfully");
  } else {
    ESP_LOGW(TAG,
             "Touch controller not available, continuing without touch input");
  }

  /* Virtual input device for remote touch over the web UI (Live Control).
   *
   * NOTE: We deliberately DO NOT call remote_touch_init(disp) here. Calling
   * lv_indev_drv_register() at boot before the dashboard widgets exist
   * corrupts LVGL state — specifically, the first lv_obj_create() of any
   * panel widget then infinite-loops inside lv_obj_get_screen() walking a
   * cyclic parent chain. Exact mechanism not fully characterised but
   * empirically reproducible: disabling this boot-time registration lets
   * boot complete cleanly; re-enabling it hangs widget creation.
   *
   * Instead, remote_touch_init() is called lazily by the /api/touch POST
   * handler the first time the user toggles CONTROL on in Studio. By then
   * the dashboard is fully built and LVGL is stable, so indev registration
   * succeeds without side-effects. Functionally identical for users — the
   * feature is disabled until explicitly enabled anyway. */
  /* remote_touch_init(disp);  -- deferred, see comment above */
  (void)disp; /* disp is passed to remote_touch_init lazily, stored by web
                 handler */

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
  xTaskCreatePinnedToCore(rdm_lvgl_port_task, "LVGL",
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
           wire_inputs_get_left_gpio(), wire_inputs_get_right_gpio());

  /* Now that the LVGL mutex exists, start the CAN receive task */
  ESP_LOGI(TAG, "Creating CAN task now that LVGL mutex is ready...");
  can_start_task();
  ESP_LOGI(TAG, "CAN task started - data will be available when UI loads");

  ESP_LOGI(TAG, "Loading splash screen for smooth boot experience");

  // Lock the mutex due to the LVGL APIs are not thread-safe
  if (rdm_lvgl_lock(-1)) {
    ui_init(); // This shows the splash screen which will auto-transition to
               // main screen
    rdm_lvgl_unlock();
  }

  /* Start indicator wire task: reads GPIO 43/44 and drives indicators when
   * source is Wire */
  xTaskCreatePinnedToCore(wire_inputs_task, "ind_wire", 2048, NULL, 3, NULL, 0);
  ESP_LOGI(TAG, "Indicator wire task started");

  /* Fuel sender – ADC on GPIO 6, exposed as FUEL_SENDER_V signal */
  fuel_sender_adc_init();

  // Initialize remaining components while splash is showing
  sd_manager_init();
  data_logger_init();
  user_signals_init();
  /* Initialize UART serial protocol (core 0, priority 5).
   * Shares GPIO 43/44 with the ESP-IDF console (UART0) — the board's
   * CH422G I/O expander selects which UART drives the USB bridge.
   *
   * Skipped when wire-input mode is enabled in NVS: GPIO 43/44 are then
   * reserved as indicator wire inputs and must not be driven by UART1 TX
   * (UART TX idles HIGH, overriding the pull-down and permanently reading
   * as active on the indicator circuit).
   *
   * DEBUG: set RDM7_DEBUG_KEEP_CONSOLE to 1 to skip UART1 takeover so boot
   * logs remain visible on the USB-UART bridge past this point. Desktop app
   * cannot connect while this is enabled. Flip back to 0 for production. */
#ifndef RDM7_DEBUG_KEEP_CONSOLE
#define RDM7_DEBUG_KEEP_CONSOLE 1
#endif
#if RDM7_DEBUG_KEEP_CONSOLE
  ESP_LOGW(TAG, "RDM7_DEBUG_KEEP_CONSOLE=1 — skipping uart_protocol_init "
                "so console logs stay on USB. Desktop app will not connect.");
#else
  {
    bool wire_input_mode = false;
    config_store_load_wire_input_mode(&wire_input_mode);
    if (wire_input_mode) {
      ESP_LOGI(TAG, "Wire input mode: GPIO 43/44 reserved for indicators, "
                    "UART1 serial disabled.");
    } else {
      if (uart_protocol_init() != ESP_OK) {
        ESP_LOGE(TAG, "UART protocol init failed!");
      }
    }
  }
#endif

  /* USB CDC disabled — ESP32-S3 USB Serial/JTAG and USB OTG share the
   * same PHY and can't coexist. To enable USB CDC in the future:
   * 1. Disable CONFIG_USJ_ENABLE_USB_SERIAL_JTAG in sdkconfig
   * 2. Disable CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG
   * 3. Uncomment usb_cdc_protocol_init() below
   */
  // if (usb_cdc_protocol_init() != ESP_OK) {
  // 	ESP_LOGW(TAG, "USB CDC protocol init failed");
  // }

  /* Initialize WiFi manager (creates netif, no radio start yet) */
  wifi_manager_init();
  wifi_ui_init();

  /* mDNS was removed 2026-04-27 (see ADR 0001 wifi-onboarding-reliability).
     The managed component pinned MDNS_MEMORY_ALLOC_INTERNAL, which couldn't
     allocate from the fragmented internal RAM pool after WiFi init.
     Users reach the dash via IP (shown in Device Settings) or QR code. */

  /* Check if WiFi should start on boot */
  wifi_boot_config_t boot_cfg;
  config_store_load_wifi_boot(&boot_cfg);

  /* First-run setup (#17): on a fresh device (no first_run_done flag in NVS),
     auto-enable WiFi (STA) on boot so the user can join their home network
     from the on-screen wizard. Hotspot is NOT enabled by default — the user
     opts in via the wizard's "Start Hotspot" button (or the WiFi screen)
     if they don't have WiFi available. The on-screen wizard (shown from
     splash_screen.c after the dashboard loads) presents CAN-scan and
     Wi-Fi/Hotspot buttons; dismissing the wizard is what sets
     first_run_done = true. */
  bool first_run_done = false;
  config_store_load_first_run_done(&first_run_done);
  if (!first_run_done) {
    ESP_LOGW(TAG, "First run detected — enabling Wi-Fi (STA) for onboarding");
    boot_cfg.wifi_on_boot = true;
    boot_cfg.ap_enabled = false;
    (void)config_store_save_wifi_boot(&boot_cfg);
    /* Do NOT mark first_run_done here — the wizard does that on dismissal
     */
  }

  if (boot_cfg.wifi_on_boot) {
    ESP_LOGI(TAG, "WiFi-on-boot enabled, starting WiFi after 4s delay...");
    /* Start WiFi with a 4-second delay to let dashboard load first */
    lv_timer_t *wifi_boot_timer =
        lv_timer_create(_deferred_wifi_boot_cb, 4000, NULL);
    lv_timer_set_repeat_count(wifi_boot_timer, 1);
  } else {
    ESP_LOGI(TAG, "WiFi disabled at boot — enable from Settings > Wi-Fi");
  }

  ESP_LOGI(TAG, "All systems initialized - splash screen will transition to "
                "main screen automatically");
}
