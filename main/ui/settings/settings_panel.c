#include "settings_panel.h"
#include "kit/ui_kit.h"

/* Row layout constants — rows keep these heights so callers' tab heights
 * (config_modal.c) still fit the same number of rows. */
#define ROW_H       34
#define ROW_PAD_V    3
#define ROW_PAD_H    8
#define LABEL_W    140

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static lv_obj_t *make_row(settings_section_t *sec)
{
    lv_obj_t *row = lv_obj_create(sec);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_set_style_pad_all(row, ROW_PAD_V, 0);
    lv_obj_set_style_pad_left(row, ROW_PAD_H, 0);
    lv_obj_set_style_pad_right(row, ROW_PAD_H, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static void make_label(lv_obj_t *row, const char *text)
{
    lv_obj_t *lbl = uk_label(row, text, UK_FONT_SMALL, UK_TONE_MUTED);
    lv_obj_set_width(lbl, LABEL_W);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
}

/* =========================================================================
 * Panel
 * ========================================================================= */

settings_panel_t *settings_panel_create(lv_obj_t *parent,
                                        lv_coord_t x, lv_coord_t y,
                                        lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, UK_R_TILE, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, THEME_PAD_NORMAL, 0);
    lv_obj_set_style_pad_row(panel, 6, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    uk_style_scrollbar(panel);
    return panel;
}

/* =========================================================================
 * Section
 * ========================================================================= */

/* A section is a kit card with a muted caps heading. @p accent used to paint
 * the heading and a bar down the left edge; the kit tells sections apart by
 * position and heading instead, so it is accepted and ignored. */
settings_section_t *settings_add_section(settings_panel_t *panel,
                                         const char *title,
                                         lv_color_t accent)
{
    (void)accent;
    lv_obj_t *card = uk_card(panel);
    lv_obj_set_style_pad_row(card, 3, 0);   /* rows carry their own padding */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (title && title[0]) {
        lv_obj_t *hdr = uk_section(card, title);
        lv_obj_set_style_pad_top(hdr, 0, 0);
        lv_obj_set_style_pad_bottom(hdr, 4, 0);
    }
    return card;
}

/* =========================================================================
 * Tabs
 * ========================================================================= */

settings_tabs_t *settings_add_tabs(settings_panel_t *panel,
                                   const char * const *tab_names,
                                   uint8_t n_tabs,
                                   lv_coord_t h)
{
    lv_obj_t *tabs = lv_tabview_create(panel, LV_DIR_TOP, 32);
    lv_obj_set_size(tabs, lv_pct(100), h > 0 ? h : 200);
    lv_obj_set_style_bg_color(tabs, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tabs, 0, 0);

    lv_obj_t *btns = lv_tabview_get_tab_btns(tabs);
    lv_obj_set_style_text_font(btns, uk_font(UK_FONT_SMALL), 0);
    lv_obj_set_style_bg_color(btns, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(btns, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btns, 1, 0);
    lv_obj_set_style_border_color(btns, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_side(btns, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_text_color(btns, THEME_COLOR_TEXT_MUTED, 0);

    /* Inactive items: transparent bg, no border */
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, LV_PART_ITEMS);
    lv_obj_set_style_border_width(btns, 0, LV_PART_ITEMS);

    /* Active tab: text lights up, accent underline only */
    lv_obj_set_style_text_color(btns, THEME_COLOR_TEXT_PRIMARY,
                                 LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP,
                             LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_side(btns, LV_BORDER_SIDE_BOTTOM,
                                  LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_color(btns, THEME_COLOR_ACCENT,
                                   LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_border_width(btns, 2,
                                   LV_PART_ITEMS | LV_STATE_CHECKED);

    for (uint8_t i = 0; i < n_tabs; i++) {
        lv_tabview_add_tab(tabs, tab_names[i]);
    }
    return tabs;
}

lv_obj_t *settings_get_tab(settings_tabs_t *tabs, uint8_t idx)
{
    return lv_obj_get_child(lv_tabview_get_content(tabs), idx);
}

/* =========================================================================
 * Row builders
 * ========================================================================= */

lv_obj_t *settings_add_text_input(settings_section_t *sec,
                                   const char *label,
                                   const char *placeholder,
                                   const char *initial_text)
{
    lv_obj_t *row = make_row(sec);
    make_label(row, label);

    lv_obj_t *ta = lv_textarea_create(row);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder ? placeholder : "");
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_set_height(ta, ROW_H - ROW_PAD_V * 2 - 2);
    uk_style_textarea(ta);
    lv_obj_set_style_text_font(ta, uk_font(UK_FONT_SMALL), 0);
    lv_obj_set_style_pad_left(ta, THEME_PAD_SMALL, 0);
    lv_obj_set_style_pad_right(ta, THEME_PAD_SMALL, 0);
    lv_obj_set_style_pad_top(ta, THEME_PAD_TINY, 0);
    lv_obj_set_style_pad_bottom(ta, THEME_PAD_TINY, 0);
    if (initial_text) lv_textarea_set_text(ta, initial_text);
    return ta;
}

lv_obj_t *settings_add_number_input(settings_section_t *sec,
                                     const char *label,
                                     const char *placeholder,
                                     const char *initial_text)
{
    return settings_add_text_input(sec, label, placeholder, initial_text);
}

lv_obj_t *settings_add_dropdown(settings_section_t *sec,
                                 const char *label,
                                 const char *options,
                                 lv_coord_t ctrl_w)
{
    lv_obj_t *row = make_row(sec);
    make_label(row, label);

    lv_obj_t *dd = lv_dropdown_create(row);
    if (options) lv_dropdown_set_options(dd, options);
    lv_obj_set_height(dd, ROW_H - ROW_PAD_V * 2 - 2);
    if (ctrl_w > 0) {
        lv_obj_set_width(dd, ctrl_w);
    } else {
        lv_obj_set_flex_grow(dd, 1);
    }
    uk_style_dropdown(dd);
    lv_obj_set_style_text_font(dd, uk_font(UK_FONT_SMALL), 0);
    lv_obj_set_style_pad_ver(dd, 4, 0);   /* fits the 26 px control height */
    lv_obj_set_style_text_color(dd, THEME_COLOR_TEXT_MUTED,
                                 LV_PART_INDICATOR | LV_STATE_DEFAULT);
    return dd;
}

lv_obj_t *settings_add_switch(settings_section_t *sec,
                               const char *label,
                               bool checked)
{
    lv_obj_t *row = make_row(sec);
    make_label(row, label);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 50, 25);
    uk_style_switch(sw);
    if (checked) lv_obj_add_state(sw, LV_STATE_CHECKED);
    return sw;
}

lv_obj_t *settings_add_roller(settings_section_t *sec,
                               const char *label,
                               const char *options,
                               uint8_t visible_rows)
{
    lv_obj_t *row = make_row(sec);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    make_label(row, label);

    /* No kit restyle for rollers: theme tokens, with the selected line in
     * the soft accent every other picker uses for a selection. */
    lv_obj_t *roller = lv_roller_create(row);
    if (options) lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller, visible_rows > 0 ? visible_rows : 3);
    lv_obj_set_style_text_font(roller, uk_font(UK_FONT_SMALL), 0);
    lv_obj_set_style_bg_color(roller, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_text_color(roller, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_border_color(roller, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(roller, 1, 0);
    lv_obj_set_style_radius(roller, UK_R_BTN, 0);
    lv_obj_set_style_bg_color(roller, THEME_COLOR_ACCENT_DIM,
                              LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(roller, LV_OPA_COVER,
                             LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(roller, ui_pal->accent_ink,
                                 LV_PART_SELECTED | LV_STATE_DEFAULT);
    lv_obj_set_flex_grow(roller, 1);
    return roller;
}

lv_obj_t *settings_add_color_swatch(settings_section_t *sec,
                                     const char *label,
                                     const char *options,
                                     lv_coord_t ctrl_w)
{
    return settings_add_dropdown(sec, label, options, ctrl_w);
}

lv_obj_t *settings_add_info_row(settings_section_t *sec,
                                 const char *key,
                                 const char *value_text)
{
    lv_obj_t *row = make_row(sec);
    make_label(row, key);

    lv_obj_t *val = uk_label(row, value_text ? value_text : "", UK_FONT_SMALL, UK_TONE_TEXT);
    lv_obj_set_flex_grow(val, 1);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    return val;
}

/* A full-width neutral kit button. @p bg_color used to fill it; kit buttons
 * take their colour from their kind, and none of these is the screen's one
 * primary action, so it is accepted and ignored. The caller adds its own
 * LV_EVENT_CLICKED handler to the returned button, as before. */
lv_obj_t *settings_add_button(settings_section_t *sec,
                               const char *text,
                               lv_color_t bg_color,
                               lv_coord_t h)
{
    (void)bg_color;
    lv_obj_t *btn = uk_btn(sec, UK_ICON_NONE, text, UK_BTN_NEUTRAL, NULL, NULL);
    lv_obj_set_size(btn, lv_pct(100), h > 0 ? h : 36);
    return btn;
}
