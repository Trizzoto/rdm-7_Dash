// ui_Screen1.h
#ifndef UI_SCREEN1_H
#define UI_SCREEN1_H

#include "lvgl.h"
#include "driver/twai.h"

void create_rpm_container();
void update_panel_value_from_can(uint32_t panel_index);
#define NUM_PANELS 13  // Define the number of panels here
extern int selected_signals[NUM_PANELS];  // Declare the selected_signals array for sharing between files

void update_panel_value(int panel, float value);  // Declaration of the update function
void process_can_message(const twai_message_t *message);
void init_can_to_panel_map();

// Add other function declarations here...

#endif // UI_SCREEN1_H
