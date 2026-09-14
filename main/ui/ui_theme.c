/*
 * ui_theme.c — the menu palettes (ADR-0075).
 *
 * THEME_COLOR_* (theme.h) and the UI kit read ui_pal. Adding a theme is adding
 * a table here; nothing that draws a menu knows which table it is reading.
 */
#include "theme.h"
#include "kit/ui_kit.h"

/* Every argument parenthesised: LV_COLOR_MAKE16 shifts its arguments unguarded. */
#define H(x) LV_COLOR_MAKE((((x) >> 16) & 0xFF), (((x) >> 8) & 0xFF), ((x) & 0xFF))

static const ui_palette_t s_palettes[UI_THEME__COUNT] = {
    [UI_THEME_DARK] = {
        .bg             = H(0x000000),
        .bar            = H(0x000000),
        .surface        = H(0x151615),
        .card           = H(0x1D1F1D),
        .raised         = H(0x292C29),
        .raised_hi      = H(0x393C39),
        .input          = H(0x0C0D0C),
        .line           = H(0x2E302E),
        .line_strong    = H(0x464846),
        .scrollbar      = H(0x4A4D4A),
        .text           = H(0xE8E8E8),
        .text_muted     = H(0x8B8D8B),
        .text_hint      = H(0x5A5B5A),
        .text_on_accent = H(0xFFFFFF),
        .accent         = H(0xE8433C),
        .accent_pressed = H(0xF0625B),
        .accent_soft    = H(0x3A1413),
        .accent_ink     = H(0xF3B1AD),
        .ok             = H(0x3CCF7A),
        .warn           = H(0xF0A53A),
        .danger         = H(0xF26B64),
        .danger_fill    = H(0xB3261F),
        .danger_soft    = H(0x3A1413),
        .knob           = H(0xFFFFFF),
    },
    /* Light is drawn but not offered yet: it exists so the swap is proven,
     * and so the next person picks colours rather than plumbing. */
    [UI_THEME_LIGHT] = {
        .bg             = H(0xEFEFED),
        .bar            = H(0xFFFFFF),
        .surface        = H(0xFFFFFF),
        .card           = H(0xFFFFFF),
        .raised         = H(0xE6E7E5),
        .raised_hi      = H(0xD6D7D5),
        .input          = H(0xF6F6F5),
        .line           = H(0xD9DAD8),
        .line_strong    = H(0xBDBEBC),
        .scrollbar      = H(0xB0B1AF),
        .text           = H(0x161716),
        .text_muted     = H(0x5D5F5D),
        .text_hint      = H(0x8F918F),
        .text_on_accent = H(0xFFFFFF),
        .accent         = H(0xD2232A),
        .accent_pressed = H(0xB51C22),
        .accent_soft    = H(0xFBE2E1),
        .accent_ink     = H(0x9B1A1F),
        .ok             = H(0x1C9A54),
        .warn           = H(0xB36A00),
        .danger         = H(0xC62828),
        .danger_fill    = H(0xC62828),
        .danger_soft    = H(0xFBE2E1),
        .knob           = H(0xFFFFFF),
    },
};

const ui_palette_t *ui_pal = &s_palettes[UI_THEME_DARK];
static ui_theme_id_t s_theme = UI_THEME_DARK;

void ui_theme_set(ui_theme_id_t id)
{
    if ((unsigned)id >= UI_THEME__COUNT) return;
    s_theme = id;
    ui_pal = &s_palettes[id];

    lv_disp_t *disp = lv_disp_get_default();
    if (disp) {
        lv_theme_t *th = lv_theme_default_init(disp, ui_pal->accent, ui_pal->ok,
                                               id == UI_THEME_DARK, LV_FONT_DEFAULT);
        lv_disp_set_theme(disp, th);
    }
    uk_styles_rebuild();
}

ui_theme_id_t ui_theme_get(void)
{
    return s_theme;
}
