/*
 * ui_graphs.h — Live graphs: up to four channels drawn as they arrive
 * (ADR-0076).
 *
 * Built for watching two things against each other while driving or tuning —
 * lambda and its target, boost and its target — so channels that share a unit
 * share an axis, and a target is drawn dashed over the value it chases.
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Open the graphs screen from a menu screen (it replaces that screen; Back
 *  returns to the launcher). The picked channels are remembered until the
 *  dash restarts. */
void graphs_ui_show(void);

#ifdef __cplusplus
}
#endif
