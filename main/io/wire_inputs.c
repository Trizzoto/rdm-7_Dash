#include "wire_inputs.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl_helpers.h"
#include "net/uart_protocol.h"
#include "storage/config_store.h"

static const char *TAG = "wire_inputs";

/* The indicator circuit and the USB-UART chip share the ESP32-S3's UART pads.
 * A slide switch on the board picks which one is joined to them:
 *   UART1: GPIO 43 = TX to the USB-UART chip, GPIO 44 = RX from it.
 *   UART2: GPIO 43 = left indicator input,     GPIO 44 = right indicator input. */
#define WIRE_INPUT_LEFT_PIN_ACTIVE   43
#define WIRE_INPUT_RIGHT_PIN_ACTIVE  44

/* How the switch position is read, from GPIO 44 alone (it is the RX input in
 * both modes, so it can always be sampled):
 *   - The USB-UART chip's TXD idles HIGH and only dips during a byte, so on
 *     UART1 the pin is never low for long.
 *   - The indicator input sits LOW with the indicator off, and a flasher is
 *     only ever high for well under a second.
 * Low for LOW_TO_INDICATOR_MS => the switch is on UART2. High for
 * HIGH_TO_USB_MS => back on UART1. The long high window is what keeps a
 * slow flasher, or hazards, from reading as USB. */
#define POLL_MS                 50
#define LOW_TO_INDICATOR_MS   1000
#define HIGH_TO_USB_MS        5000

static bool s_wire_mode = false;   /* true = pads belong to the indicator circuit */
static bool s_uart_ready = false;  /* UART driver installed; its pins may be re-taken */

/* indicator_apply_analog_state() is declared in widget_indicator.h and
 * also in ui_Screen3.h.  It internally checks each indicator's
 * input_source, so we always call it and let it skip CAN-mode channels. */
extern void indicator_apply_analog_state(bool left_on, bool right_on);

int wire_inputs_get_left_gpio(void)  { return s_wire_mode ? WIRE_INPUT_LEFT_PIN_ACTIVE : -1; }
int wire_inputs_get_right_gpio(void) { return s_wire_mode ? WIRE_INPUT_RIGHT_PIN_ACTIVE : -1; }
bool wire_inputs_mode_active(void)   { return s_wire_mode; }
void wire_inputs_uart_ready(void)    { s_uart_ready = true; }

/* Boot guess, so indicators work from the first frame rather than after the
 * LOW_TO_INDICATOR_MS window: high the whole 600 ms = UART1, any low = UART2. */
static bool _probe_switch_is_uart2(void)
{
    gpio_pullup_dis(WIRE_INPUT_RIGHT_PIN_ACTIVE);
    gpio_pulldown_en(WIRE_INPUT_RIGHT_PIN_ACTIVE);   /* pad is the RX input at boot: input already on */
    vTaskDelay(pdMS_TO_TICKS(20));

    int high = 0;
    const int samples = 12;
    for (int i = 0; i < samples; i++) {
        high += gpio_get_level(WIRE_INPUT_RIGHT_PIN_ACTIVE);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "UART switch probe: GPIO %d high %d/%d -> %s",
             WIRE_INPUT_RIGHT_PIN_ACTIVE, high, samples,
             high == samples ? "UART1 (USB)" : "UART2 (indicator circuit)");
    return high != samples;
}

/* Hand GPIO 43 to the indicator circuit: detach UART TX from the pad so the
 * dash stops driving a line the indicator circuit is driving. RX (44) stays
 * routed to the UART; it is read with gpio_get_level() either way. */
static void _enter_indicator_mode(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << WIRE_INPUT_LEFT_PIN_ACTIVE,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_pullup_dis(WIRE_INPUT_RIGHT_PIN_ACTIVE);
    gpio_pulldown_en(WIRE_INPUT_RIGHT_PIN_ACTIVE);
    s_wire_mode = true;
    ESP_LOGI(TAG, "UART switch on UART2: GPIO 43/44 read the indicator circuit");
}

/* Give GPIO 43 back to UART TX for the USB link. */
static void _enter_usb_mode(void)
{
    if (s_uart_ready) {
        uart_set_pin(UART_PROTO_PORT_NUM, UART_PROTO_TX_PIN, UART_PROTO_RX_PIN,
                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        gpio_pullup_dis(UART_PROTO_TX_PIN);
        gpio_pulldown_en(UART_PROTO_TX_PIN);
        gpio_pullup_dis(UART_PROTO_RX_PIN);
        gpio_pulldown_en(UART_PROTO_RX_PIN);
    }
    s_wire_mode = false;
    ESP_LOGI(TAG, "UART switch on UART1: GPIO 43/44 carry USB");
}

void wire_inputs_init(void)
{
    bool forced = false;
    config_store_load_wire_input_mode(&forced);
    /* Only the guess is taken here; the pads are handed over by the task once
     * uart_protocol_init() has run, or UART's own uart_set_pin() would take
     * GPIO 43 straight back. */
    s_wire_mode = forced || _probe_switch_is_uart2();
}

void wire_inputs_task(void *pvParam)
{
    (void)pvParam;

    /* Wait for the UART driver (main.c signals it), then apply the boot
     * guess. If the UART never comes up, carry on after 5 s anyway so the
     * indicators still work. */
    for (int waited = 0; !s_uart_ready && waited < 5000; waited += POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    if (s_wire_mode) _enter_indicator_mode();
    else ESP_LOGI(TAG, "UART switch on UART1 at boot: USB active");

    /* Report the deepest stack use once the paint path has actually run a few
     * times. indicator_apply_analog_state() rasterises a polygon into an
     * lv_canvas, and LVGL v8 keeps the draw descriptor and its blend scratch
     * on the CALLER's stack — that overflowed this task's original 2 KB and
     * panicked the device. The number below is the evidence for the 8 KB it
     * is created with now; if a future indicator change pushes it toward
     * zero, this is where it will show up before it becomes a boot loop. */
    int paints = 0;
    int low_ms = 0, high_ms = 0;

    for (;;) {
        bool right = gpio_get_level(WIRE_INPUT_RIGHT_PIN_ACTIVE) == 1;
        if (right) { high_ms += POLL_MS; low_ms = 0; }
        else       { low_ms  += POLL_MS; high_ms = 0; }

        if (!s_wire_mode) {
            /* USB mode: the switch has moved to UART2 once GPIO 44 goes quiet low. */
            if (low_ms >= LOW_TO_INDICATOR_MS) _enter_indicator_mode();
        } else if (high_ms >= HIGH_TO_USB_MS) {
            /* Indicator mode: held high far longer than any flasher = UART1. */
            if (rdm_lvgl_lock(20)) {
                indicator_apply_analog_state(false, false);
                rdm_lvgl_unlock();
            }
            _enter_usb_mode();
        }

        if (s_wire_mode) {
            bool left = gpio_get_level(WIRE_INPUT_LEFT_PIN_ACTIVE) == 1;
            if (rdm_lvgl_lock(20)) {
                indicator_apply_analog_state(left, right);
                rdm_lvgl_unlock();

                if (paints < 5 && ++paints == 5) {
                    ESP_LOGI(TAG, "indicator paint stack headroom: %u bytes free "
                                  "of 8192", (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
