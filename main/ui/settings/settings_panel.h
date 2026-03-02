#pragma once

#include "lvgl.h"
#include "../theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * settings_panel — reusable settings-UI builder for LVGL v8 / ESP32-S3
 *
 * Usage pattern:
 *   settings_panel_t *sp = settings_panel_create(screen, x, y, w, h);
 *   settings_section_t *sec = settings_add_section(sp, "TITLE", THEME_COLOR_ACCENT_BLUE);
 *   lv_obj_t *dd = settings_add_dropdown(sec, "Bitrate:", "125k\n250k\n500k", 140);
 *   lv_dropdown_set_selected(dd, saved_idx);
 *   lv_obj_add_event_cb(dd, my_cb, LV_EVENT_VALUE_CHANGED, NULL);
 */

/** Scrollable column container — add sections to it */
typedef lv_obj_t settings_panel_t;

/** Section card (flex-column) — add rows to it */
typedef lv_obj_t settings_section_t;

/** Tab bar returned by settings_add_tabs */
typedef lv_obj_t settings_tabs_t;

/* =========================================================================
 * Panel and section
 * ========================================================================= */

/**
 * Create a scrollable settings panel at absolute position (x,y) with size (w,h).
 * Uses dark surface background + thin border.
 */
settings_panel_t *settings_panel_create(lv_obj_t *parent,
                                        lv_coord_t x, lv_coord_t y,
                                        lv_coord_t w, lv_coord_t h);

/**
 * Add a titled card to a panel (flex-column, sections stack vertically).
 * @param accent  Colour for the section title label.
 * Returns the section — pass to settings_add_* row builders.
 */
settings_section_t *settings_add_section(settings_panel_t *panel,
                                         const char *title,
                                         lv_color_t accent);

/**
 * Add an lv_tabview to the panel.
 * @param tab_names  NULL-terminated array of tab name strings.
 * Returns the tabview object — use settings_get_tab() to get per-tab containers.
 */
settings_tabs_t *settings_add_tabs(settings_panel_t *panel,
                                   const char * const *tab_names,
                                   uint8_t n_tabs,
                                   lv_coord_t h);

/**
 * Return the content area of tab[idx] from a tabview created by settings_add_tabs.
 * Use this as the 'panel' argument for settings_add_section inside a tab.
 */
lv_obj_t *settings_get_tab(settings_tabs_t *tabs, uint8_t idx);

/* =========================================================================
 * Row builders — each returns the interactive control widget.
 * Attach callbacks and set initial state on the returned handle.
 * ========================================================================= */

/** Single-line text input */
lv_obj_t *settings_add_text_input(settings_section_t *sec,
                                   const char *label,
                                   const char *placeholder,
                                   const char *initial_text);

/** Single-line numeric/hex input (visually identical to text input) */
lv_obj_t *settings_add_number_input(settings_section_t *sec,
                                     const char *label,
                                     const char *placeholder,
                                     const char *initial_text);

/** Dropdown list.  ctrl_w <= 0 → flex-grow to fill row */
lv_obj_t *settings_add_dropdown(settings_section_t *sec,
                                 const char *label,
                                 const char *options,
                                 lv_coord_t ctrl_w);

/** On/off toggle switch */
lv_obj_t *settings_add_switch(settings_section_t *sec,
                               const char *label,
                               bool checked);

/** Roller (vertical scroll picker) */
lv_obj_t *settings_add_roller(settings_section_t *sec,
                               const char *label,
                               const char *options,
                               uint8_t visible_rows);

/** Colour-swatch dropdown (same as dropdown, semantic alias) */
lv_obj_t *settings_add_color_swatch(settings_section_t *sec,
                                     const char *label,
                                     const char *options,
                                     lv_coord_t ctrl_w);

/** Read-only key/value info row.  Returns the value label for later updates. */
lv_obj_t *settings_add_info_row(settings_section_t *sec,
                                 const char *key,
                                 const char *value_text);

/** Full-width action button.  h <= 0 → default 34 px */
lv_obj_t *settings_add_button(settings_section_t *sec,
                               const char *text,
                               lv_color_t bg_color,
                               lv_coord_t h);

#ifdef __cplusplus
}
#endif
