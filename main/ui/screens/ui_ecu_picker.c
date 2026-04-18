/* ui_ecu_picker.c - shared ECU-selection overlay.
 *
 * Two-step picker: pick a make, then a version. When the user taps Apply,
 * the selected preset is applied to the named layout and persisted in NVS.
 * The caller's done callback receives an `applied` flag so it can trigger
 * a dashboard reload at the appropriate moment (reloading inline would
 * destroy any parent overlay - see first_run_wizard.c for the wizard path). */

#include "ui_ecu_picker.h"

#include "esp_log.h"
#include "lvgl.h"

#include "../theme.h"
#include "../../layout/ecu_presets.h"
#include "../../layout/layout_manager.h"
#include "../../storage/config_store.h"

static const char *TAG = "ecu_picker";

#define CARD_W 600
#define CARD_H 360

/* Extra pseudo-option for "Custom / None (configure signals manually)".
 * Stored as the string below ECU_PRESETS in the make dropdown. */
#define CUSTOM_LABEL "Custom / None"

typedef struct {
    lv_obj_t *overlay;
    lv_obj_t *card;
    lv_obj_t *make_dd;
    lv_obj_t *version_dd;
    lv_obj_t *apply_btn;
    lv_obj_t *apply_label;
    char      layout_name[LAYOUT_MAX_NAME];
    ecu_picker_done_cb_t cb;
    void     *ctx;
} ecu_picker_state_t;

static ecu_picker_state_t s;

/* Count how many versions exist for the given make in ECU_PRESETS. */
static int _versions_for_make(const char *make, const char *out_names[], int max) {
    int n = 0;
    for (int i = 0; i < ECU_PRESETS_COUNT && n < max; i++) {
        if (strcmp(ECU_PRESETS[i].make, make) == 0) {
            out_names[n++] = ECU_PRESETS[i].version;
        }
    }
    return n;
}

/* Build a newline-separated dedup'd list of makes from ECU_PRESETS,
 * plus the trailing "Custom / None" pseudo-option. */
static void _build_make_options(char *buf, size_t n) {
    buf[0] = '\0';
    for (int i = 0; i < ECU_PRESETS_COUNT; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) {
            if (strcmp(ECU_PRESETS[j].make, ECU_PRESETS[i].make) == 0) { dup = true; break; }
        }
        if (dup) continue;
        if (buf[0]) strncat(buf, "\n", n - strlen(buf) - 1);
        strncat(buf, ECU_PRESETS[i].make, n - strlen(buf) - 1);
    }
    if (buf[0]) strncat(buf, "\n", n - strlen(buf) - 1);
    strncat(buf, CUSTOM_LABEL, n - strlen(buf) - 1);
}

static void _update_apply_state(void) {
    if (!s.apply_btn || !s.apply_label) return;
    char make[48] = {0};
    lv_dropdown_get_selected_str(s.make_dd, make, sizeof(make));
    bool is_custom = (strcmp(make, CUSTOM_LABEL) == 0);
    bool has_make  = make[0] != '\0';
    /* Apply enables for real ECUs (needs both make + version) OR for Custom
     * (which just clears NVS, no version needed). */
    bool enable = is_custom || (has_make && s.version_dd &&
                                lv_dropdown_get_option_cnt(s.version_dd) > 0);
    if (enable) {
        lv_obj_clear_state(s.apply_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s.apply_btn, THEME_COLOR_ACCENT_BLUE, 0);
        lv_obj_set_style_text_color(s.apply_label, THEME_COLOR_TEXT_ON_ACCENT, 0);
    } else {
        lv_obj_add_state(s.apply_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s.apply_btn, THEME_COLOR_SECTION_BG, 0);
        lv_obj_set_style_text_color(s.apply_label, THEME_COLOR_TEXT_MUTED, 0);
    }
}

/* Repopulate the version dropdown based on the current make selection. */
static void _refresh_version_dd(const char *preselect_version) {
    if (!s.version_dd) return;
    char make[48] = {0};
    lv_dropdown_get_selected_str(s.make_dd, make, sizeof(make));

    if (strcmp(make, CUSTOM_LABEL) == 0) {
        lv_dropdown_set_options(s.version_dd, "(none)");
        lv_dropdown_set_selected(s.version_dd, 0);
        lv_obj_add_state(s.version_dd, LV_STATE_DISABLED);
        _update_apply_state();
        return;
    }
    lv_obj_clear_state(s.version_dd, LV_STATE_DISABLED);

    const char *vers[8]; int n = _versions_for_make(make, vers, 8);
    if (n == 0) {
        lv_dropdown_set_options(s.version_dd, "(no versions)");
        lv_dropdown_set_selected(s.version_dd, 0);
        _update_apply_state();
        return;
    }
    char buf[256]; buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i) strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
        strncat(buf, vers[i], sizeof(buf) - strlen(buf) - 1);
    }
    lv_dropdown_set_options(s.version_dd, buf);

    int sel = 0;
    if (preselect_version) {
        for (int i = 0; i < n; i++) {
            if (strcmp(vers[i], preselect_version) == 0) { sel = i; break; }
        }
    }
    lv_dropdown_set_selected(s.version_dd, sel);
    _update_apply_state();
}

static void _make_dd_event_cb(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    _refresh_version_dd(NULL);
}

static void _close(bool applied) {
    ecu_picker_done_cb_t cb = s.cb;
    void *ctx = s.ctx;
    if (s.overlay && lv_obj_is_valid(s.overlay)) lv_obj_del_async(s.overlay);
    memset(&s, 0, sizeof(s));
    if (cb) cb(applied, ctx);
}

static void _apply_cb(lv_event_t *e) {
    (void)e;
    char make[48] = {0}, version[48] = {0};
    lv_dropdown_get_selected_str(s.make_dd, make, sizeof(make));

    bool applied = false;
    if (strcmp(make, CUSTOM_LABEL) == 0) {
        config_store_save_ecu("", "");
    } else {
        lv_dropdown_get_selected_str(s.version_dd, version, sizeof(version));
        const ecu_preset_t *p = ecu_preset_find(make, version);
        if (!p) {
            ESP_LOGW(TAG, "No preset found for %s / %s", make, version);
        } else if (ecu_preset_apply_to_layout(s.layout_name, p) != ESP_OK) {
            ESP_LOGE(TAG, "apply failed for %s / %s", make, version);
        } else {
            config_store_save_ecu(make, version);
            applied = true;
        }
    }
    _close(applied);
}

static void _skip_cb(lv_event_t *e) {
    (void)e;
    _close(false);
}

static lv_obj_t *_make_btn(lv_obj_t *parent, const char *text,
                           lv_color_t bg, lv_color_t fg, lv_coord_t x_ofs,
                           lv_event_cb_t cb, lv_obj_t **out_label) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 240, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, x_ofs, -10);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);
    lv_obj_set_style_text_font(lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, fg, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    if (out_label) *out_label = lbl;
    return btn;
}

static void _style_dropdown(lv_obj_t *dd) {
    lv_obj_set_style_bg_color(dd, THEME_COLOR_INPUT_BG, 0);
    lv_obj_set_style_bg_opa(dd, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(dd, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_set_style_text_font(dd, THEME_FONT_SMALL, 0);
    lv_obj_set_style_border_color(dd, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(dd, 1, 0);
    lv_obj_set_style_radius(dd, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(dd, 8, 0);
    lv_obj_set_style_text_color(dd, THEME_COLOR_TEXT_MUTED, LV_PART_INDICATOR);
}

void ecu_picker_open(const char *layout_name, bool allow_skip,
                     ecu_picker_done_cb_t cb, void *ctx) {
    if (s.overlay && lv_obj_is_valid(s.overlay)) return;

    memset(&s, 0, sizeof(s));
    s.cb = cb;
    s.ctx = ctx;
    strncpy(s.layout_name, layout_name ? layout_name : "default",
            sizeof(s.layout_name) - 1);

    char cur_make[32] = {0}, cur_ver[32] = {0};
    config_store_load_ecu(cur_make, sizeof(cur_make), cur_ver, sizeof(cur_ver));

    /* Overlay */
    lv_obj_t *scr = lv_layer_top();
    s.overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s.overlay);
    lv_obj_set_size(s.overlay, lv_pct(100), lv_pct(100));
    lv_obj_center(s.overlay);
    lv_obj_set_style_bg_color(s.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s.overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(s.overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s.overlay, LV_OBJ_FLAG_CLICKABLE);

    /* Card */
    s.card = lv_obj_create(s.overlay);
    lv_obj_set_size(s.card, CARD_W, CARD_H);
    lv_obj_center(s.card);
    lv_obj_set_style_bg_color(s.card, THEME_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(s.card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s.card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(s.card, 1, 0);
    lv_obj_set_style_radius(s.card, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(s.card, 20, 0);
    lv_obj_clear_flag(s.card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(s.card);
    lv_label_set_text(title, "Select Your ECU");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    lv_obj_t *sub = lv_label_create(s.card);
    lv_label_set_text(sub, "Auto-configures the default layout with your ECU's CAN broadcast decode.");
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_text_font(sub, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(sub, THEME_COLOR_TEXT_MUTED, 0);

    /* Make row */
    lv_obj_t *mk_lbl = lv_label_create(s.card);
    lv_label_set_text(mk_lbl, "Make");
    lv_obj_align(mk_lbl, LV_ALIGN_TOP_LEFT, 10, 80);
    lv_obj_set_style_text_font(mk_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(mk_lbl, THEME_COLOR_TEXT_MUTED, 0);

    s.make_dd = lv_dropdown_create(s.card);
    lv_obj_set_size(s.make_dd, 430, 40);
    lv_obj_align(s.make_dd, LV_ALIGN_TOP_LEFT, 100, 76);
    _style_dropdown(s.make_dd);
    {
        char buf[256];
        _build_make_options(buf, sizeof(buf));
        lv_dropdown_set_options(s.make_dd, buf);
    }
    lv_obj_add_event_cb(s.make_dd, _make_dd_event_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* Version row */
    lv_obj_t *vr_lbl = lv_label_create(s.card);
    lv_label_set_text(vr_lbl, "Version");
    lv_obj_align(vr_lbl, LV_ALIGN_TOP_LEFT, 10, 138);
    lv_obj_set_style_text_font(vr_lbl, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(vr_lbl, THEME_COLOR_TEXT_MUTED, 0);

    s.version_dd = lv_dropdown_create(s.card);
    lv_obj_set_size(s.version_dd, 430, 40);
    lv_obj_align(s.version_dd, LV_ALIGN_TOP_LEFT, 100, 134);
    _style_dropdown(s.version_dd);

    /* Preselect saved ECU. If cur_make matches a known make, pick that;
     * otherwise default to the Custom option. Then cascade to version. */
    if (cur_make[0] == '\0') {
        /* Select the "Custom / None" row (last in the dropdown). */
        uint16_t cnt = lv_dropdown_get_option_cnt(s.make_dd);
        if (cnt > 0) lv_dropdown_set_selected(s.make_dd, cnt - 1);
        _refresh_version_dd(NULL);
    } else {
        /* Find make index in the dropdown string. */
        char opts[256];
        _build_make_options(opts, sizeof(opts));
        int idx = 0, pos = 0;
        for (char *tok = strtok(opts, "\n"); tok; tok = strtok(NULL, "\n"), idx++) {
            if (strcmp(tok, cur_make) == 0) { pos = idx; break; }
        }
        lv_dropdown_set_selected(s.make_dd, pos);
        _refresh_version_dd(cur_ver[0] ? cur_ver : NULL);
    }

    /* Buttons */
    s.apply_btn = _make_btn(s.card, "Apply",
                            THEME_COLOR_ACCENT_BLUE, THEME_COLOR_TEXT_ON_ACCENT,
                            allow_skip ? 130 : 0, _apply_cb, &s.apply_label);
    if (allow_skip) {
        _make_btn(s.card, "Skip",
                  THEME_COLOR_SECTION_BG, THEME_COLOR_TEXT_MUTED,
                  -130, _skip_cb, NULL);
    }
    _update_apply_state();
}
