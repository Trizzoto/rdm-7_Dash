/**
 * layout_switcher.h — Prev/next layout cycling for the dash arrow buttons.
 *
 * The cycle source is the comma-separated list stored in NVS by
 * config_store_save_layout_switcher. When that key is empty (or every
 * pinned name is stale and no longer exists on LittleFS), the cycle
 * falls back to "all non-system layouts in filesystem order" so the
 * arrows still do something useful out of the box.
 *
 * All functions are thread-safe with respect to NVS reads; no global
 * state is kept beyond the NVS key itself. Layout switching happens
 * in the caller (these only resolve names).
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Resolve the layout that should follow @p current in the switcher cycle.
 * Wraps at the end. Writes into @p out (cap bytes), returns true on
 * success. Returns false (and leaves out unchanged) when the cycle has
 * fewer than two entries — caller can hide the arrow in that case.
 *
 * If @p current isn't in the cycle, returns the first entry so a single
 * tap from a non-pinned layout still moves the user somewhere defined.
 */
bool layout_switcher_get_next(const char *current, char *out, size_t cap);

/** Mirror of get_next, walking backward. */
bool layout_switcher_get_prev(const char *current, char *out, size_t cap);

/** Number of entries in the effective cycle (NVS list if set, otherwise
 *  the filesystem fallback). Used by UI to gate arrow visibility. */
int  layout_switcher_count(void);

#ifdef __cplusplus
}
#endif
