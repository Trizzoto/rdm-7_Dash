#include "ui.h"
#include "../theme.h"
#include "ui_helpers.h"
#include "ui_Screen3.h"
#include "esp_timer.h"

// Forward declarations
void ui_Screen3_screen_init(void);

static lv_obj_t * splash_screen = NULL;
static esp_timer_handle_t splash_timer = NULL;

// Timer callback for splash screen
static void splash_timer_cb(void * arg) {
    // Initialize and load the main screen
    ui_Screen3_screen_init();
    
    // Load Screen3 directly without animation
    lv_scr_load(ui_Screen3);
    
    // Delete the splash screen
    if (splash_screen) {
        lv_obj_del(splash_screen);
        splash_screen = NULL;
    }
    
    // Delete the timer
    if (splash_timer) {
        esp_timer_delete(splash_timer);
        splash_timer = NULL;
    }
}

// Function to show splash screen
void show_splash_screen(void) {
    // Create a new screen for the splash
    splash_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(splash_screen, THEME_COLOR_BG, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(splash_screen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(splash_screen, LV_OBJ_FLAG_SCROLLABLE);

    // Create and center the RDM logo
    lv_obj_t * logo = lv_img_create(splash_screen);
    lv_img_set_src(logo, &ui_img_RDM_Light);
    // Set zoom to 125% (320 = 1.25x size, 256 = no zoom/100%)
    lv_img_set_zoom(logo, 320);
    // Set object size to content so it matches the zoomed image
    lv_obj_set_size(logo, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(logo);

    // Load the splash screen
    lv_scr_load(splash_screen);

    // Create ESP timer configuration
    esp_timer_create_args_t timer_args = {
        .callback = splash_timer_cb,
        .name = "splash_timer"
    };

    // Create and start the timer - reduced time for faster CAN data display
    esp_timer_create(&timer_args, &splash_timer);
    esp_timer_start_once(splash_timer, 900000); // 0.9 seconds in microseconds (reduced from 1.2s)
} 