/**
 * theme.h — Centralised design tokens for the RDM-7 dashboard.
 *
 * Palette built from user-specified colours:
 *   0x000000  black (dashboard bg)
 *   0x181C18  darkest layer (inputs, borders)
 *   0x292C29  surface (modals, settings containers)
 *   0x393C39  panel / cards (sections, elevated panels)
 *   0x5A595A  mid-tone (scrollbar, neutral buttons, hint text)
 *   0x848684  light (muted text, medium borders)
 *   0x218E8C  teal accent
 *
 * Two-step build safety:
 *   1. Include this header in every .c file that touches LVGL styling.
 *   2. Run the compiler — any misspelled constant is a compile-time error.
 */

#pragma once
#include "lvgl.h"

/* =========================================================================
 * PALETTE — the menus' colours, chosen at runtime (ADR-0075)
 *
 * Every menu, popup, settings page and the setup wizard colours itself through
 * the THEME_COLOR_* names below. Those names used to be constants; they now
 * read the active palette, so dark and light are two tables in ui_theme.c and
 * switching is ui_theme_set() plus rebuilding the open screen — no screen
 * code knows which one it is drawing.
 *
 * Dashboard widgets do NOT use these: a layout's colours are the layout's,
 * not the menu theme's. They use the frozen WIDGET_COLOR_* set further down.
 * ========================================================================= */

typedef struct {
    lv_color_t bg;            /* screen behind a menu                        */
    lv_color_t bar;           /* the brand / title bar                       */
    lv_color_t surface;       /* popups, modal bodies                        */
    lv_color_t card;          /* tiles, cards                                */
    lv_color_t raised;        /* buttons and controls sitting on a card      */
    lv_color_t raised_hi;     /* pressed control, a second neutral step      */
    lv_color_t input;         /* text fields, slider tracks                  */
    lv_color_t line;          /* hairline borders and dividers               */
    lv_color_t line_strong;   /* popup edges                                 */
    lv_color_t scrollbar;
    lv_color_t text;
    lv_color_t text_muted;
    lv_color_t text_hint;
    lv_color_t text_on_accent;
    lv_color_t accent;        /* RDM red: selection, the one primary action  */
    lv_color_t accent_pressed;
    lv_color_t accent_soft;   /* tinted fill behind an "on" control          */
    lv_color_t accent_ink;    /* text / icon on accent_soft                  */
    lv_color_t ok;
    lv_color_t warn;
    lv_color_t danger;        /* destructive text                            */
    lv_color_t danger_fill;   /* destructive button fill                     */
    lv_color_t danger_soft;
    lv_color_t knob;          /* slider / switch knob                        */
} ui_palette_t;

typedef enum { UI_THEME_DARK = 0, UI_THEME_LIGHT, UI_THEME__COUNT } ui_theme_id_t;

/** The palette in use. Never NULL — starts on the dark table. */
extern const ui_palette_t *ui_pal;

/** Switch palettes. Rebuilds the kit's shared styles and the LVGL default
 *  theme; screens already on the glass keep their colours until rebuilt. */
void ui_theme_set(ui_theme_id_t id);
ui_theme_id_t ui_theme_get(void);

/* ── Backgrounds ───────────────────────────────────────────────────────── */
#define THEME_COLOR_BG                  (ui_pal->bg)
#define THEME_COLOR_SURFACE             (ui_pal->surface)
#define THEME_COLOR_SURFACE_ALT         (ui_pal->surface)
#define THEME_COLOR_INPUT_BG            (ui_pal->input)
#define THEME_COLOR_SECTION_BG          (ui_pal->raised)
#define THEME_COLOR_INACTIVE            (ui_pal->card)
#define THEME_COLOR_PANEL               (ui_pal->card)
#define THEME_COLOR_KEYBOARD_BG         (ui_pal->surface)
#define THEME_COLOR_CONTROL_BG          (ui_pal->raised)
#define THEME_COLOR_BORDER              (ui_pal->line)
#define THEME_COLOR_BTN_NEUTRAL         (ui_pal->raised)
#define THEME_COLOR_SCROLLBAR           (ui_pal->scrollbar)
#define THEME_COLOR_BORDER_MED          (ui_pal->line_strong)
#define THEME_COLOR_HIGHLIGHT           (ui_pal->raised)
/** Near-white RPM bar background — a widget default, not themed. */
#define THEME_COLOR_RPM_BAR_BG          lv_color_hex(0xF0F0F0)

/* ── Text ──────────────────────────────────────────────────────────────── */
#define THEME_COLOR_TEXT_ON_LIGHT       lv_color_hex(0x000000)
#define THEME_COLOR_TEXT_ON_ACCENT      (ui_pal->text_on_accent)
#define THEME_COLOR_TEXT_GHOST          (ui_pal->text_hint)
#define THEME_COLOR_TEXT_HINT           (ui_pal->text_hint)
#define THEME_COLOR_TEXT_DISABLED       (ui_pal->text_muted)
#define THEME_COLOR_TEXT_MUTED          (ui_pal->text_muted)
#define THEME_COLOR_TEXT_PRIMARY        (ui_pal->text)

/* ── Buttons ───────────────────────────────────────────────────────────── */
#define THEME_COLOR_BTN_SAVE            (ui_pal->accent)
#define THEME_COLOR_BTN_SAVE_PRESSED    (ui_pal->accent_pressed)
#define THEME_COLOR_BTN_CANCEL          (ui_pal->raised_hi)
#define THEME_COLOR_BTN_CANCEL_PRESSED  (ui_pal->line_strong)
#define THEME_COLOR_BTN_CLOSE           (ui_pal->raised_hi)
#define THEME_COLOR_BTN_CLOSE_PRESSED   (ui_pal->line_strong)
#define THEME_COLOR_BTN_SAVE_ALT        (ui_pal->accent)
#define THEME_COLOR_BTN_SAVE_ALT_PRESSED (ui_pal->accent_pressed)
#define THEME_COLOR_BTN_DIM             (ui_pal->raised)
#define THEME_COLOR_BTN_DIM_PRESSED     (ui_pal->raised_hi)
#define THEME_COLOR_BTN_CONNECT         (ui_pal->accent)
#define THEME_COLOR_BTN_CONNECT_PRESSED (ui_pal->accent_pressed)
#define THEME_COLOR_BTN_GRAY            (ui_pal->raised_hi)
#define THEME_COLOR_BTN_GRAY_PRESSED    (ui_pal->line_strong)
#define THEME_COLOR_BTN_DANGER          (ui_pal->danger_fill)
#define THEME_COLOR_BTN_DANGER_BG       (ui_pal->danger_soft)

/* ── Status / accent ───────────────────────────────────────────────────── */
#define THEME_COLOR_STATUS_CONNECTED    (ui_pal->ok)
#define THEME_COLOR_STATUS_ERROR        (ui_pal->danger)
#define THEME_COLOR_STATUS_WARN         (ui_pal->warn)
#define THEME_COLOR_ACCENT_BLUE         (ui_pal->accent)      /* historical name */
#define THEME_COLOR_ACCENT_BLUE_PRESSED (ui_pal->accent_pressed)
#define THEME_COLOR_ACCENT_YELLOW       (ui_pal->text)
#define THEME_COLOR_ACCENT_ORANGE       (ui_pal->warn)
#define THEME_COLOR_ACCENT              (ui_pal->accent)
#define THEME_COLOR_ACCENT_DIM          (ui_pal->accent_soft)
#define THEME_COLOR_ACCENT_AMBER        (ui_pal->warn)
#define THEME_COLOR_ACCENT_TEAL         (ui_pal->ok)
#define THEME_COLOR_NAV_DEFAULT         (ui_pal->text_hint)
#define THEME_COLOR_NAV_PRESSED         (ui_pal->accent)
#define THEME_COLOR_CHART_BORDER        (ui_pal->line_strong)

/* One colour per settings section used to tell them apart; the new look
 * tells them apart by position and a muted caps label instead. */
#define THEME_COLOR_SECTION_CAN_TITLE   (ui_pal->text_muted)
#define THEME_COLOR_SECTION_INFO_TITLE  (ui_pal->text_muted)
#define THEME_COLOR_SECTION_NET_TITLE   (ui_pal->text_muted)
#define THEME_COLOR_SECTION_DISP_TITLE  (ui_pal->text_muted)
#define THEME_COLOR_SECTION_ECU_TITLE   (ui_pal->text_muted)

/* =========================================================================
 * COLOURS — dashboard widget defaults (frozen)
 * What a gauge, bar or panel draws with when its layout doesn't say. These
 * are part of how a layout looks, so they must match Studio's renderer and
 * never follow the menu theme.
 * ========================================================================= */
#define WIDGET_COLOR_BG                 lv_color_hex(0x000000)
#define WIDGET_COLOR_SURFACE            lv_color_hex(0x292C29)
#define WIDGET_COLOR_INPUT_BG           lv_color_hex(0x181C18)
#define WIDGET_COLOR_INACTIVE           lv_color_hex(0x292C29)
#define WIDGET_COLOR_PANEL              lv_color_hex(0x393C39)
#define WIDGET_COLOR_CONTROL_BG         lv_color_hex(0x393C39)
#define WIDGET_COLOR_BORDER_MED         lv_color_hex(0x848684)
#define WIDGET_COLOR_TEXT_MUTED         lv_color_hex(0x848684)
#define WIDGET_COLOR_TEXT_PRIMARY       lv_color_hex(0xE8E8E8)
#define WIDGET_COLOR_BTN_SAVE           lv_color_hex(0x2196F3)
#define WIDGET_COLOR_BTN_CANCEL         lv_color_hex(0xF04030)

/* =========================================================================
 * COLOURS — user-selectable widget palette
 * (these are the colour options presented in dropdowns / wheels)
 * ========================================================================= */
#define THEME_COLOR_GREEN               lv_color_hex(0x00F800)
#define THEME_COLOR_GREEN_BRIGHT        lv_color_hex(0x38F800)
#define THEME_COLOR_CYAN                lv_color_hex(0x00F8F8)
#define THEME_COLOR_YELLOW              lv_color_hex(0xF8FC00)
#define THEME_COLOR_ORANGE              lv_color_hex(0xF88000)
#define THEME_COLOR_ORANGE_WEB          lv_color_hex(0xF8A400)
#define THEME_COLOR_RED                 lv_color_hex(0xF80000)
#define THEME_COLOR_BLUE                lv_color_hex(0x0080F8)
#define THEME_COLOR_BLUE_DARK           lv_color_hex(0x184098)
#define THEME_COLOR_BLUE_PURE           lv_color_hex(0x0000F8)
#define THEME_COLOR_PURPLE              lv_color_hex(0x8000F8)
#define THEME_COLOR_MAGENTA             lv_color_hex(0xF800F8)
#define THEME_COLOR_PINK                lv_color_hex(0xF81490)

/* =========================================================================
 * FONTS — system (Montserrat, with a symbol fallback)
 * ========================================================================= */

/** Montserrat at px (10, 12 … 22), with λ, μ, Δ, ±, ×, — and friends drawn
 *  from a DejaVu fallback once uk_init() has run (ui_kit.c). Before that it
 *  is the plain built-in face. */
const lv_font_t *ui_theme_font(uint8_t px);

/** Tiny — warning circles, instruction text, preview text. */
#define THEME_FONT_TINY                 ui_theme_font(10)

/** Small — unit labels, CAN config labels, indicator labels. */
#define THEME_FONT_SMALL                ui_theme_font(12)

/** Body — custom panel text, preconfig screen, config menu text. */
#define THEME_FONT_BODY                 ui_theme_font(14)

/** Medium — dialog titles, section headers, bar value readouts. */
#define THEME_FONT_MEDIUM               ui_theme_font(16)

/** Large — popup / widget dialog titles (device-settings, wifi). */
#define THEME_FONT_LARGE                ui_theme_font(18)

/** X-Large — primary screen titles (device-settings header, Screen4). */
#define THEME_FONT_XLARGE               ui_theme_font(20)

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

/** Panel numeric value (3-4 digit). */
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
#define THEME_RADIUS_SMALL              4
#define THEME_RADIUS_NORMAL             6
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
