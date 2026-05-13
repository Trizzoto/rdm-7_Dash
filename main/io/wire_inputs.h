#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Indicator wire inputs share GPIO 43/44 with UART1 (desktop serial). They
 * cannot both be active at the same time, so the assignment is gated by the
 * NVS "wire_input_mode" flag set from Device Settings:
 *
 *   wire_input_mode = false (default): UART1 owns GPIO 43/44, wire inputs
 *                                      resolve to -1 (disabled at init).
 *   wire_input_mode = true:            wire inputs claim GPIO 43/44, UART1
 *                                      init is skipped in main.c. Requires
 *                                      reboot to take effect either direction.
 *
 * wire_inputs_init() reads the flag at boot and stores the resolved pin
 * numbers in module-local state; the getters below are provided so other
 * modules (currently just main.c for the boot log) can report them.
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

/* Pin getters — return the resolved GPIO number (>=0) or -1 if wire-input
 * mode is disabled. Safe to call before wire_inputs_init(); both return -1
 * until init runs. */
int wire_inputs_get_left_gpio(void);
int wire_inputs_get_right_gpio(void);

#ifdef __cplusplus
}
#endif
