#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Indicator circuit inputs share GPIO 43/44 with UART1 (the USB link to RDM
 * Studio). A PHYSICAL slide switch on the board joins the pads to one or the
 * other: UART1 = the USB-UART port, UART2 = the indicator circuit header.
 *
 * The firmware follows the switch live, no restart needed:
 *   - wire_inputs_init() takes a 600 ms boot guess from GPIO 44.
 *   - wire_inputs_task() keeps watching GPIO 44. Low for 1 s = UART2: GPIO 43
 *     is detached from UART TX and both pads read the indicators. High for
 *     5 s = UART1: GPIO 43 goes back to UART TX.
 * The UART protocol stays installed throughout (main.c); only the TX pad
 * routing changes. The NVS "wire_input_mode" flag, when true, only forces the
 * boot guess to UART2.
 */
void wire_inputs_init(void);

/** Call once uart_protocol_init() has run, so the task can re-route its pins. */
void wire_inputs_uart_ready(void);

/** True while GPIO 43/44 belong to the indicator circuit (switch on UART2). */
bool wire_inputs_mode_active(void);

/**
 * @brief FreeRTOS task: follows the UART switch and, while it is on UART2,
 *        polls the indicator inputs at 20 Hz and forwards them to the LVGL
 *        indicator widgets (only those whose input_source is Wire).
 *
 *        Recommended creation:
 *          xTaskCreatePinnedToCore(wire_inputs_task, "ind_wire",
 *                                  8192, NULL, 3, NULL, 0);
 */
void wire_inputs_task(void *pvParam);

/* Pin getters — the GPIO number while in indicator mode, -1 otherwise. */
int wire_inputs_get_left_gpio(void);
int wire_inputs_get_right_gpio(void);

#ifdef __cplusplus
}
#endif
