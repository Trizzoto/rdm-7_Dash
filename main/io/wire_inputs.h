#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** GPIO number for the left turn-indicator wire input (high = active).
 *  Set to -1 to disable — GPIO 43 is shared with UART1 TX (desktop serial
 *  protocol). Re-enabling requires free GPIO pins or a CH422G expander line,
 *  both pending a hardware revision. See #24 in launch-freeze docs. */
#define WIRE_INPUT_LEFT_GPIO                                                   \
  43 // 43 — reserved for UART1 TX, do not enable without HW rework

/** GPIO number for the right turn-indicator wire input (high = active).
 *  Shared with UART1 RX (desktop serial protocol). See note above. */
#define WIRE_INPUT_RIGHT_GPIO 44 // 44 — reserved for UART1 RX

/**
 * @brief Configure GPIO 43 and 44 as digital inputs with pull-down enabled.
 *        Call once during hardware initialisation, before the LVGL task starts.
 */
void wire_inputs_init(void);

/**
 * @brief FreeRTOS task that polls the indicator wire inputs at 20 Hz and
 *        forwards state changes to the LVGL indicator widgets.
 *
 *        Pins are only sampled when at least one indicator channel has its
 *        input_source set to Wire (0).  Pin state is forwarded inside the LVGL
 *        mutex so UI updates are thread-safe.
 *
 *        Recommended creation:
 *          xTaskCreatePinnedToCore(wire_inputs_task, "ind_wire",
 *                                  2048, NULL, 3, NULL, 0);
 */
void wire_inputs_task(void *pvParam);

#ifdef __cplusplus
}
#endif
