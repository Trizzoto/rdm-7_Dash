/*
 * widget_track_map.h — draws a circuit's outline, with the car on it.
 *
 * The geometry lives in a NAMED DEVICE ASSET (an .rdmtrk in LittleFS, see
 * track_map_geo.h), not in the layout JSON. That is what makes pushing a new
 * track one action instead of re-pushing a whole dash design: the widget holds
 * a name, the asset holds the shape, and swapping circuits is a one-field edit
 * or a single upload.
 *
 * All parsing and projection is in track_map_geo.c, which is LVGL-free and
 * host-testable. What is left in here is LVGL: allocation, the draw callback,
 * and the live position marker.
 */
#ifndef WIDGET_TRACK_MAP_H
#define WIDGET_TRACK_MAP_H

#include "widgets/widget_types.h"

#ifdef __cplusplus
extern "C" {
#endif

widget_t *widget_track_map_create_instance(uint8_t slot);

#ifdef __cplusplus
}
#endif
#endif /* WIDGET_TRACK_MAP_H */
