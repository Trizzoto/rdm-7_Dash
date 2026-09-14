/*
 * ui_kit.c — see ui_kit.h and ADR-0075.
 *
 * Every look lives in a shared lv_style_t built from ui_pal. Parts are created
 * with lv_obj_remove_style_all() first, so the LVGL default theme contributes
 * nothing to them and a part looks the same wherever it is used. Restyle
 * helpers for stock controls (sliders, dropdowns…) add styles on top of the
 * default theme instead, because those widgets depend on its part geometry.
 */
#include "kit/ui_kit.h"

#include "esp_log.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_kit";

LV_IMG_DECLARE(ui_img_RDM_Light);

/* Barlow Semi Condensed SemiBold, subset to Latin basic + a few marks
 * (tools: pyftsubset, see main/embed/fonts). Rendered by lv_tiny_ttf. */
extern const uint8_t barlow_ui_ttf_start[] asm("_binary_barlow_ui_ttf_start");
extern const uint8_t barlow_ui_ttf_end[]   asm("_binary_barlow_ui_ttf_end");

static const lv_font_t *s_fonts[UK_FONT__COUNT];
static bool s_inited = false;

/* ── Styles ───────────────────────────────────────────────────────────── */
#define TONES 7
static lv_style_t s_screen, s_bar, s_body, s_grid, s_scroll;
static lv_style_t s_tile, s_tile_pr, s_tile_hero, s_tile_danger;
static lv_style_t s_card, s_section, s_row, s_caps;
static lv_style_t s_btn, s_btn_dis, s_btn_k[UK_BTN__COUNT], s_btn_k_pr[UK_BTN__COUNT];
static lv_style_t s_icon_t[TONES], s_text_t[TONES];
static lv_style_t s_backdrop, s_popup, s_toast, s_dot;
static lv_style_t s_sl_main, s_sl_ind, s_sl_knob;
static lv_style_t s_sw_main, s_sw_ind, s_sw_knob;
static lv_style_t s_dd_main, s_dd_list, s_dd_sel;
static lv_style_t s_ta_main, s_ta_focus, s_ta_cursor, s_ta_ph;
static lv_style_t s_mb_main, s_mb_btn, s_mb_btn_pr;
static lv_style_t s_kb_main, s_kb_key, s_kb_key_pr, s_kb_key_ck;
static lv_style_t s_list_main, s_list_btn;
static lv_style_t s_scrollbar;

static lv_style_t *const s_all[] = {
    &s_screen, &s_bar, &s_body, &s_grid, &s_scroll,
    &s_tile, &s_tile_pr, &s_tile_hero, &s_tile_danger,
    &s_card, &s_section, &s_row, &s_caps, &s_btn, &s_btn_dis,
    &s_backdrop, &s_popup, &s_toast, &s_dot,
    &s_sl_main, &s_sl_ind, &s_sl_knob, &s_sw_main, &s_sw_ind, &s_sw_knob,
    &s_dd_main, &s_dd_list, &s_dd_sel, &s_ta_main, &s_ta_focus, &s_ta_cursor, &s_ta_ph,
    &s_mb_main, &s_mb_btn, &s_mb_btn_pr, &s_kb_main, &s_kb_key, &s_kb_key_pr, &s_kb_key_ck,
    &s_list_main, &s_list_btn, &s_scrollbar,
};

lv_color_t uk_tone(uk_tone_t t)
{
    switch (t) {
    case UK_TONE_MUTED:  return ui_pal->text_muted;
    case UK_TONE_HINT:   return ui_pal->text_hint;
    case UK_TONE_ACCENT: return ui_pal->accent;
    case UK_TONE_OK:     return ui_pal->ok;
    case UK_TONE_WARN:   return ui_pal->warn;
    case UK_TONE_DANGER: return ui_pal->danger;
    case UK_TONE_TEXT:
    default:             return ui_pal->text;
    }
}

const lv_font_t *uk_font(uk_font_t f)
{
    if ((unsigned)f >= UK_FONT__COUNT || !s_fonts[f]) return &lv_font_montserrat_14;
    return s_fonts[f];
}

static const lv_font_t *_ttf(lv_coord_t px, const lv_font_t *fallback)
{
    size_t cache = (size_t)px * (size_t)px * 48;   /* ~48 glyphs; see font_manager.c */
    lv_font_t *f = lv_tiny_ttf_create_data_ex(barlow_ui_ttf_start,
                                              (size_t)(barlow_ui_ttf_end - barlow_ui_ttf_start),
                                              px, cache);
    if (!f) {
        ESP_LOGE(TAG, "Barlow %d px failed; using Montserrat", (int)px);
        return fallback;
    }
    return f;
}

void uk_styles_rebuild(void)
{
    const ui_palette_t *p = ui_pal;
    for (size_t i = 0; i < sizeof(s_all) / sizeof(s_all[0]); i++) lv_style_reset(s_all[i]);
    for (int k = 0; k < UK_BTN__COUNT; k++) { lv_style_reset(&s_btn_k[k]); lv_style_reset(&s_btn_k_pr[k]); }
    for (int t = 0; t < TONES; t++) { lv_style_reset(&s_icon_t[t]); lv_style_reset(&s_text_t[t]); }

    /* Screen, bar, body */
    lv_style_set_bg_color(&s_screen, p->bg);
    lv_style_set_bg_opa(&s_screen, LV_OPA_COVER);
    lv_style_set_text_color(&s_screen, p->text);
    lv_style_set_text_font(&s_screen, uk_font(UK_FONT_BODY));

    lv_style_set_bg_color(&s_bar, p->bar);
    lv_style_set_bg_opa(&s_bar, LV_OPA_COVER);
    lv_style_set_border_color(&s_bar, p->line);
    lv_style_set_border_width(&s_bar, 1);
    lv_style_set_border_side(&s_bar, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_pad_left(&s_bar, 14);
    lv_style_set_pad_right(&s_bar, 10);
    lv_style_set_pad_column(&s_bar, 14);

    lv_style_set_pad_all(&s_body, UK_PAD);

    lv_style_set_pad_row(&s_grid, UK_GAP);
    lv_style_set_pad_column(&s_grid, UK_GAP);

    lv_style_set_pad_row(&s_scroll, UK_GAP);

    lv_style_set_bg_color(&s_scrollbar, p->scrollbar);
    lv_style_set_bg_opa(&s_scrollbar, LV_OPA_60);
    lv_style_set_width(&s_scrollbar, 4);
    lv_style_set_radius(&s_scrollbar, 2);
    lv_style_set_pad_right(&s_scrollbar, 2);

    /* Tiles */
    lv_style_set_bg_color(&s_tile, p->card);
    lv_style_set_bg_opa(&s_tile, LV_OPA_COVER);
    lv_style_set_radius(&s_tile, UK_R_TILE);
    lv_style_set_border_color(&s_tile, p->line);
    lv_style_set_border_width(&s_tile, 1);
    lv_style_set_pad_top(&s_tile, 16);
    lv_style_set_pad_bottom(&s_tile, 14);
    lv_style_set_pad_hor(&s_tile, 14);
    lv_style_set_clip_corner(&s_tile, false);

    lv_style_set_bg_color(&s_tile_pr, p->raised);
    lv_style_set_border_color(&s_tile_pr, p->line_strong);

    /* No tinted gradient: at RGB565 a dark red-to-grey ramp has four or five
     * steps and bands visibly. The accent edge, lip and icon carry it. */
    lv_style_set_border_color(&s_tile_hero, p->accent);
    lv_style_set_border_opa(&s_tile_hero, LV_OPA_70);

    lv_style_set_border_color(&s_tile_danger, p->danger_fill);
    lv_style_set_border_opa(&s_tile_danger, LV_OPA_60);

    /* Cards, sections, rows, caps */
    lv_style_set_bg_color(&s_card, p->card);
    lv_style_set_bg_opa(&s_card, LV_OPA_COVER);
    lv_style_set_radius(&s_card, UK_R_TILE);
    lv_style_set_border_color(&s_card, p->line);
    lv_style_set_border_width(&s_card, 1);
    lv_style_set_pad_all(&s_card, 14);
    lv_style_set_pad_row(&s_card, 8);
    lv_style_set_pad_column(&s_card, 8);

    lv_style_set_text_color(&s_section, p->text_muted);
    lv_style_set_text_font(&s_section, uk_font(UK_FONT_LABEL));
    lv_style_set_text_letter_space(&s_section, 1);
    lv_style_set_pad_top(&s_section, 4);

    lv_style_set_border_color(&s_row, p->line);
    lv_style_set_border_width(&s_row, 1);
    lv_style_set_border_side(&s_row, LV_BORDER_SIDE_BOTTOM);
    lv_style_set_pad_ver(&s_row, 9);
    lv_style_set_pad_column(&s_row, 12);

    lv_style_set_text_letter_space(&s_caps, 1);

    /* Buttons */
    lv_style_set_radius(&s_btn, UK_R_BTN);
    lv_style_set_bg_opa(&s_btn, LV_OPA_COVER);
    lv_style_set_pad_hor(&s_btn, 16);
    lv_style_set_pad_column(&s_btn, 8);
    lv_style_set_text_font(&s_btn, uk_font(UK_FONT_BODY));
    lv_style_set_opa(&s_btn_dis, LV_OPA_40);

    struct { lv_color_t bg, fg, pr; lv_opa_t bg_opa; bool border; } kd[UK_BTN__COUNT] = {
        [UK_BTN_NEUTRAL] = { p->raised,      p->text,           p->raised_hi,      LV_OPA_COVER, false },
        [UK_BTN_PRIMARY] = { p->accent,      p->text_on_accent, p->accent_pressed, LV_OPA_COVER, false },
        [UK_BTN_DANGER]  = { p->danger_soft, p->danger,         p->danger_fill,    LV_OPA_COVER, false },
        [UK_BTN_GHOST]   = { p->bg,          p->text_muted,     p->raised,         LV_OPA_TRANSP, true },
        [UK_BTN_ON]      = { p->accent_soft, p->accent_ink,     p->raised_hi,      LV_OPA_COVER, false },
    };
    for (int k = 0; k < UK_BTN__COUNT; k++) {
        lv_style_set_bg_color(&s_btn_k[k], kd[k].bg);
        lv_style_set_bg_opa(&s_btn_k[k], kd[k].bg_opa);
        lv_style_set_text_color(&s_btn_k[k], kd[k].fg);
        lv_style_set_border_width(&s_btn_k[k], kd[k].border ? 1 : 0);
        lv_style_set_border_color(&s_btn_k[k], p->line_strong);
        lv_style_set_bg_color(&s_btn_k_pr[k], kd[k].pr);
        lv_style_set_bg_opa(&s_btn_k_pr[k], LV_OPA_COVER);
        if (k == UK_BTN_DANGER) lv_style_set_text_color(&s_btn_k_pr[k], p->text_on_accent);
    }

    for (int t = 0; t < TONES; t++) {
        lv_style_set_img_recolor(&s_icon_t[t], uk_tone((uk_tone_t)t));
        lv_style_set_img_recolor_opa(&s_icon_t[t], LV_OPA_COVER);
        lv_style_set_text_color(&s_text_t[t], uk_tone((uk_tone_t)t));
    }

    /* Popups, toast */
    lv_style_set_bg_color(&s_backdrop, lv_color_black());
    lv_style_set_bg_opa(&s_backdrop, 150);

    lv_style_set_bg_color(&s_popup, p->surface);
    lv_style_set_bg_opa(&s_popup, LV_OPA_COVER);
    lv_style_set_radius(&s_popup, UK_R_POPUP);
    lv_style_set_border_color(&s_popup, p->line_strong);
    lv_style_set_border_width(&s_popup, 1);
    lv_style_set_pad_all(&s_popup, 18);
    lv_style_set_text_color(&s_popup, p->text);
    lv_style_set_text_font(&s_popup, uk_font(UK_FONT_BODY));

    lv_style_set_bg_color(&s_toast, p->raised_hi);
    lv_style_set_bg_opa(&s_toast, LV_OPA_COVER);
    lv_style_set_radius(&s_toast, LV_RADIUS_CIRCLE);
    lv_style_set_pad_hor(&s_toast, 18);
    lv_style_set_pad_ver(&s_toast, 9);
    lv_style_set_text_font(&s_toast, uk_font(UK_FONT_BODY));

    lv_style_set_radius(&s_dot, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(&s_dot, LV_OPA_COVER);
    lv_style_set_width(&s_dot, 8);
    lv_style_set_height(&s_dot, 8);

    /* Slider: a thin track, an ink fill, a white knob — the dock's look */
    lv_style_set_bg_color(&s_sl_main, p->raised_hi);
    lv_style_set_bg_opa(&s_sl_main, LV_OPA_COVER);
    lv_style_set_radius(&s_sl_main, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s_sl_ind, p->text);
    lv_style_set_bg_opa(&s_sl_ind, LV_OPA_COVER);
    lv_style_set_radius(&s_sl_ind, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s_sl_knob, p->knob);
    lv_style_set_bg_opa(&s_sl_knob, LV_OPA_COVER);
    lv_style_set_radius(&s_sl_knob, LV_RADIUS_CIRCLE);
    lv_style_set_pad_all(&s_sl_knob, 7);
    lv_style_set_shadow_width(&s_sl_knob, 0);
    lv_style_set_border_width(&s_sl_knob, 0);

    lv_style_set_bg_color(&s_sw_main, p->raised_hi);
    lv_style_set_bg_opa(&s_sw_main, LV_OPA_COVER);
    lv_style_set_bg_color(&s_sw_ind, p->accent);
    lv_style_set_bg_opa(&s_sw_ind, LV_OPA_COVER);
    lv_style_set_bg_color(&s_sw_knob, p->knob);
    lv_style_set_bg_opa(&s_sw_knob, LV_OPA_COVER);

    /* Dropdown */
    lv_style_set_bg_color(&s_dd_main, p->raised);
    lv_style_set_bg_opa(&s_dd_main, LV_OPA_COVER);
    lv_style_set_border_color(&s_dd_main, p->line);
    lv_style_set_border_width(&s_dd_main, 1);
    lv_style_set_radius(&s_dd_main, UK_R_BTN);
    lv_style_set_text_color(&s_dd_main, p->text);
    lv_style_set_pad_hor(&s_dd_main, 12);
    lv_style_set_shadow_width(&s_dd_main, 0);
    lv_style_set_bg_color(&s_dd_list, p->surface);
    lv_style_set_bg_opa(&s_dd_list, LV_OPA_COVER);
    lv_style_set_border_color(&s_dd_list, p->line_strong);
    lv_style_set_border_width(&s_dd_list, 1);
    lv_style_set_radius(&s_dd_list, UK_R_BTN);
    lv_style_set_text_color(&s_dd_list, p->text);
    lv_style_set_shadow_width(&s_dd_list, 0);
    lv_style_set_bg_color(&s_dd_sel, p->accent_soft);
    lv_style_set_bg_opa(&s_dd_sel, LV_OPA_COVER);
    lv_style_set_text_color(&s_dd_sel, p->accent_ink);

    /* Text area */
    lv_style_set_bg_color(&s_ta_main, p->input);
    lv_style_set_bg_opa(&s_ta_main, LV_OPA_COVER);
    lv_style_set_border_color(&s_ta_main, p->line);
    lv_style_set_border_width(&s_ta_main, 1);
    lv_style_set_radius(&s_ta_main, UK_R_BTN);
    lv_style_set_text_color(&s_ta_main, p->text);
    lv_style_set_border_color(&s_ta_focus, p->accent);
    lv_style_set_border_color(&s_ta_cursor, p->accent);
    lv_style_set_text_color(&s_ta_ph, p->text_hint);

    /* Message box */
    lv_style_set_bg_color(&s_mb_main, p->surface);
    lv_style_set_bg_opa(&s_mb_main, LV_OPA_COVER);
    lv_style_set_border_color(&s_mb_main, p->line_strong);
    lv_style_set_border_width(&s_mb_main, 1);
    lv_style_set_radius(&s_mb_main, UK_R_POPUP);
    lv_style_set_pad_all(&s_mb_main, 18);
    lv_style_set_pad_row(&s_mb_main, 12);
    lv_style_set_shadow_width(&s_mb_main, 0);
    lv_style_set_text_color(&s_mb_main, p->text);
    lv_style_set_bg_color(&s_mb_btn, p->raised);
    lv_style_set_bg_opa(&s_mb_btn, LV_OPA_COVER);
    lv_style_set_text_color(&s_mb_btn, p->text);
    lv_style_set_radius(&s_mb_btn, UK_R_BTN);
    lv_style_set_border_width(&s_mb_btn, 0);
    lv_style_set_shadow_width(&s_mb_btn, 0);
    lv_style_set_bg_color(&s_mb_btn_pr, p->raised_hi);

    /* Keyboard */
    lv_style_set_bg_color(&s_kb_main, p->surface);
    lv_style_set_bg_opa(&s_kb_main, LV_OPA_COVER);
    lv_style_set_border_color(&s_kb_main, p->line);
    lv_style_set_border_width(&s_kb_main, 1);
    lv_style_set_border_side(&s_kb_main, LV_BORDER_SIDE_TOP);
    lv_style_set_bg_color(&s_kb_key, p->raised);
    lv_style_set_bg_opa(&s_kb_key, LV_OPA_COVER);
    lv_style_set_text_color(&s_kb_key, p->text);
    lv_style_set_radius(&s_kb_key, 6);
    lv_style_set_border_width(&s_kb_key, 0);
    lv_style_set_shadow_width(&s_kb_key, 0);
    lv_style_set_bg_color(&s_kb_key_pr, p->raised_hi);
    lv_style_set_bg_color(&s_kb_key_ck, p->card);

    /* List */
    lv_style_set_bg_color(&s_list_main, p->surface);
    lv_style_set_bg_opa(&s_list_main, LV_OPA_COVER);
    lv_style_set_border_color(&s_list_main, p->line);
    lv_style_set_border_width(&s_list_main, 1);
    lv_style_set_radius(&s_list_main, UK_R_TILE);
    lv_style_set_bg_opa(&s_list_btn, LV_OPA_TRANSP);
    lv_style_set_text_color(&s_list_btn, p->text);
    lv_style_set_border_color(&s_list_btn, p->line);
    lv_style_set_border_width(&s_list_btn, 1);
    lv_style_set_border_side(&s_list_btn, LV_BORDER_SIDE_BOTTOM);

    if (s_inited) lv_obj_report_style_change(NULL);
}

void uk_init(void)
{
    if (s_inited) return;
    s_fonts[UK_FONT_TITLE] = _ttf(26, &lv_font_montserrat_22);
    s_fonts[UK_FONT_HEAD]  = _ttf(21, &lv_font_montserrat_18);
    s_fonts[UK_FONT_LABEL] = _ttf(16, &lv_font_montserrat_14);
    s_fonts[UK_FONT_BODY]  = &lv_font_montserrat_14;
    s_fonts[UK_FONT_SMALL] = &lv_font_montserrat_12;
    s_fonts[UK_FONT_TINY]  = &lv_font_montserrat_10;
    for (size_t i = 0; i < sizeof(s_all) / sizeof(s_all[0]); i++) lv_style_init(s_all[i]);
    for (int k = 0; k < UK_BTN__COUNT; k++) { lv_style_init(&s_btn_k[k]); lv_style_init(&s_btn_k_pr[k]); }
    for (int t = 0; t < TONES; t++) { lv_style_init(&s_icon_t[t]); lv_style_init(&s_text_t[t]); }
    uk_styles_rebuild();
    s_inited = true;
}

/* ── Helpers ──────────────────────────────────────────────────────────── */

static lv_obj_t *_bare(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* The condensed face is drawn in capitals, like the mockups' CSS did. */
static void _set_caps(lv_obj_t *label, const char *text)
{
    char buf[96];
    size_t i = 0;
    for (; text && text[i] && i < sizeof(buf) - 1; i++)
        buf[i] = (char)toupper((unsigned char)text[i]);
    buf[i] = '\0';
    lv_label_set_text(label, buf);
}

lv_obj_t *uk_label(lv_obj_t *parent, const char *text, uk_font_t font, uk_tone_t tone)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_style(l, &s_text_t[tone], 0);
    lv_obj_set_style_text_font(l, uk_font(font), 0);
    if (font <= UK_FONT_LABEL) {
        lv_obj_add_style(l, &s_caps, 0);
        _set_caps(l, text);
    } else {
        lv_label_set_text(l, text ? text : "");
    }
    return l;
}

/* ── Screens ──────────────────────────────────────────────────────────── */

lv_obj_t *uk_screen(void)
{
    uk_init();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_add_style(scr, &s_screen, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t *uk_logo(lv_obj_t *parent)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, &ui_img_RDM_Light);
    return img;
}

lv_obj_t *uk_bar(lv_obj_t *screen, const char *title, uk_bar_action_t action,
                 lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *bar = _bare(screen);
    lv_obj_add_style(bar, &s_bar, 0);
    lv_obj_set_size(bar, lv_pct(100), UK_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    uk_logo(bar);                                                   /* 0 */
    lv_obj_t *t = uk_label(bar, title, UK_FONT_HEAD, UK_TONE_MUTED); /* 1 */
    lv_obj_set_style_pad_top(t, 2, 0);

    lv_obj_t *status = _bare(bar);                                  /* 2 */
    lv_obj_set_height(status, lv_pct(100));
    lv_obj_set_flex_grow(status, 1);
    lv_obj_set_flex_flow(status, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status, 18, 0);
    lv_obj_set_style_pad_right(status, 6, 0);

    if (action != UK_BAR_NONE) {
        lv_obj_t *b = uk_btn(bar,
                             action == UK_BAR_BACK ? UK_ICON_LEFT : UK_ICON_CLOSE,
                             action == UK_BAR_BACK ? "Back" : "Close",
                             UK_BTN_NEUTRAL, cb, user_data);        /* 3 */
        lv_obj_set_height(b, 36);
        lv_obj_set_style_pad_left(b, 10, 0);
        lv_obj_set_style_pad_right(b, 14, 0);
        lv_obj_set_ext_click_area(b, 10);
    }
    return bar;
}

lv_obj_t *uk_bar_status(lv_obj_t *bar, bool dot, uk_tone_t tone, const char *text)
{
    lv_obj_t *status = lv_obj_get_child(bar, 2);
    if (!status) return NULL;
    lv_obj_t *item = _bare(status);
    lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 7, 0);
    if (dot) {
        lv_obj_t *d = _bare(item);
        lv_obj_add_style(d, &s_dot, 0);
        lv_obj_set_style_bg_color(d, uk_tone(tone), 0);
    }
    lv_obj_t *l = uk_label(item, text, UK_FONT_SMALL, dot ? UK_TONE_MUTED : tone);
    return l;
}

lv_obj_t *uk_body(lv_obj_t *screen)
{
    lv_obj_t *b = _bare(screen);
    lv_obj_add_style(b, &s_body, 0);
    lv_obj_set_size(b, lv_pct(100), LV_VER_RES - UK_BAR_H);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, UK_BAR_H);
    return b;
}

lv_obj_t *uk_scroll(lv_obj_t *parent)
{
    lv_obj_t *s = _bare(parent);
    lv_obj_add_style(s, &s_scroll, 0);
    lv_obj_add_style(s, &s_scrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_size(s, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s, LV_DIR_VER);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    return s;
}

#define FR LV_GRID_FR(1)
#define GRID_TAG 0x6B697400   /* "kit" */
static const lv_coord_t s_fr[7][8] = {
    { LV_GRID_TEMPLATE_LAST },
    { FR, LV_GRID_TEMPLATE_LAST },
    { FR, FR, LV_GRID_TEMPLATE_LAST },
    { FR, FR, FR, LV_GRID_TEMPLATE_LAST },
    { FR, FR, FR, FR, LV_GRID_TEMPLATE_LAST },
    { FR, FR, FR, FR, FR, LV_GRID_TEMPLATE_LAST },
    { FR, FR, FR, FR, FR, FR, LV_GRID_TEMPLATE_LAST },
};

lv_obj_t *uk_grid(lv_obj_t *parent, uint8_t cols, uint8_t rows)
{
    if (cols < 1) cols = 1;
    if (cols > 6) cols = 6;
    if (rows < 1) rows = 1;
    if (rows > 6) rows = 6;
    lv_obj_t *g = _bare(parent);
    lv_obj_add_style(g, &s_grid, 0);
    lv_obj_set_size(g, lv_pct(100), lv_pct(100));
    lv_obj_set_grid_dsc_array(g, s_fr[cols], s_fr[rows]);
    lv_obj_set_user_data(g, (void *)(intptr_t)(GRID_TAG | cols));
    return g;
}

/* A part created straight into a uk_grid takes the next cell in reading
 * order; uk_grid_place() afterwards overrides it. */
static void _autoplace(lv_obj_t *child)
{
    lv_obj_t *g = lv_obj_get_parent(child);
    intptr_t tag = (intptr_t)lv_obj_get_user_data(g);
    if ((tag & ~0xFF) != GRID_TAG) return;
    uint8_t cols = (uint8_t)(tag & 0xFF);
    uint32_t i = lv_obj_get_index(child);
    uk_grid_place(child, (uint8_t)(i % cols), (uint8_t)(i / cols), 1, 1);
}

void uk_grid_fill_row(lv_obj_t *grid)
{
    intptr_t tag = (intptr_t)lv_obj_get_user_data(grid);
    uint32_t n = lv_obj_get_child_cnt(grid);
    if ((tag & ~0xFF) != GRID_TAG || n == 0) return;
    uint8_t cols = (uint8_t)(tag & 0xFF);
    uint32_t i = n - 1;
    uint8_t col = (uint8_t)(i % cols);
    uk_grid_place(lv_obj_get_child(grid, i), col, (uint8_t)(i / cols), (uint8_t)(cols - col), 1);
}

void uk_grid_place(lv_obj_t *child, uint8_t col, uint8_t row, uint8_t col_span, uint8_t row_span)
{
    lv_obj_set_grid_cell(child, LV_GRID_ALIGN_STRETCH, col, col_span ? col_span : 1,
                         LV_GRID_ALIGN_STRETCH, row, row_span ? row_span : 1);
}

/* ── Tiles ────────────────────────────────────────────────────────────── */
enum { T_ICON, T_SPACER, T_TITLE, T_DESC, T_STATUS, T_BADGE };

lv_obj_t *uk_tile(lv_obj_t *parent, uk_icon_t icon, const char *title,
                  const char *desc, uk_tile_kind_t kind,
                  lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_add_style(t, &s_tile, 0);
    if (kind == UK_TILE_HERO) lv_obj_add_style(t, &s_tile_hero, 0);
    if (kind == UK_TILE_DANGER) lv_obj_add_style(t, &s_tile_danger, 0);
    lv_obj_add_style(t, &s_tile_pr, LV_STATE_PRESSED);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    if (cb) lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, user_data);
    _autoplace(t);

    uk_tone_t icon_tone = kind == UK_TILE_HERO ? UK_TONE_ACCENT
                        : kind == UK_TILE_DANGER ? UK_TONE_DANGER : UK_TONE_TEXT;
    lv_obj_t *ic = uk_icon(t, icon, UK_ICON_LG, icon_tone);
    if (icon == UK_ICON_NONE) lv_obj_add_flag(ic, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *sp = _bare(t);
    lv_obj_set_width(sp, 1);
    lv_obj_set_flex_grow(sp, 1);

    /* With a description the tile carries three lines of text, so the name
     * steps down a size rather than wrapping. */
    lv_obj_t *tl = uk_label(t, title, (desc && desc[0]) ? UK_FONT_HEAD : UK_FONT_TITLE,
                            kind == UK_TILE_DANGER ? UK_TONE_DANGER : UK_TONE_TEXT);
    lv_obj_set_width(tl, lv_pct(100));
    lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);

    lv_obj_t *dl = uk_label(t, desc, UK_FONT_SMALL, UK_TONE_MUTED);
    lv_obj_set_width(dl, lv_pct(100));
    lv_label_set_long_mode(dl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(dl, 2, 0);
    lv_obj_set_style_pad_top(dl, 6, 0);
    if (!desc || !desc[0]) lv_obj_add_flag(dl, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *st = _bare(t);
    lv_obj_set_size(st, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(st, 6, 0);
    lv_obj_set_flex_flow(st, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    uk_label(st, "", UK_FONT_SMALL, UK_TONE_TEXT);               /* strong */
    /* The separator is drawn, not typed: Montserrat has no middle dot. */
    lv_obj_t *sep = _bare(st);
    lv_obj_set_size(sep, 15, 3);
    lv_obj_t *sep_dot = _bare(sep);
    lv_obj_add_style(sep_dot, &s_dot, 0);
    lv_obj_set_size(sep_dot, 3, 3);
    lv_obj_set_style_bg_color(sep_dot, ui_pal->text_hint, 0);
    lv_obj_center(sep_dot);
    lv_obj_t *rest = uk_label(st, "", UK_FONT_SMALL, UK_TONE_MUTED);
    lv_obj_set_flex_grow(rest, 1);
    lv_label_set_long_mode(rest, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(st, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *bd = _bare(t);
    lv_obj_add_flag(bd, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(bd, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(bd, LV_ALIGN_TOP_RIGHT, 0, 2);
    lv_obj_set_flex_flow(bd, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bd, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bd, 6, 0);
    lv_obj_t *bdot = _bare(bd);
    lv_obj_add_style(bdot, &s_dot, 0);
    uk_label(bd, "", UK_FONT_LABEL, UK_TONE_ACCENT);

    if (kind == UK_TILE_HERO) {
        /* The red top edge. Inset by the radius so it reads as a lip, not a
         * stripe glued over the rounded corners. */
        lv_obj_t *lip = _bare(t);
        lv_obj_add_flag(lip, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_style_bg_color(lip, ui_pal->accent, 0);
        lv_obj_set_style_bg_opa(lip, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(lip, 2, 0);
        lv_obj_set_size(lip, lv_pct(100), 3);
        /* Grow past the padding on both sides without moving layout, stopping
         * where the corner radius starts. */
        lv_obj_set_style_transform_width(lip, 14 - 8, 0);
        lv_obj_align(lip, LV_ALIGN_TOP_MID, 0, -15);
    }
    return t;
}

void uk_tile_set_status(lv_obj_t *tile, const char *strong, const char *rest, uk_tone_t tone)
{
    lv_obj_t *st = lv_obj_get_child(tile, T_STATUS);
    if (!st) return;
    lv_obj_t *a = lv_obj_get_child(st, 0);
    lv_obj_t *sep = lv_obj_get_child(st, 1);
    lv_obj_t *b = lv_obj_get_child(st, 2);
    bool has_a = strong && strong[0];
    bool has_b = rest && rest[0];
    if (!has_a && !has_b) { lv_obj_add_flag(st, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_clear_flag(st, LV_OBJ_FLAG_HIDDEN);

    for (int t = 0; t < TONES; t++) {
        lv_obj_remove_style(a, &s_text_t[t], 0);
        lv_obj_remove_style(b, &s_text_t[t], 0);
    }
    if (has_a) {
        lv_obj_clear_flag(a, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(a, strong);
        lv_obj_add_style(a, &s_text_t[tone], 0);
        lv_obj_add_style(b, &s_text_t[UK_TONE_MUTED], 0);
        lv_label_set_text(b, has_b ? rest : "");
        if (has_b) lv_obj_clear_flag(sep, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(sep, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sep, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(b, rest);
        lv_obj_add_style(b, &s_text_t[tone], 0);
    }
}

lv_obj_t *uk_tile_status_label(lv_obj_t *tile)
{
    lv_obj_t *st = lv_obj_get_child(tile, T_STATUS);
    return st ? lv_obj_get_child(st, 2) : NULL;
}

void uk_tile_set_badge(lv_obj_t *tile, const char *text, uk_tone_t tone, bool dot)
{
    lv_obj_t *bd = lv_obj_get_child(tile, T_BADGE);
    if (!bd) return;
    if (!text) { lv_obj_add_flag(bd, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_t *d = lv_obj_get_child(bd, 0);
    lv_obj_t *l = lv_obj_get_child(bd, 1);
    lv_obj_set_style_bg_color(d, uk_tone(tone), 0);
    if (dot) lv_obj_clear_flag(d, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    for (int t = 0; t < TONES; t++) lv_obj_remove_style(l, &s_text_t[t], 0);
    lv_obj_add_style(l, &s_text_t[tone], 0);
    _set_caps(l, text);
    lv_obj_clear_flag(bd, LV_OBJ_FLAG_HIDDEN);
}

/* ── Cards, sections, rows ────────────────────────────────────────────── */

lv_obj_t *uk_card(lv_obj_t *parent)
{
    lv_obj_t *c = _bare(parent);
    lv_obj_add_style(c, &s_card, 0);
    lv_obj_set_width(c, lv_pct(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    _autoplace(c);
    return c;
}

lv_obj_t *uk_section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_add_style(l, &s_section, 0);
    _set_caps(l, text);
    return l;
}

lv_obj_t *uk_row(lv_obj_t *parent, const char *key, const char *value)
{
    lv_obj_t *r = _bare(parent);
    lv_obj_add_style(r, &s_row, 0);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    uk_label(r, key, UK_FONT_BODY, UK_TONE_MUTED);
    lv_obj_t *v = uk_label(r, value, UK_FONT_BODY, UK_TONE_TEXT);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    return v;
}

/* ── Buttons ──────────────────────────────────────────────────────────── */
enum { B_ICON, B_LABEL };

static uk_tone_t _btn_icon_tone(uk_btn_kind_t k)
{
    switch (k) {
    case UK_BTN_DANGER: return UK_TONE_DANGER;
    case UK_BTN_GHOST:  return UK_TONE_MUTED;
    case UK_BTN_ON:     return UK_TONE_ACCENT;
    default:            return UK_TONE_TEXT;
    }
}

lv_obj_t *uk_btn(lv_obj_t *parent, uk_icon_t icon, const char *text,
                 uk_btn_kind_t kind, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_add_style(b, &s_btn, 0);
    lv_obj_add_style(b, &s_btn_dis, LV_STATE_DISABLED);
    lv_obj_set_height(b, UK_BTN_H);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ic = uk_icon(b, icon == UK_ICON_NONE ? UK_ICON_CHECK : icon, UK_ICON_MD, UK_TONE_TEXT);
    if (icon == UK_ICON_NONE) lv_obj_add_flag(ic, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text ? text : "");
    if (!text || !text[0]) {
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_pad_hor(b, 8, 0);
    }
    uk_btn_set_kind(b, kind);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    return b;
}

void uk_btn_set_kind(lv_obj_t *btn, uk_btn_kind_t kind)
{
    if ((unsigned)kind >= UK_BTN__COUNT) kind = UK_BTN_NEUTRAL;
    for (int k = 0; k < UK_BTN__COUNT; k++) {
        lv_obj_remove_style(btn, &s_btn_k[k], LV_PART_MAIN | LV_STATE_ANY);
        lv_obj_remove_style(btn, &s_btn_k_pr[k], LV_PART_MAIN | LV_STATE_ANY);
    }
    lv_obj_add_style(btn, &s_btn_k[kind], 0);
    lv_obj_add_style(btn, &s_btn_k_pr[kind], LV_STATE_PRESSED);
    lv_obj_t *ic = lv_obj_get_child(btn, B_ICON);
    if (ic) uk_icon_set_tone(ic, _btn_icon_tone(kind));
}

lv_obj_t *uk_btn_label(lv_obj_t *btn)
{
    return lv_obj_get_child(btn, B_LABEL);
}

void uk_btn_set_text(lv_obj_t *btn, const char *text)
{
    lv_obj_t *l = uk_btn_label(btn);
    if (!l) return;
    lv_label_set_text(l, text ? text : "");
    if (text && text[0]) lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
}

/* ── Pictures ─────────────────────────────────────────────────────────── */

lv_obj_t *uk_icon(lv_obj_t *parent, uk_icon_t icon, uk_icon_size_t size, uk_tone_t tone)
{
    lv_obj_t *img = lv_img_create(parent);
    const lv_img_dsc_t *src = uk_icon_img(icon, size);
    if (src) lv_img_set_src(img, src);
    else lv_obj_set_size(img, size == UK_ICON_LG ? UK_ICON_PX_LG : UK_ICON_PX_MD,
                         size == UK_ICON_LG ? UK_ICON_PX_LG : UK_ICON_PX_MD);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    uk_icon_set_tone(img, tone);
    return img;
}

void uk_icon_set_tone(lv_obj_t *icon, uk_tone_t tone)
{
    for (int t = 0; t < TONES; t++) lv_obj_remove_style(icon, &s_icon_t[t], 0);
    lv_obj_add_style(icon, &s_icon_t[tone], 0);
}

/* ── Popups ───────────────────────────────────────────────────────────── */

static void _popup_deleted_cb(lv_event_t *e)
{
    lv_obj_t *backdrop = lv_event_get_user_data(e);
    if (backdrop && lv_obj_is_valid(backdrop)) lv_obj_del(backdrop);
}

lv_obj_t *uk_popup(lv_coord_t w, lv_coord_t h, const char *title, lv_event_cb_t close_cb)
{
    uk_init();
    /* Backdrop and card are siblings on the top layer, not parent and child:
     * callers delete the card pointer they were given, and the card's DELETE
     * handler can then safely delete a sibling (never an ancestor). */
    lv_obj_t *bd = _bare(lv_layer_top());
    lv_obj_add_style(bd, &s_backdrop, 0);
    lv_obj_set_size(bd, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(bd, LV_OBJ_FLAG_CLICKABLE);   /* taps behind the card stop here */

    lv_obj_t *card = _bare(lv_layer_top());
    lv_obj_add_style(card, &s_popup, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(card, w, h);
    lv_obj_center(card);
    lv_obj_add_event_cb(card, _popup_deleted_cb, LV_EVENT_DELETE, bd);

    lv_obj_t *t = uk_label(card, title, UK_FONT_HEAD, UK_TONE_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 4);

    lv_obj_t *x = uk_btn(card, UK_ICON_CLOSE, NULL, UK_BTN_NEUTRAL, close_cb, NULL);
    lv_obj_set_size(x, 40, 36);
    lv_obj_set_style_pad_hor(x, 0, 0);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 4, -4);
    lv_obj_set_ext_click_area(x, 12);
    return card;
}

static void _toast_timer_cb(lv_timer_t *t)
{
    lv_obj_t *o = t->user_data;
    if (o && lv_obj_is_valid(o)) lv_obj_del(o);
}

void uk_toast(const char *text, uk_tone_t tone)
{
    uk_init();
    lv_obj_t *o = _bare(lv_layer_top());
    lv_obj_add_style(o, &s_toast, 0);
    lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_t *l = lv_label_create(o);
    lv_obj_add_style(l, &s_text_t[tone], 0);
    lv_label_set_text(l, text);
    lv_obj_align(o, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_timer_t *tm = lv_timer_create(_toast_timer_cb, 2200, o);
    lv_timer_set_repeat_count(tm, 1);
}

/* ── Stock controls ───────────────────────────────────────────────────── */

void uk_style_slider(lv_obj_t *s)
{
    lv_obj_add_style(s, &s_sl_main, LV_PART_MAIN);
    lv_obj_add_style(s, &s_sl_ind, LV_PART_INDICATOR);
    lv_obj_add_style(s, &s_sl_knob, LV_PART_KNOB);
}

void uk_style_switch(lv_obj_t *sw)
{
    lv_obj_add_style(sw, &s_sw_main, LV_PART_MAIN);
    lv_obj_add_style(sw, &s_sw_ind, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_style(sw, &s_sw_knob, LV_PART_KNOB);
}

void uk_style_dropdown(lv_obj_t *dd)
{
    lv_obj_add_style(dd, &s_dd_main, LV_PART_MAIN);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    if (list) {
        lv_obj_add_style(list, &s_dd_list, LV_PART_MAIN);
        lv_obj_add_style(list, &s_dd_sel, LV_PART_SELECTED | LV_STATE_CHECKED);
        lv_obj_add_style(list, &s_dd_sel, LV_PART_SELECTED | LV_STATE_PRESSED);
        lv_obj_add_style(list, &s_scrollbar, LV_PART_SCROLLBAR);
    }
}

void uk_style_textarea(lv_obj_t *ta)
{
    lv_obj_add_style(ta, &s_ta_main, LV_PART_MAIN);
    lv_obj_add_style(ta, &s_ta_focus, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_style(ta, &s_ta_cursor, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_add_style(ta, &s_ta_ph, LV_PART_TEXTAREA_PLACEHOLDER);
}

void uk_style_msgbox(lv_obj_t *mbox)
{
    lv_obj_add_style(mbox, &s_mb_main, LV_PART_MAIN);

    /* A modal message box (parent NULL) sits on its own full-screen backdrop,
     * which the default theme paints grey. Make it the kit's backdrop. */
    lv_obj_t *bg = lv_obj_get_parent(mbox);
    if (bg && bg != lv_layer_top() && bg != lv_scr_act() && lv_obj_get_parent(bg) == lv_layer_top())
        lv_obj_add_style(bg, &s_backdrop, 0);

    lv_obj_t *title = lv_msgbox_get_title(mbox);
    if (title) {
        lv_obj_set_style_text_font(title, uk_font(UK_FONT_HEAD), 0);
        lv_obj_add_style(title, &s_caps, 0);
        _set_caps(title, lv_label_get_text(title));
    }
    lv_obj_t *text = lv_msgbox_get_text(mbox);
    if (text) {
        lv_obj_set_style_text_font(text, uk_font(UK_FONT_BODY), 0);
        lv_obj_set_style_text_line_space(text, 4, 0);
        lv_obj_set_style_text_color(text, ui_pal->text_muted, 0);
    }
    lv_obj_t *btns = lv_msgbox_get_btns(mbox);
    if (btns) {
        lv_obj_add_style(btns, &s_mb_btn, LV_PART_ITEMS);
        lv_obj_add_style(btns, &s_mb_btn_pr, LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_width(btns, lv_pct(100));
        lv_obj_set_height(btns, UK_BTN_H);
        lv_obj_set_style_pad_all(btns, 0, 0);
        lv_obj_set_style_pad_column(btns, 10, 0);
        lv_obj_set_style_pad_top(btns, 4, 0);
        lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btns, 0, 0);
        lv_obj_set_style_text_font(btns, uk_font(UK_FONT_BODY), LV_PART_ITEMS);
    }
    lv_obj_t *x = lv_msgbox_get_close_btn(mbox);
    if (x) {
        lv_obj_set_style_bg_color(x, ui_pal->raised, 0);
        lv_obj_set_style_bg_color(x, ui_pal->raised_hi, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(x, ui_pal->text, 0);
        lv_obj_set_style_radius(x, UK_R_BTN, 0);
        lv_obj_set_style_shadow_width(x, 0, 0);
        lv_obj_set_size(x, 36, 36);
    }
}

void uk_style_keyboard(lv_obj_t *kb)
{
    lv_obj_add_style(kb, &s_kb_main, LV_PART_MAIN);
    lv_obj_add_style(kb, &s_kb_key, LV_PART_ITEMS);
    lv_obj_add_style(kb, &s_kb_key_pr, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_add_style(kb, &s_kb_key_ck, LV_PART_ITEMS | LV_STATE_CHECKED);
}

void uk_style_list(lv_obj_t *list)
{
    lv_obj_add_style(list, &s_list_main, LV_PART_MAIN);
    lv_obj_add_style(list, &s_scrollbar, LV_PART_SCROLLBAR);
    uint32_t n = lv_obj_get_child_cnt(list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(list, i);
        if (lv_obj_check_type(c, &lv_list_btn_class)) lv_obj_add_style(c, &s_list_btn, 0);
    }
}

void uk_style_scrollbar(lv_obj_t *obj)
{
    lv_obj_add_style(obj, &s_scrollbar, LV_PART_SCROLLBAR);
}
