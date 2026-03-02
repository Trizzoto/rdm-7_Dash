SET(SOURCES components/ui_comp_slider2.c
    components/ui_comp.c
    screens/ui_Screen1.c
    screens/ui_Screen2.c
    screens/ui_Screen3.c
    screens/ui_Screen4.c
    ui.c
    ui_preconfig.c
    config/create_config_controls.c
    callbacks/ui_callbacks.c
    menu/menu_screen.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_daihatsu_logo_png.c
    images/ui_img_indicator_left_png.c
    images/ui_img_indicator_right_png.c
    images/ui_img_handbrake_png.c
    images/ui_img_check_engine_png.c
    images/ui_img_launch_control_png.c
    images/ui_img_high_beam_png.c
    images/ui_img_Smart_car_key.c
	images/ui_img_669363031.c
	images/ui_img_rev_0_sticker_png.c
	images/ui_img_rev_0_sticker2_png.c
	images/ui_img_rev_0_sticker3_png.c
    ui_temporary_image.c
    fonts/ui_font_Manrope_16_BOLD.c
    fonts/ui_font_Manrope_35_BOLD.c
    fonts/ui_font_Manrope_54_BOLD.c
    fonts/ui_font_fugaz_14.c
    fonts/ui_font_fugaz_17.c
    fonts/ui_font_fugaz_18.c
    fonts/ui_font_fugaz_28.c
    fonts/ui_font_fugaz_31.c
    fonts/ui_font_fugaz_56.c
    
    signals.h

add_library(ui ${SOURCES})
