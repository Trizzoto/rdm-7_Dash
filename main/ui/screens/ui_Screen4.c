#include "ui.h"
#include "../theme.h"



// Event handler for the "Back" button on ui_Screen4
static void back_to_screen1_event_handler(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_disp_load_scr(ui_Screen1); // Navigate back to screen1
    }
}


void ui_Screen4_screen_init(void)
{
	    // Initialize screen4
    ui_Screen4 = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(ui_Screen4, THEME_COLOR_SURFACE_ALT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_Screen4, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Add a title to screen4
    lv_obj_t *title = lv_label_create(ui_Screen4);
    lv_label_set_text(title, "Setup Data Logging");
    lv_obj_set_style_text_font(title, THEME_FONT_XLARGE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_text_color(title, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);


	// Add a "Back" button at the bottom
	lv_obj_t *btn_back = lv_btn_create(ui_Screen4);
	lv_obj_set_size(btn_back, 100, 50);
	lv_obj_align(btn_back, LV_ALIGN_BOTTOM_LEFT, 20, -20);
	lv_obj_t *label_back = lv_label_create(btn_back);
	lv_label_set_text(label_back, "Back");
	lv_obj_set_style_text_color(label_back, THEME_COLOR_TEXT_PRIMARY, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_text_font(label_back, THEME_FONT_BODY, LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_center(label_back);
	lv_obj_add_event_cb(btn_back, back_to_screen1_event_handler, LV_EVENT_CLICKED, NULL);

}