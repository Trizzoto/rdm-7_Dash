#pragma once
#include "lvgl.h"
#include "widgets/widget_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Open the tabbed configuration modal for a widget.
 *
 * Tab 1: Signal  -- CAN ID, endian, bit start/length, scale, offset, signed
 * Tab 2: Alerts  -- only shown if widget_has_alert_support(w) is true
 *
 * The modal modifies live signal_t and type_data fields directly.
 * Save/Cancel footer buttons delegate to menu_screen.c callbacks.
 *
 * @param screen  The screen object that owns the modal (ui_MenuScreen).
 * @param w       The widget to configure.
 */
void config_modal_open_for_widget(lv_obj_t *screen, widget_t *w);

#ifdef __cplusplus
}
#endif
