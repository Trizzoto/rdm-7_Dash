/*
 * ui_kit.h — the one set of parts every on-glass menu is built from (ADR-0075).
 *
 * A menu is a screen with a brand bar and a body; the body holds tiles,
 * cards, rows and buttons; anything modal is a popup. Every part takes its
 * colours from the active palette (theme.h, ui_theme.c) through styles this
 * module owns, so a menu written with these parts looks like every other menu
 * and follows a theme change without knowing one happened.
 *
 * Parts are plain LVGL objects: position them, hide them, add event
 * callbacks as usual. The kit only decides how they look.
 *
 * Call uk_init() once before building any menu (ui_init does).
 */
#pragma once

#include "lvgl.h"
#include "theme.h"
#include "kit/uk_icons.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Geometry ─────────────────────────────────────────────────────────── */
#define UK_BAR_H        52      /* brand / title bar                         */
#define UK_GAP          10      /* between tiles and cards                   */
#define UK_PAD          12      /* body padding                              */
#define UK_BTN_H        40
#define UK_R_TILE       10
#define UK_R_BTN        8
#define UK_R_POPUP      14

/* ── Setup ────────────────────────────────────────────────────────────── */
void uk_init(void);
/** Re-derive every shared style from ui_pal. ui_theme_set() calls this. */
void uk_styles_rebuild(void);

/* ── Type ─────────────────────────────────────────────────────────────── */
typedef enum {
    UK_FONT_TITLE,   /* Barlow Semi Condensed 26 — tile names, big values    */
    UK_FONT_HEAD,    /* Barlow Semi Condensed 20 — bar and popup titles      */
    UK_FONT_LABEL,   /* Barlow Semi Condensed 16 — caps labels, section heads*/
    UK_FONT_BODY,    /* Montserrat 14                                         */
    UK_FONT_SMALL,   /* Montserrat 12                                         */
    UK_FONT_TINY,    /* Montserrat 10                                         */
    UK_FONT__COUNT
} uk_font_t;
const lv_font_t *uk_font(uk_font_t f);

/** Semantic colour for text, icons and status lines. */
typedef enum {
    UK_TONE_TEXT, UK_TONE_MUTED, UK_TONE_HINT, UK_TONE_ACCENT,
    UK_TONE_OK, UK_TONE_WARN, UK_TONE_DANGER,
} uk_tone_t;
lv_color_t uk_tone(uk_tone_t t);

/** Caps label in the kit's condensed face (tracks +1). */
lv_obj_t *uk_label(lv_obj_t *parent, const char *text, uk_font_t font, uk_tone_t tone);

/* ── Screens ──────────────────────────────────────────────────────────── */
/** A new, unloaded screen on the palette background. */
lv_obj_t *uk_screen(void);

typedef enum { UK_BAR_CLOSE, UK_BAR_BACK, UK_BAR_NONE } uk_bar_action_t;

/** Brand bar across the top of @p screen: the RDM logo, @p title, a status
 *  strip (see uk_bar_status) and Close/Back on the right wired to @p cb. */
lv_obj_t *uk_bar(lv_obj_t *screen, const char *title, uk_bar_action_t action,
                 lv_event_cb_t cb, void *user_data);
/** Add a status item to the bar (a coloured dot when @p dot, then text).
 *  Returns the text label so a timer can repaint it. */
lv_obj_t *uk_bar_status(lv_obj_t *bar, bool dot, uk_tone_t tone, const char *text);

/** The area under the bar. Not scrollable; put a uk_grid or uk_scroll in it. */
lv_obj_t *uk_body(lv_obj_t *screen);
/** A vertically scrolling column filling @p parent. */
lv_obj_t *uk_scroll(lv_obj_t *parent);

/** An evenly divided grid filling @p parent (cols, rows <= 6). */
lv_obj_t *uk_grid(lv_obj_t *parent, uint8_t cols, uint8_t rows);
void uk_grid_place(lv_obj_t *child, uint8_t col, uint8_t row,
                   uint8_t col_span, uint8_t row_span);
/** Stretch the grid's last child across the rest of its row, so a page with
 *  an odd number of tiles ends on a wide tile instead of a hole. */
void uk_grid_fill_row(lv_obj_t *grid);

/* ── Tiles ────────────────────────────────────────────────────────────── */
typedef enum { UK_TILE_NORMAL, UK_TILE_HERO, UK_TILE_DANGER } uk_tile_kind_t;

/** A launcher tile: icon top-left, name at the foot, a status line under it.
 *  @p desc is an optional one-or-two line explanation above the status. */
lv_obj_t *uk_tile(lv_obj_t *parent, uk_icon_t icon, const char *title,
                  const char *desc, uk_tile_kind_t kind,
                  lv_event_cb_t cb, void *user_data);
/** Status line: @p strong in @p tone, then " · " and @p rest in muted.
 *  Either may be NULL; with no @p strong, @p rest takes the tone. */
void uk_tile_set_status(lv_obj_t *tile, const char *strong, const char *rest,
                        uk_tone_t tone);
/** Small caps badge top-right (e.g. a live "REC"). NULL hides it. */
void uk_tile_set_badge(lv_obj_t *tile, const char *text, uk_tone_t tone, bool dot);
/** The status line's first label, for code that repaints it by pointer. */
lv_obj_t *uk_tile_status_label(lv_obj_t *tile);

/* ── Cards, sections, rows ────────────────────────────────────────────── */
lv_obj_t *uk_card(lv_obj_t *parent);
lv_obj_t *uk_section(lv_obj_t *parent, const char *text);
/** Key on the left, value on the right, hairline under. Returns the value. */
lv_obj_t *uk_row(lv_obj_t *parent, const char *key, const char *value);

/* ── Buttons ──────────────────────────────────────────────────────────── */
typedef enum {
    UK_BTN_NEUTRAL,   /* the default                                          */
    UK_BTN_PRIMARY,   /* the one thing this screen is for                     */
    UK_BTN_DANGER,    /* wipes something                                      */
    UK_BTN_GHOST,     /* outline only — Close, Cancel                         */
    UK_BTN_ON,        /* a toggle that is currently on                        */
    UK_BTN__COUNT
} uk_btn_kind_t;

lv_obj_t *uk_btn(lv_obj_t *parent, uk_icon_t icon, const char *text,
                 uk_btn_kind_t kind, lv_event_cb_t cb, void *user_data);
void uk_btn_set_kind(lv_obj_t *btn, uk_btn_kind_t kind);
void uk_btn_set_text(lv_obj_t *btn, const char *text);
lv_obj_t *uk_btn_label(lv_obj_t *btn);

/* ── Popups ───────────────────────────────────────────────────────────── */
/** A modal card centred on lv_layer_top() over a dimmed backdrop that eats
 *  taps. The title sits top-left, a Close button top-right calls @p close_cb.
 *  Children go at y >= UK_POPUP_BODY_Y inside the card's padding. Deleting
 *  the card (lv_obj_del) takes the backdrop with it. */
#define UK_POPUP_BODY_Y 56
lv_obj_t *uk_popup(lv_coord_t w, lv_coord_t h, const char *title,
                   lv_event_cb_t close_cb);

/** Brief message pill near the bottom of the screen. */
void uk_toast(const char *text, uk_tone_t tone);

/* ── Pictures ─────────────────────────────────────────────────────────── */
lv_obj_t *uk_icon(lv_obj_t *parent, uk_icon_t icon, uk_icon_size_t size, uk_tone_t tone);
void uk_icon_set_tone(lv_obj_t *icon, uk_tone_t tone);
lv_obj_t *uk_logo(lv_obj_t *parent);

/* ── Restyling stock LVGL controls ────────────────────────────────────── */
void uk_style_slider(lv_obj_t *slider);
void uk_style_switch(lv_obj_t *sw);
void uk_style_dropdown(lv_obj_t *dd);
void uk_style_textarea(lv_obj_t *ta);
void uk_style_msgbox(lv_obj_t *mbox);
void uk_style_keyboard(lv_obj_t *kb);
void uk_style_list(lv_obj_t *list);
void uk_style_scrollbar(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
