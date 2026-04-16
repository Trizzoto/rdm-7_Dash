/* first_run_wizard.c — see header.
 *
 * Design choices:
 *  - Pure LVGL overlay on top of the dashboard screen (not a separate screen),
 *    so dismissal simply deletes the overlay object.
 *  - Three action buttons lead to EXISTING screens (device_settings_with_return_screen,
 *    wifi_ui_show) rather than reimplementing their contents. Saves ~500 lines
 *    and keeps behaviour consistent with the rest of the app.
 *  - NVS flag first_run_done is set when the user dismisses (Finish or skip).
 *    This lets repeat visits to Settings not re-trigger the wizard, while still
 *    allowing a factory reset to re-arm it. */

#include "first_run_wizard.h"

#include "lvgl.h"
#include "esp_log.h"

#include "../theme.h"
#include "../../storage/config_store.h"
#include "../../system/device_id.h"
#include "../settings/device_settings.h"
#include "ui_wifi.h"
#include "ui_Screen3.h"

static const char *TAG = "first_run";
static lv_obj_t *s_overlay = NULL;

static void _mark_done_and_close(void) {
    (void) config_store_save_first_run_done(true);
    if (s_overlay && lv_obj_is_valid(s_overlay)) {
        lv_obj_del_async(s_overlay);
    }
    s_overlay = NULL;
    ESP_LOGI(TAG, "First-run wizard dismissed, flag set");
}

static void _btn_scan_can_cb(lv_event_t *e) {
    (void)e;
    /* Open device settings with return-screen = current (dashboard). The user
       taps Scan CAN Bus from the settings page — single tap away. Closing
       device settings drops them back on the wizard overlay, which they can
       then dismiss. */
    _mark_done_and_close();
    device_settings_with_return_screen(ui_Screen3);
}

static void _btn_wifi_cb(lv_event_t *e) {
    (void)e;
    _mark_done_and_close();
    wifi_ui_show();
}

static void _btn_finish_cb(lv_event_t *e) {
    (void)e;
    _mark_done_and_close();
}

void show_first_run_wizard(void) {
    if (s_overlay && lv_obj_is_valid(s_overlay)) return; /* already showing */

    /* Full-screen translucent overlay on the currently-active screen */
    lv_obj_t *scr = lv_scr_act();
    s_overlay = lv_obj_create(scr);
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, lv_pct(100), lv_pct(100));
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_80, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_foreground(s_overlay);

    /* Card */
    lv_obj_t *card = lv_obj_create(s_overlay);
    lv_obj_set_size(card, 520, 360);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, THEME_COLOR_PANEL, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, THEME_RADIUS_NORMAL, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "Welcome to RDM-7");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_text_font(title, THEME_FONT_LARGE, 0);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, 0);

    /* Serial badge */
    char serial[MAX_SERIAL_LENGTH] = "";
    (void) get_device_serial(serial);
    lv_obj_t *badge = lv_label_create(card);
    lv_label_set_text_fmt(badge, "Device %s", serial);
    lv_obj_align(badge, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_text_font(badge, THEME_FONT_TINY, 0);
    lv_obj_set_style_text_color(badge, THEME_COLOR_TEXT_MUTED, 0);

    /* Instructions */
    lv_obj_t *body = lv_label_create(card);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, lv_pct(90));
    lv_label_set_text(body,
        "Let's get you set up. These steps take under a minute. "
        "The Wi-Fi hotspot 'RDM7-XXXX' (password: rdm7dash) is already on "
        "if you want to jump straight to the web UI at 192.168.4.1.");
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 70);
    lv_obj_set_style_text_font(body, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(body, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);

    /* Action buttons */
    lv_obj_t *btn_scan = lv_btn_create(card);
    lv_obj_set_size(btn_scan, 460, 44);
    lv_obj_align(btn_scan, LV_ALIGN_TOP_MID, 0, 170);
    lv_obj_set_style_bg_color(btn_scan, THEME_COLOR_ACCENT_BLUE, 0);
    lv_obj_set_style_radius(btn_scan, THEME_RADIUS_NORMAL, 0);
    lv_obj_t *lbl_scan = lv_label_create(btn_scan);
    lv_label_set_text(lbl_scan, "1.  Auto-detect CAN bitrate");
    lv_obj_center(lbl_scan);
    lv_obj_set_style_text_font(lbl_scan, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl_scan, THEME_COLOR_TEXT_ON_ACCENT, 0);
    lv_obj_add_event_cb(btn_scan, _btn_scan_can_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_wifi = lv_btn_create(card);
    lv_obj_set_size(btn_wifi, 460, 44);
    lv_obj_align(btn_wifi, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_set_style_bg_color(btn_wifi, THEME_COLOR_SECTION_BG, 0);
    lv_obj_set_style_border_width(btn_wifi, 1, 0);
    lv_obj_set_style_border_color(btn_wifi, THEME_COLOR_BORDER, 0);
    lv_obj_set_style_radius(btn_wifi, THEME_RADIUS_NORMAL, 0);
    lv_obj_t *lbl_wifi = lv_label_create(btn_wifi);
    lv_label_set_text(lbl_wifi, "2.  Join your Wi-Fi network");
    lv_obj_center(lbl_wifi);
    lv_obj_set_style_text_font(lbl_wifi, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl_wifi, THEME_COLOR_TEXT_PRIMARY, 0);
    lv_obj_add_event_cb(btn_wifi, _btn_wifi_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_finish = lv_btn_create(card);
    lv_obj_set_size(btn_finish, 460, 36);
    lv_obj_align(btn_finish, LV_ALIGN_TOP_MID, 0, 274);
    lv_obj_set_style_bg_opa(btn_finish, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_finish, 0, 0);
    lv_obj_set_style_shadow_width(btn_finish, 0, 0);
    lv_obj_t *lbl_finish = lv_label_create(btn_finish);
    lv_label_set_text(lbl_finish, "Skip — I'll configure later");
    lv_obj_center(lbl_finish);
    lv_obj_set_style_text_font(lbl_finish, THEME_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl_finish, THEME_COLOR_TEXT_MUTED, 0);
    lv_obj_add_event_cb(btn_finish, _btn_finish_cb, LV_EVENT_CLICKED, NULL);

    ESP_LOGI(TAG, "First-run wizard shown");
}
