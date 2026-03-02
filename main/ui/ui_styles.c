#include "ui_styles.h"
#include "theme.h"

/* -------------------------------------------------------------------------
 * Storage — one lv_style_t per shared style
 * ---------------------------------------------------------------------- */

lv_style_t ui_style_label_primary;
lv_style_t ui_style_label_muted;
lv_style_t ui_style_label_hint;
lv_style_t ui_style_label_on_light;

lv_style_t ui_style_screen_bg;
lv_style_t ui_style_surface;
lv_style_t ui_style_surface_bordered;
lv_style_t ui_style_input_bg;
lv_style_t ui_style_panel;
lv_style_t ui_style_panel_bordered;
lv_style_t ui_style_section_bg;
lv_style_t ui_style_inactive;

lv_style_t ui_style_btn_save;
lv_style_t ui_style_btn_cancel;
lv_style_t ui_style_btn_close;
lv_style_t ui_style_btn_save_alt;
lv_style_t ui_style_btn_connect;
lv_style_t ui_style_btn_gray;

lv_style_t ui_style_popup;
lv_style_t ui_style_dropdown;
lv_style_t ui_style_scrollbar;

/* -------------------------------------------------------------------------
 * ui_styles_init()
 * ---------------------------------------------------------------------- */
void ui_styles_init(void)
{
    /* --- Text --- */
    lv_style_init(&ui_style_label_primary);
    lv_style_set_text_color(&ui_style_label_primary, THEME_COLOR_TEXT_PRIMARY);

    lv_style_init(&ui_style_label_muted);
    lv_style_set_text_color(&ui_style_label_muted, THEME_COLOR_TEXT_MUTED);

    lv_style_init(&ui_style_label_hint);
    lv_style_set_text_color(&ui_style_label_hint, THEME_COLOR_TEXT_HINT);

    lv_style_init(&ui_style_label_on_light);
    lv_style_set_text_color(&ui_style_label_on_light, THEME_COLOR_TEXT_ON_LIGHT);

    /* --- Backgrounds --- */
    lv_style_init(&ui_style_screen_bg);
    lv_style_set_bg_color(&ui_style_screen_bg, THEME_COLOR_BG);
    lv_style_set_bg_opa(&ui_style_screen_bg, LV_OPA_COVER);

    lv_style_init(&ui_style_surface);
    lv_style_set_bg_color(&ui_style_surface, THEME_COLOR_SURFACE);
    lv_style_set_bg_opa(&ui_style_surface, LV_OPA_COVER);

    lv_style_init(&ui_style_surface_bordered);
    lv_style_set_bg_color(&ui_style_surface_bordered, THEME_COLOR_SURFACE);
    lv_style_set_bg_opa(&ui_style_surface_bordered, LV_OPA_COVER);
    lv_style_set_border_color(&ui_style_surface_bordered, THEME_COLOR_BORDER);
    lv_style_set_border_width(&ui_style_surface_bordered, THEME_BORDER_W_THIN);

    lv_style_init(&ui_style_input_bg);
    lv_style_set_bg_color(&ui_style_input_bg, THEME_COLOR_INPUT_BG);
    lv_style_set_bg_opa(&ui_style_input_bg, LV_OPA_COVER);

    lv_style_init(&ui_style_panel);
    lv_style_set_bg_color(&ui_style_panel, THEME_COLOR_PANEL);
    lv_style_set_bg_opa(&ui_style_panel, LV_OPA_COVER);

    lv_style_init(&ui_style_panel_bordered);
    lv_style_set_bg_color(&ui_style_panel_bordered, THEME_COLOR_PANEL);
    lv_style_set_bg_opa(&ui_style_panel_bordered, LV_OPA_COVER);
    lv_style_set_border_color(&ui_style_panel_bordered, THEME_COLOR_PANEL);
    lv_style_set_border_width(&ui_style_panel_bordered, THEME_BORDER_W_THIN);

    lv_style_init(&ui_style_section_bg);
    lv_style_set_bg_color(&ui_style_section_bg, THEME_COLOR_SECTION_BG);
    lv_style_set_bg_opa(&ui_style_section_bg, LV_OPA_COVER);
    lv_style_set_border_color(&ui_style_section_bg, THEME_COLOR_BORDER);
    lv_style_set_border_width(&ui_style_section_bg, THEME_BORDER_W_THIN);

    lv_style_init(&ui_style_inactive);
    lv_style_set_bg_color(&ui_style_inactive, THEME_COLOR_INACTIVE);
    lv_style_set_bg_opa(&ui_style_inactive, LV_OPA_COVER);

    /* --- Buttons --- */
    lv_style_init(&ui_style_btn_save);
    lv_style_set_bg_color(&ui_style_btn_save, THEME_COLOR_BTN_SAVE);
    lv_style_set_bg_opa(&ui_style_btn_save, LV_OPA_COVER);

    lv_style_init(&ui_style_btn_cancel);
    lv_style_set_bg_color(&ui_style_btn_cancel, THEME_COLOR_BTN_CANCEL);
    lv_style_set_bg_opa(&ui_style_btn_cancel, LV_OPA_COVER);

    lv_style_init(&ui_style_btn_close);
    lv_style_set_bg_color(&ui_style_btn_close, THEME_COLOR_BTN_CLOSE);
    lv_style_set_bg_opa(&ui_style_btn_close, LV_OPA_COVER);

    lv_style_init(&ui_style_btn_save_alt);
    lv_style_set_bg_color(&ui_style_btn_save_alt, THEME_COLOR_BTN_SAVE_ALT);
    lv_style_set_bg_opa(&ui_style_btn_save_alt, LV_OPA_COVER);

    lv_style_init(&ui_style_btn_connect);
    lv_style_set_bg_color(&ui_style_btn_connect, THEME_COLOR_BTN_CONNECT);
    lv_style_set_bg_opa(&ui_style_btn_connect, LV_OPA_COVER);

    lv_style_init(&ui_style_btn_gray);
    lv_style_set_bg_color(&ui_style_btn_gray, THEME_COLOR_BTN_GRAY);
    lv_style_set_bg_opa(&ui_style_btn_gray, LV_OPA_COVER);

    /* --- Popup --- */
    lv_style_init(&ui_style_popup);
    lv_style_set_bg_color(&ui_style_popup, THEME_COLOR_PANEL);
    lv_style_set_bg_opa(&ui_style_popup, LV_OPA_COVER);
    lv_style_set_border_color(&ui_style_popup, THEME_COLOR_BORDER_MED);
    lv_style_set_border_width(&ui_style_popup, THEME_BORDER_W_THIN);
    lv_style_set_shadow_color(&ui_style_popup, THEME_COLOR_BG);
    lv_style_set_shadow_width(&ui_style_popup, THEME_SHADOW_W_POPUP);
    lv_style_set_shadow_ofs_x(&ui_style_popup, THEME_SHADOW_OFS_POPUP);
    lv_style_set_shadow_ofs_y(&ui_style_popup, THEME_SHADOW_OFS_POPUP);

    /* --- Controls --- */
    lv_style_init(&ui_style_dropdown);
    lv_style_set_bg_color(&ui_style_dropdown, THEME_COLOR_CONTROL_BG);
    lv_style_set_bg_opa(&ui_style_dropdown, LV_OPA_COVER);
    lv_style_set_text_color(&ui_style_dropdown, THEME_COLOR_TEXT_PRIMARY);
    lv_style_set_border_color(&ui_style_dropdown, THEME_COLOR_SCROLLBAR);
    lv_style_set_border_width(&ui_style_dropdown, THEME_BORDER_W_THIN);

    lv_style_init(&ui_style_scrollbar);
    lv_style_set_bg_color(&ui_style_scrollbar, THEME_COLOR_SCROLLBAR);
    lv_style_set_bg_opa(&ui_style_scrollbar, LV_OPA_COVER);
}
