/**
 * theme.h — Centralised design tokens for the RDM-7 dashboard.
 *
 * All lv_color_hex() literals, font references and spacing constants used
 * anywhere in the project must be taken from here.  Never hardcode a
 * colour, font pointer or padding value directly in a .c file.
 *
 * Two-step build safety:
 *   1. Include this header in every .c file that touches LVGL styling.
 *   2. Run the compiler — any misspelled constant is a compile-time error.
 */

#pragma once
#include "lvgl.h"

/* =========================================================================
 * COLOURS — backgrounds
 * ========================================================================= */

/** Pure black.  Screen/modal root background. */
#define THEME_COLOR_BG                  lv_color_hex(0x000000)

/** Very dark surface.  Main containers, device-settings root, popups. */
#define THEME_COLOR_SURFACE             lv_color_hex(0x1A1A1A)

/** Slightly lighter dark surface.  Screen4 background. */
#define THEME_COLOR_SURFACE_ALT         lv_color_hex(0x1E1E1E)

/** Near-black.  Input borders, config panel inner areas. */
#define THEME_COLOR_INPUT_BG            lv_color_hex(0x181818)

/** Dark section background.  Settings section cards. */
#define THEME_COLOR_SECTION_BG          lv_color_hex(0x262626)

/** Dark greenish-gray.  Inactive warning circles, outer config panel. */
#define THEME_COLOR_INACTIVE            lv_color_hex(0x292C29)

/** Standard panel / box background.  Dashboard boxes, bar bg, dialogs. */
#define THEME_COLOR_PANEL               lv_color_hex(0x2E2F2E)

/** On-screen keyboard background. */
#define THEME_COLOR_KEYBOARD_BG         lv_color_hex(0x303030)

/** Dropdown / control background. */
#define THEME_COLOR_CONTROL_BG          lv_color_hex(0x333333)

/** Section card border, neutral button background. */
#define THEME_COLOR_BORDER              lv_color_hex(0x404040)

/** Empty / placeholder button background. */
#define THEME_COLOR_BTN_NEUTRAL         lv_color_hex(0x444444)

/** Scrollbar track, fuel-sender cal buttons, generic dividers. */
#define THEME_COLOR_SCROLLBAR           lv_color_hex(0x555555)

/** Medium-gray border (Screen2 chart, popup shadow border). */
#define THEME_COLOR_BORDER_MED          lv_color_hex(0x808080)

/** Near-white.  RPM bar gauge background. */
#define THEME_COLOR_RPM_BAR_BG          lv_color_hex(0xF3F3F3)

/* =========================================================================
 * COLOURS — text
 * ========================================================================= */

/** Black text — used on light/mint-green button backgrounds. */
#define THEME_COLOR_TEXT_ON_LIGHT       lv_color_hex(0x000000)

/** Ghost / placeholder text inside text inputs. */
#define THEME_COLOR_TEXT_GHOST          lv_color_hex(0x888888)

/** Hint / metadata text (serial, firmware labels). */
#define THEME_COLOR_TEXT_HINT           lv_color_hex(0x999999)

/** Light muted text (fuel-current label, OTA notes). */
#define THEME_COLOR_TEXT_DISABLED       lv_color_hex(0xAAAAAA)

/** Standard secondary / muted label text. */
#define THEME_COLOR_TEXT_MUTED          lv_color_hex(0xCCCCCC)

/** Primary / high-contrast label text. */
#define THEME_COLOR_TEXT_PRIMARY        lv_color_hex(0xFFFFFF)

/* =========================================================================
 * COLOURS — interactive buttons
 * ========================================================================= */

/** Material green — Save / OK buttons. */
#define THEME_COLOR_BTN_SAVE            lv_color_hex(0x4CAF50)

/** Material red — Cancel / Back buttons. */
#define THEME_COLOR_BTN_CANCEL          lv_color_hex(0xF44336)

/** Danger red — Close (×) buttons in dialogs. */
#define THEME_COLOR_BTN_CLOSE           lv_color_hex(0xFF4444)

/** Close button pressed state. */
#define THEME_COLOR_BTN_CLOSE_PRESSED   lv_color_hex(0xFF6666)

/** Mint-green Save button (device-settings variant). */
#define THEME_COLOR_BTN_SAVE_ALT        lv_color_hex(0x40FF80)

/** Mint-green Save button pressed state. */
#define THEME_COLOR_BTN_SAVE_ALT_PRESSED lv_color_hex(0x50FF90)

/** Dimmer / display-settings toggle button. */
#define THEME_COLOR_BTN_DIM             lv_color_hex(0x404040)

/** Dimmer button pressed state. */
#define THEME_COLOR_BTN_DIM_PRESSED     lv_color_hex(0x505050)

/** WiFi connect button / network action. */
#define THEME_COLOR_BTN_CONNECT         lv_color_hex(0x00AA44)

/** WiFi connect button pressed state. */
#define THEME_COLOR_BTN_CONNECT_PRESSED lv_color_hex(0x00CC55)

/** Neutral cancel button (OTA / WiFi dialogs). */
#define THEME_COLOR_BTN_GRAY            lv_color_hex(0x666666)

/** Neutral cancel button pressed state. */
#define THEME_COLOR_BTN_GRAY_PRESSED    lv_color_hex(0x777777)

/* =========================================================================
 * COLOURS — status / accent
 * ========================================================================= */

/** WiFi / OTA "connected / success" status. */
#define THEME_COLOR_STATUS_CONNECTED    lv_color_hex(0x00FF80)

/** WiFi / OTA "disconnected / error" status. */
#define THEME_COLOR_STATUS_ERROR        lv_color_hex(0xFF4444)

/** Pink-red — ECU section title, alternative disconnect indicator. */
#define THEME_COLOR_STATUS_WARN         lv_color_hex(0xFF4080)

/** Blue — WiFi button, progress bar, OTA dialog border. */
#define THEME_COLOR_ACCENT_BLUE         lv_color_hex(0x4080FF)

/** Blue pressed state. */
#define THEME_COLOR_ACCENT_BLUE_PRESSED lv_color_hex(0x5090FF)

/** Yellow — brightness value labels, display-section title. */
#define THEME_COLOR_ACCENT_YELLOW       lv_color_hex(0xFFFF40)

/** Orange — network / connectivity section title. */
#define THEME_COLOR_ACCENT_ORANGE       lv_color_hex(0xFF8040)

/** Professional steel-blue accent — active tabs, Load Preset button, section headers. */
#define THEME_COLOR_ACCENT              lv_color_hex(0x3D7EAA)

/** Dark accent fill — active tab background, pressed states. */
#define THEME_COLOR_ACCENT_DIM          lv_color_hex(0x1A3D54)

/** Muted amber — Alerts / warning section header. */
#define THEME_COLOR_ACCENT_AMBER        lv_color_hex(0xC89630)

/** Soft teal — Display section header. */
#define THEME_COLOR_ACCENT_TEAL         lv_color_hex(0x2A8A7A)

/** Navigation arrow icon default (Screen2). */
#define THEME_COLOR_NAV_DEFAULT         lv_color_hex(0x5F5F5F)

/** Navigation arrow icon pressed (Screen2). */
#define THEME_COLOR_NAV_PRESSED         lv_color_hex(0x35E31C)

/** Chart / scope border colour (Screen2). */
#define THEME_COLOR_CHART_BORDER        lv_color_hex(0xA0A0A0)

/* =========================================================================
 * COLOURS — device-settings section title accents
 * (one distinct colour per card to aid quick scanning)
 * ========================================================================= */
#define THEME_COLOR_SECTION_CAN_TITLE   lv_color_hex(0x00FF80)
#define THEME_COLOR_SECTION_INFO_TITLE  lv_color_hex(0x4080FF)
#define THEME_COLOR_SECTION_NET_TITLE   lv_color_hex(0xFF8040)
#define THEME_COLOR_SECTION_DISP_TITLE  lv_color_hex(0xFFFF40)
#define THEME_COLOR_SECTION_ECU_TITLE   lv_color_hex(0xFF4080)

/* =========================================================================
 * COLOURS — user-selectable widget palette
 * (these are the colour options presented in dropdowns / wheels)
 * ========================================================================= */
#define THEME_COLOR_GREEN               lv_color_hex(0x00FF00)
#define THEME_COLOR_GREEN_BRIGHT        lv_color_hex(0x38FF00)
#define THEME_COLOR_CYAN                lv_color_hex(0x00FFFF)
#define THEME_COLOR_YELLOW              lv_color_hex(0xFFFF00)
#define THEME_COLOR_ORANGE              lv_color_hex(0xFF7F00)
#define THEME_COLOR_ORANGE_WEB          lv_color_hex(0xFFA500)
#define THEME_COLOR_RED                 lv_color_hex(0xFF0000)
#define THEME_COLOR_BLUE                lv_color_hex(0x0080FF)
#define THEME_COLOR_BLUE_DARK           lv_color_hex(0x19439A)
#define THEME_COLOR_BLUE_PURE           lv_color_hex(0x0000FF)
#define THEME_COLOR_PURPLE              lv_color_hex(0x8000FF)
#define THEME_COLOR_MAGENTA             lv_color_hex(0xFF00FF)
#define THEME_COLOR_PINK                lv_color_hex(0xFF1493)

/* =========================================================================
 * FONTS — system (lv_font_montserrat_*)
 * ========================================================================= */

/** Tiny — warning circles, instruction text, preview text. */
#define THEME_FONT_TINY                 (&lv_font_montserrat_10)

/** Small — unit labels, CAN config labels, indicator labels. */
#define THEME_FONT_SMALL                (&lv_font_montserrat_12)

/** Body — custom panel text, preconfig screen, config menu text. */
#define THEME_FONT_BODY                 (&lv_font_montserrat_14)

/** Medium — dialog titles, section headers, bar value readouts. */
#define THEME_FONT_MEDIUM               (&lv_font_montserrat_16)

/** Large — popup / widget dialog titles (device-settings, wifi). */
#define THEME_FONT_LARGE                (&lv_font_montserrat_18)

/** X-Large — primary screen titles (device-settings header, Screen4). */
#define THEME_FONT_XLARGE               (&lv_font_montserrat_20)

/* =========================================================================
 * FONTS — dashboard display (custom bitmap fonts)
 * These mirror the LV_FONT_DECLARE names in ui.h.
 * ========================================================================= */

/** Panel heading / bar label / RPM label / gear screen title. */
#define THEME_FONT_DASH_LABEL           (&ui_font_fugaz_14)

/** RPM bar tick marks. */
#define THEME_FONT_DASH_TICK            (&ui_font_fugaz_17)

/** RPM numeric readout. */
#define THEME_FONT_DASH_RPM             (&ui_font_fugaz_28)

/** Speed numeric readout. */
#define THEME_FONT_DASH_SPEED           (&ui_font_fugaz_56)

/** Panel numeric value (3–4 digit). */
#define THEME_FONT_DASH_VALUE           (&ui_font_Manrope_35_BOLD)

/** Gear indicator value (single character, very large). */
#define THEME_FONT_DASH_GEAR            (&ui_font_Manrope_54_BOLD)

/* =========================================================================
 * SPACING & GEOMETRY
 * ========================================================================= */

/* Border widths */
#define THEME_BORDER_W_NONE             0
#define THEME_BORDER_W_THIN             1
#define THEME_BORDER_W_NORMAL           2

/* Border / outline radius */
#define THEME_RADIUS_NONE               0
#define THEME_RADIUS_SMALL              5
#define THEME_RADIUS_NORMAL             7
#define THEME_RADIUS_LARGE              10
#define THEME_RADIUS_PILL               LV_RADIUS_CIRCLE

/* Padding presets */
#define THEME_PAD_NONE                  0
#define THEME_PAD_TINY                  3
#define THEME_PAD_SMALL                 5
#define THEME_PAD_NORMAL                8
#define THEME_PAD_MEDIUM                12
#define THEME_PAD_LARGE                 20

/* Shadow parameters */
#define THEME_SHADOW_W_POPUP            20
#define THEME_SHADOW_OFS_POPUP          0
