/*
 * can_emu_settings.h — Device Settings → Your car → Keypads & IO boxes.
 *
 * The on-glass half of can_emu (ADR-0077): see each device the dash is
 * playing, whether it is on the bus and why not, switch it on or off, hold
 * its buttons to test them, and add one of the built-in templates. Writing a
 * spec from scratch is the web editor's job — not a 7" touchscreen's.
 */
#pragma once

#include <stddef.h>
#include "kit/ui_kit.h"

#ifdef __cplusplus
extern "C" {
#endif

void can_emu_settings_open(void);
void can_emu_settings_close(void);

/** The tile's status line: "2 on", "Off", "Check", "Add one". */
void can_emu_settings_stat(char *buf, size_t cap, uk_tone_t *tone);

#ifdef __cplusplus
}
#endif
