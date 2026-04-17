#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Full LittleFS path of the embedded RDM logo. */
#define BOOT_ASSETS_RDM_LOGO_PATH "/lfs/images/RDM.rdmimg"

/** Image name (without extension) as referenced by layouts. */
#define BOOT_ASSETS_RDM_LOGO_NAME "RDM"

/**
 * Seed firmware-embedded default assets into LittleFS if they are missing.
 *
 * Currently seeds:
 *   - /lfs/images/RDM.rdmimg  (built-in RDM logo)
 *
 * Safe to call on every boot — files are only written when missing, so
 * user-uploaded replacements are preserved. Must be called AFTER LittleFS
 * has been mounted.
 */
esp_err_t boot_assets_seed_defaults(void);

/**
 * Return true if the given image name is a protected built-in asset that
 * should not be user-deleted (e.g. the RDM logo). The comparison is
 * case-sensitive and matches the bare name without the .rdmimg extension.
 */
bool boot_assets_is_protected_image(const char *name);

#ifdef __cplusplus
}
#endif
