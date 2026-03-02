#ifndef CONFIG_CONTROLS_H
#define CONFIG_CONTROLS_H

#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Function declaration
void create_config_controls(lv_obj_t * parent, uint8_t value_id);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_CONTROLS_H */