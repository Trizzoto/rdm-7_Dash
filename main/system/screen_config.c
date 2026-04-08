/**
 * screen_config.c — Runtime accessor for the screen hardware profile.
 */
#include "screen_config.h"

static const screen_profile_t s_profile = {
    .width    = SCREEN_W,
    .height   = SCREEN_H,
    .origin_x = SCREEN_ORIGIN_X,
    .origin_y = SCREEN_ORIGIN_Y,
    .shape    = SCREEN_SHAPE,
};

const screen_profile_t *screen_get_profile(void) {
    return &s_profile;
}
