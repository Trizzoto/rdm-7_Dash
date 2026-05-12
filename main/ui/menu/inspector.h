/**
 * inspector.h — Per-widget Inspector modal (Phase 3 shell).
 *
 * Full-screen 800×480 modal that opens when the user taps Configure on the
 * bottom toolbar. Header bar, tab bar (DATA / STYLE / LAYOUT / RULES), and
 * a scrollable content area.
 *
 * Phase 3.1 (this commit):
 *   - Shell + tab bar + tab switching.
 *   - LAYOUT tab functional (x/y/w/h sliders, live preview on apply).
 *   - DATA / STYLE / RULES placeholder with "Open legacy editor" fallback
 *     so widgets that relied on load_menu_screen_for_widget don't regress.
 *
 * Later phases fill in the placeholder tabs with schema-driven content
 * (see the master plan in docs / earlier conversation turns).
 */
#pragma once
#include "widgets/widget_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Open the Inspector for @p w. No-op if already open or @p w is NULL.
 *  Loads its own LVGL screen — the dashboard becomes inactive but stays in
 *  memory (widget signals keep firing, edit-mode state persists). */
void inspector_open(widget_t *w);

/** Close the Inspector. lv_scr_load's ui_Screen3 and deletes the Inspector
 *  screen. No-op if not open. */
void inspector_close(void);

/** True iff the Inspector is currently the active screen. */
bool inspector_is_open(void);

#ifdef __cplusplus
}
#endif
