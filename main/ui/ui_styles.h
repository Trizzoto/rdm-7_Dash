/**
 * ui_styles.h — Shared lv_style_t objects for the RDM-7 dashboard.
 *
 * Call ui_styles_init() once during boot (before any screen is created).
 * Apply styles to objects with lv_obj_add_style(obj, &ui_style_xxx, selector).
 *
 * Inline lv_obj_set_style_* calls applied after lv_obj_add_style() will
 * override these base styles as expected by LVGL's cascade rules, so dynamic
 * colour updates (e.g. RPM colour changes) continue to work without changes.
 */

#pragma once
#include "lvgl.h"

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/** Initialise all shared styles.  Must be called before any UI is built. */
void ui_styles_init(void);

/* -------------------------------------------------------------------------
 * Text styles
 * ---------------------------------------------------------------------- */

/** White primary text — labels, values, titles. */
extern lv_style_t ui_style_label_primary;

/** 0xCCCCCC muted text — secondary settings labels. */
extern lv_style_t ui_style_label_muted;

/** 0x999999 hint text — metadata / firmware-info labels. */
extern lv_style_t ui_style_label_hint;

/** 0x000000 text for labels on light / mint-green backgrounds. */
extern lv_style_t ui_style_label_on_light;

/* -------------------------------------------------------------------------
 * Background / container styles
 * ---------------------------------------------------------------------- */

/** Pure-black screen background (opacity COVER). */
extern lv_style_t ui_style_screen_bg;

/** 0x1A1A1A dark surface container (opacity COVER). */
extern lv_style_t ui_style_surface;

/** 0x1A1A1A surface + 0x404040 border. */
extern lv_style_t ui_style_surface_bordered;

/** 0x181818 input-area / nested panel background. */
extern lv_style_t ui_style_input_bg;

/** 0x2E2F2E panel / box background (borderless). */
extern lv_style_t ui_style_panel;

/** 0x2E2F2E panel bg + 0x2E2F2E border (dashboard boxes). */
extern lv_style_t ui_style_panel_bordered;

/** 0x262626 section-card bg + 0x404040 border (device-settings cards). */
extern lv_style_t ui_style_section_bg;

/** 0x292C29 inactive / off-state background. */
extern lv_style_t ui_style_inactive;

/* -------------------------------------------------------------------------
 * Button styles
 * ---------------------------------------------------------------------- */

/** Material green (0x4CAF50) save / OK button. */
extern lv_style_t ui_style_btn_save;

/** Material red (0xF44336) cancel / back button. */
extern lv_style_t ui_style_btn_cancel;

/** Danger red (0xFF4444) close (×) button. */
extern lv_style_t ui_style_btn_close;

/** Mint-green (0x40FF80) alternative save button (device-settings). */
extern lv_style_t ui_style_btn_save_alt;

/** WiFi / network action button (0x4080FF blue). */
extern lv_style_t ui_style_btn_connect;

/** Neutral gray cancel button (0x666666) used in OTA / WiFi dialogs. */
extern lv_style_t ui_style_btn_gray;

/* -------------------------------------------------------------------------
 * Modal / popup styles
 * ---------------------------------------------------------------------- */

/**
 * Standard popup panel:
 *   bg  = THEME_COLOR_PANEL (0x2E2F2E)
 *   border = THEME_COLOR_BORDER_MED (0x808080)
 *   shadow colour = THEME_COLOR_BG, width = THEME_SHADOW_W_POPUP
 */
extern lv_style_t ui_style_popup;

/* -------------------------------------------------------------------------
 * Control styles
 * ---------------------------------------------------------------------- */

/**
 * Dropdown / spinner control:
 *   bg = 0x333333, text = white, border = 0x555555
 */
extern lv_style_t ui_style_dropdown;

/** Scrollbar track highlight (0x555555). */
extern lv_style_t ui_style_scrollbar;
