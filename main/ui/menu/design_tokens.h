/**
 * design_tokens.h — Editor design system tokens.
 *
 * Single source of truth for the on-device widget editor (Edit Mode banner,
 * pill button, future Inspector). Aliases existing theme.h constants where
 * they match, defines editor-specific extras (soft red, soft green, handle
 * sizes, etc.) where theme.h doesn't have an equivalent.
 *
 * Designed to mirror the web editor's visual language so the on-device
 * experience reads as a sibling, not a stripped-down copy.
 */
#pragma once
#include "ui/theme.h"   /* transitively brings in lvgl.h */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Surfaces ─────────────────────────────────────────────────────────────── */
#define DT_BG_BASE       lv_color_hex(0x303030)
#define DT_BG_PANEL      THEME_COLOR_SECTION_BG       /* 0x393C39 */
#define DT_BG_INSET      lv_color_hex(0x484848)
#define DT_BORDER_DARK   THEME_COLOR_BORDER           /* 0x181C18 */
#define DT_BORDER_LIGHT  lv_color_hex(0x555555)

/* ── Text ─────────────────────────────────────────────────────────────────── */
#define DT_TEXT_PRIMARY  THEME_COLOR_TEXT_PRIMARY     /* 0xE8E8E8 */
#define DT_TEXT_MUTED    lv_color_hex(0x999999)

/* ── Semantic ─────────────────────────────────────────────────────────────── */
#define DT_ACCENT        THEME_COLOR_ACCENT_BLUE      /* 0x2196F3 */
#define DT_ACCENT_HOVER  THEME_COLOR_ACCENT_BLUE_PRESSED /* 0x42A5F5 */
#define DT_DANGER        lv_color_hex(0xF87171)       /* soft red, web parity */
#define DT_SUCCESS       lv_color_hex(0x70E68A)
#define DT_WARNING       lv_color_hex(0xFBBF24)

/* ── Geometry ─────────────────────────────────────────────────────────────── */
#define DT_RADIUS_SM     2
#define DT_RADIUS_MD     THEME_RADIUS_NORMAL          /* 4 */
#define DT_RADIUS_LG     6
#define DT_PAD_SM        6
#define DT_PAD_MD        8
#define DT_PAD_LG       12
#define DT_ROW_H        44                            /* min touch row height */
#define DT_HANDLE_HIT   28                            /* invisible hit pad */
#define DT_HANDLE_VIS   12                            /* visible handle dot */

/* ── Pill button (top-right toolbar) ──────────────────────────────────────── */
#define DT_PILL_W        100   /* Menu pill — "Menu" fits easily */
#define DT_PILL_EDIT_W   140   /* Edit Mode pill — "Exit Edit Mode" needs more room */
#define DT_PILL_H        36
#define DT_PILL_GAP      8     /* gap between pills */

/* ── Editor banner (Edit Mode top strip) ──────────────────────────────────── */
#define DT_BANNER_H     28

#ifdef __cplusplus
}
#endif
