/**
 * font_manager.h — Dynamic TTF font loading via lv_tiny_ttf.
 *
 * Manages a two-level cache:
 *   Level 1: font family data (TTF bytes loaded from /lfs/fonts/)
 *   Level 2: font instances (lv_font_t at specific sizes)
 *
 * Font names in layout JSON use "Family:size" format (e.g. "Fugaz:28").
 * Old-style names ("fugaz_28") are still handled by widget_resolve_font()
 * for backward compatibility with compiled fonts.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FONT_MAX_FAMILIES  8
#define FONT_MAX_INSTANCES 32
#define FONT_NAME_LEN      32

/**
 * Initialise the font manager. Scans /lfs/fonts/ and pre-loads
 * family data into PSRAM.  Call once at boot (before layout load).
 */
void font_manager_init(void);

/**
 * Free all cached font instances (lv_font_t) but keep family data.
 * Call between layout loads (after old screen deleted, before new
 * widgets created) so stale font pointers are cleaned up.
 */
void font_manager_reset_instances(void);

/**
 * Free everything (family data + instances).
 * Typically not needed at runtime.
 */
void font_manager_shutdown(void);

/**
 * Get a font by family name and pixel size.
 * Returns a cached lv_font_t* or creates one via lv_tiny_ttf.
 *
 * @param family  Font family name (matches filename without .ttf extension)
 * @param size    Desired font height in pixels (8-128)
 * @return        lv_font_t* or NULL if family not found / cache full
 */
const lv_font_t *font_manager_get(const char *family, uint16_t size);

/**
 * Get the number of loaded font families.
 */
uint8_t font_manager_family_count(void);

/**
 * Get family name by index (for listing in web UI).
 * @return  Family name string, or NULL if index out of range.
 */
const char *font_manager_family_name(uint8_t index);

/**
 * Load a new font family from a TTF data buffer (e.g. after upload).
 * Copies data into PSRAM.  If family already exists, replaces it.
 *
 * @param name  Family name (stored as-is, max 31 chars)
 * @param data  Raw TTF file bytes
 * @param size  Byte count
 * @return      true on success
 */
bool font_manager_add_family(const char *name, const uint8_t *data, size_t size);

/**
 * Remove a font family and all its cached instances.
 * Also deletes the .ttf file from /lfs/fonts/.
 *
 * @param name  Family name
 * @return      true if found and removed
 */
bool font_manager_remove_family(const char *name);

#ifdef __cplusplus
}
#endif
