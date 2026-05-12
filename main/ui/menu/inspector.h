/**
 * inspector.h — Per-widget Inspector overlay (Phase 3 shell).
 *
 * Opens when the user taps Configure on the bottom toolbar. Lives as a
 * translucent overlay on top of ui_Screen3 — the dashboard remains visible
 * behind a dimmed backdrop, so changes are seen in real time as the user
 * adjusts settings in the Inspector.
 *
 * Three tabs: DATA / STYLE / RULES. (Position + size are handled directly
 * by the bottom toolbar's chip popover, so LAYOUT isn't an Inspector tab.)
 *
 * Phase 3.1 wires up the shell + tab switching. Each tab is a placeholder
 * with an "Open legacy editor" fallback so widgets that relied on the old
 * config_modal (Panel/Bar/RPM_Bar) don't regress while later phases fill
 * in schema-driven content.
 */
#pragma once
#include "widgets/widget_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Open the Inspector overlay for @p w. No-op if already open or @p w is
 *  NULL. The dashboard's ui_Screen3 remains the active screen — the overlay
 *  is a child of it. */
void inspector_open(widget_t *w);

/** Close the Inspector and tear down the overlay. No-op if not open. */
void inspector_close(void);

/** True iff the Inspector overlay is currently up. */
bool inspector_is_open(void);

#ifdef __cplusplus
}
#endif
