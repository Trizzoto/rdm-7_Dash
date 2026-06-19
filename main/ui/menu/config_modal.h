#pragma once
#include "lvgl.h"
#include "widgets/widget_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Open the "Widget settings" (quick-settings) modal for a widget.
 *
 * Post-channels-migration this holds only per-widget VISUAL config plus an
 * optional Label override — data source, CAN decode, range, decimals and
 * thresholds all live on the channel (edit via the channels editor). Tabs
 * are widget-type dependent: LABEL (panel/bar), ALERTS (panel/bar),
 * RPM SETTINGS (rpm_bar), INDICATOR (indicator), ALERT (warning).
 *
 * The modal modifies live type_data fields directly. Save/Cancel footer
 * buttons delegate to menu_screen.c callbacks.
 *
 * @param screen  The screen object that owns the modal (ui_MenuScreen).
 * @param w       The widget to configure.
 */
void config_modal_open_for_widget(lv_obj_t *screen, widget_t *w);

/**
 * Whether this widget has any quick-settings content worth opening the
 * modal for. False for meter / text / arc (no label, no visual tab) —
 * their config is entirely channel-side, so callers (e.g. the channels
 * editor's "Widget settings" button) should hide the entry point.
 */
bool config_modal_has_content(widget_t *w);

#ifdef __cplusplus
}
#endif
