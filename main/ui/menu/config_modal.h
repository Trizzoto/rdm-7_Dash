#pragma once
#include "lvgl.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Open the single centred, tabbed configuration modal for value_id.
 * Covers panels (1-8) and bars (12-13).
 * Creates its own keyboard, Save and Cancel footer buttons.
 *
 * @param screen   The screen object that owns the modal (ui_MenuScreen).
 * @param value_id 1-13.
 */
void config_modal_open(lv_obj_t *screen, uint8_t value_id);

#ifdef __cplusplus
}
#endif
