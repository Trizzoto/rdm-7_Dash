#include "settings_panel.h"

/* Row layout constants */
#define ROW_H       34
#define ROW_PAD_V    3
#define ROW_PAD_H    8
#define LABEL_W    155

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static lv_obj_t *make_row(settings_section_t *sec)
{
    lv_obj_t *row = lv_obj_create(sec);
    lv_obj_set_size(row, lv_pct(100), ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, ROW_PAD_V, 0);
    lv_obj_set_style_pad_left(row, ROW_PAD_H, 0);
    lv_obj_set_style_pad_right(row, ROW_PAD_H, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static void make_label(lv_obj_t *row, const char *text)
{
    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_width(lbl, LABEL_W);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
}

static void style_ctrl(lv_obj_t *ctrl)
{
    lv_obj_set_style_bg_color(ctrl, THEME_COLOR_CONTROL_BG, 0);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ctrl, THEME_COLOR_SCROLLBAR, 0);
    lv_obj_set_style_border_width(ctrl, 1, 0);
    lv_obj_set_style_text_color(ctrl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(ctrl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_radius(ctrl, 4, 0);
    lv_obj_set_style_pad_all(ctrl, 4, 0);
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
    lv_obj_set_style_radius(panel, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(panel, 6, 0);
    lv_obj_set_style_pad_row(panel, 5, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_style_bg_color(panel, THEME_COLOR_SCROLLBAR, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(panel, 180, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    lv_obj_set_style_width(panel, 6, LV_PART_SCROLLBAR | LV_STATE_DEFAULT);
    return panel;
}

/* =========================================================================
 * Section
 * ========================================================================= */

settings_section_t *settings_add_section(settings_panel_t *panel,
                                         const char *title,
                                         lv_color_t accent)
{
    lv_obj_t *card = lv_obj_create(panel);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_set_style_pad_bottom(card, 4, 0);
    lv_obj_set_style_pad_row(card, 2, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    if (title && title[0]) {
        lv_obj_t *hdr = lv_label_create(card);
        lv_label_set_text(hdr, title);
        lv_obj_set_style_text_color(hdr, accent, 0);
        lv_obj_set_style_text_font(hdr, THEME_FONT_SMALL, 0);
        lv_obj_set_style_pad_bottom(hdr, 3, 0);
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
    lv_obj_t *tabs = lv_tabview_create(panel, LV_DIR_TOP, 30);
    lv_obj_set_size(tabs, lv_pct(100), h > 0 ? h : 200);
    lv_obj_set_style_bg_color(tabs, THEME_COLOR_SURFACE, 0);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(lv_tabview_get_tab_btns(tabs), THEME_FONT_SMALL, 0);
    lv_obj_set_style_bg_color(lv_tabview_get_tab_btns(tabs), THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_text_color(lv_tabview_get_tab_btns(tabs), THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_color(lv_tabview_get_tab_btns(tabs), THEME_COLOR_TEXT_PRIMARY,
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
    style_ctrl(ta);
    lv_obj_set_style_pad_all(ta, 3, 0);
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
    style_ctrl(dd);
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

    lv_obj_t *roller = lv_roller_create(row);
    if (options) lv_roller_set_options(roller, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(roller, visible_rows > 0 ? visible_rows : 3);
    lv_obj_set_style_text_font(roller, THEME_FONT_SMALL, 0);
    lv_obj_set_style_bg_color(roller, THEME_COLOR_CONTROL_BG, 0);
    lv_obj_set_style_border_color(roller, THEME_COLOR_SCROLLBAR, 0);
    lv_obj_set_style_border_width(roller, 1, 0);
    lv_obj_set_style_text_color(roller, THEME_COLOR_TEXT_PRIMARY,
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

    lv_obj_t *val = lv_label_create(row);
    lv_label_set_text(val, value_text ? value_text : "");
    lv_obj_set_style_text_color(val, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(val, THEME_FONT_SMALL, 0);
    lv_obj_set_flex_grow(val, 1);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_RIGHT, 0);
    return val;
}

lv_obj_t *settings_add_button(settings_section_t *sec,
                               const char *text,
                               lv_color_t bg_color,
                               lv_coord_t h)
{
    lv_obj_t *btn = lv_btn_create(sec);
    lv_obj_set_size(btn, lv_pct(100), h > 0 ? h : 34);
    lv_obj_set_style_bg_color(btn, bg_color, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, THEME_RADIUS_SMALL, 0);
    lv_obj_set_style_border_width(btn, 0, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
    lv_obj_center(lbl);
    return btn;
}
