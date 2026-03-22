#include "wire_inputs.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_helpers.h"

/* indicator_apply_analog_state() is declared in widget_indicator.h and
 * also in ui_Screen3.h.  It internally checks each indicator's
 * input_source, so we always call it and let it skip CAN-mode channels. */
extern void indicator_apply_analog_state(bool left_on, bool right_on);

void wire_inputs_init(void)
{
    /* Guard: skip GPIO init when pins are disabled (-1) */
    if (WIRE_INPUT_LEFT_GPIO < 0 || WIRE_INPUT_RIGHT_GPIO < 0) {
        return;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask  = (1ULL << WIRE_INPUT_LEFT_GPIO) | (1ULL << WIRE_INPUT_RIGHT_GPIO),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_ENABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    /* Brief stabilisation delay so the pull-down settles before first read */
    vTaskDelay(pdMS_TO_TICKS(10));
}

void wire_inputs_task(void *pvParam)
{
    (void)pvParam;
    /* Allow GPIO to stabilise and the UI to initialise before first sample */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Guard: skip polling when pins are disabled (-1) */
    if (WIRE_INPUT_LEFT_GPIO < 0 || WIRE_INPUT_RIGHT_GPIO < 0) {
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        bool left_on  = (gpio_get_level(WIRE_INPUT_LEFT_GPIO)  == 1);
        bool right_on = (gpio_get_level(WIRE_INPUT_RIGHT_GPIO) == 1);

        if (example_lvgl_lock(20)) {
            indicator_apply_analog_state(left_on, right_on);
            example_lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(50)); /* 20 Hz poll rate */
    }
}
