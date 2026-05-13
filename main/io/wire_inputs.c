#include "wire_inputs.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_helpers.h"
#include "storage/config_store.h"

static const char *TAG = "wire_inputs";

/* Default GPIO assignments when wire-input mode is ON. Both pins also happen
 * to be the ESP32-S3 UART1 TX/RX pads, which is why this is mutually
 * exclusive with desktop-serial UART1 (gated in main.c by the same flag). */
#define WIRE_INPUT_LEFT_PIN_ACTIVE   43
#define WIRE_INPUT_RIGHT_PIN_ACTIVE  44

/* Resolved at init() from NVS wire_input_mode. Stay -1 until init runs and
 * also when wire-input mode is off, so wire_inputs_task can early-exit
 * cleanly without touching GPIO it doesn't own. */
static int s_left_gpio  = -1;
static int s_right_gpio = -1;

/* indicator_apply_analog_state() is declared in widget_indicator.h and
 * also in ui_Screen3.h.  It internally checks each indicator's
 * input_source, so we always call it and let it skip CAN-mode channels. */
extern void indicator_apply_analog_state(bool left_on, bool right_on);

int wire_inputs_get_left_gpio(void)  { return s_left_gpio; }
int wire_inputs_get_right_gpio(void) { return s_right_gpio; }

void wire_inputs_init(void)
{
    bool enabled = false;
    config_store_load_wire_input_mode(&enabled);

    if (!enabled) {
        /* Wire-input mode off — leave pins at -1 so the polling task and any
         * future GPIO callers self-disable. UART1 keeps GPIO 43/44. */
        s_left_gpio  = -1;
        s_right_gpio = -1;
        ESP_LOGI(TAG, "Wire-input mode OFF — GPIO 43/44 stay with UART1");
        return;
    }

    s_left_gpio  = WIRE_INPUT_LEFT_PIN_ACTIVE;
    s_right_gpio = WIRE_INPUT_RIGHT_PIN_ACTIVE;

    gpio_config_t io_conf = {
        .pin_bit_mask  = (1ULL << s_left_gpio) | (1ULL << s_right_gpio),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_ENABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    /* Brief stabilisation delay so the pull-down settles before first read */
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "Wire-input mode ON — GPIO %d=left, %d=right",
             s_left_gpio, s_right_gpio);
}

void wire_inputs_task(void *pvParam)
{
    (void)pvParam;
    /* Allow GPIO to stabilise and the UI to initialise before first sample */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Guard: skip polling when pins are disabled (-1) — happens whenever
     * wire-input mode is off. */
    if (s_left_gpio < 0 || s_right_gpio < 0) {
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        bool left_on  = (gpio_get_level(s_left_gpio)  == 1);
        bool right_on = (gpio_get_level(s_right_gpio) == 1);

        if (rdm_lvgl_lock(20)) {
            indicator_apply_analog_state(left_on, right_on);
            rdm_lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(50)); /* 20 Hz poll rate */
    }
}
