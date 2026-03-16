#ifndef MENU_SCREEN_H
#define MENU_SCREEN_H

#include "lvgl.h"
#include "widgets/widget_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void load_menu_screen_for_widget(widget_t *w);
void close_menu_event_cb(lv_event_t * e);
void cancel_menu_event_cb(lv_event_t * e);

/* Global previewer references (used by widget update callbacks for
 * live-updating preview objects while the config menu is open) */
extern lv_obj_t * menu_panel_value_labels[8];
extern lv_obj_t * menu_panel_boxes[8];
extern lv_obj_t * menu_panel_labels[8];
extern lv_obj_t * menu_bar_objects[2];
extern lv_obj_t * menu_bar_labels[2];

#ifdef __cplusplus
}
#endif

#endif // MENU_SCREEN_H
