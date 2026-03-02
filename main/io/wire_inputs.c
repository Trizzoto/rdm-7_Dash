#include "wire_inputs.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_helpers.h"
#include "ui/screens/ui_Screen3.h"   /* indicator_configs[], indicator_apply_analog_state() */

void wire_inputs_init(void)
{
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

    for (;;) {
        /* Only drive wire-mode indicators; CAN-mode channels are handled by
           the CAN receive task via indicator_apply_analog_state(). */
        if (indicator_configs[0].input_source == 0 ||
            indicator_configs[1].input_source == 0) {

            bool left_on  = (gpio_get_level(WIRE_INPUT_LEFT_GPIO)  == 1);
            bool right_on = (gpio_get_level(WIRE_INPUT_RIGHT_GPIO) == 1);

            if (example_lvgl_lock(pdMS_TO_TICKS(20))) {
                indicator_apply_analog_state(left_on, right_on);
                example_lvgl_unlock();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50)); /* 20 Hz poll rate */
    }
}
