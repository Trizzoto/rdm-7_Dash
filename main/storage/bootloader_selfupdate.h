/*
 * bootloader_selfupdate.h — write a new 2nd-stage bootloader from the app.
 *
 * WHY THIS EXISTS
 *
 * The CAN transmit-line park that stops the dash holding the bus down for the
 * first 1.5 s of boot lives in the 2nd-stage bootloader (ADR-0047), because
 * that is the only code that runs early enough. `esp_ota_*` writes app
 * partitions only, so an ordinary OTA cannot deliver it — a dash already in a
 * customer's car would otherwise need a USB reflash for a fix that is
 * two commits old.
 *
 * This writes the bootloader region (0x0-0x8000, outside every partition)
 * directly, from an image embedded in the app. The image therefore arrives
 * with the normal OTA; nothing is downloaded at write time.
 *
 * THE RISK, PLAINLY
 *
 * There is a window during erase+write where the chip holds no valid
 * bootloader. Lose power in it and the dash will not boot. The ESP32-S3 ROM
 * bootloader is mask silicon and always offers USB download mode, so the
 * failure mode is "needs a USB reflash" — the same position the dash is in
 * before running this — but it is a real window and this must never run
 * unattended. It is deliberately not called from boot, from OTA completion,
 * or from anything automatic.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	BL_SELFUPDATE_UP_TO_DATE = 0, /* flash already holds the embedded image  */
	BL_SELFUPDATE_DIFFERENT,      /* an update is available to write         */
	BL_SELFUPDATE_UNREADABLE,     /* could not read the running bootloader   */
} bl_selfupdate_state_t;

/* Compare the bootloader in flash against the one embedded in this app.
 * Read-only — touches nothing. */
bl_selfupdate_state_t bootloader_selfupdate_check(void);

/* Size of the embedded bootloader image, in bytes. */
size_t bootloader_selfupdate_image_size(void);

/**
 * Write the embedded bootloader to 0x0 and verify it read back byte-for-byte.
 *
 * Refuses before erasing anything if the embedded image fails its sanity
 * checks (magic, size, flash mode/size header). Returns ESP_OK only after a
 * full read-back comparison has passed, so a caller that gets ESP_OK is safe
 * to reboot. Does NOT reboot on its own.
 *
 * A failure AFTER the erase has begun leaves the dash needing a USB reflash;
 * `err_detail` (optional, may be NULL) is filled with a short description so
 * the caller can say which side of that line it landed on.
 *
 * Blocks for a few hundred milliseconds with the cache disabled. Call from a
 * normal task, never from a timer or callback.
 */
esp_err_t bootloader_selfupdate_apply(const char **err_detail,
                                      bool *past_point_of_no_return);

#ifdef __cplusplus
}
#endif
