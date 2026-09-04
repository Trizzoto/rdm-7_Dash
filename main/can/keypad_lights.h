/* keypad_lights.h — play a Studio-designed keypad boot with no laptop.
 *
 * A Blink keypad cannot animate itself. Its only self-driven show is the
 * factory one in CANopen object 2014h — three fixed choices, none of them
 * yours. Everything else has to be fed to it frame by frame by a host, which
 * until now meant RDM Studio open on a laptop with the window in front of
 * you. That is a bench feature, and the boot people actually want is the one
 * that plays when they turn the key in a car with no laptop in it.
 *
 * The dash is already the host: it is on the same bus, it is powered by the
 * same ignition, and it transmits. So it plays the file.
 *
 * WHAT THIS IS NOT. It is not the keypad manager from the platform plan —
 * there is no button state machine here, no channels, no LED bindings. It is
 * a tape player: Studio bakes the boot to timed CAN frames (it already does,
 * for the "Frames" export), the dash stores the tape and plays it once at
 * power-up. Everything clever stays in Studio, where it can be seen while it
 * is being made.
 *
 * The tail of the tape means one of two things, and the file says which:
 *
 *   loop = false  the boot ends by handing the rings back. The last frame IS
 *                 that hand-back — every ring the buttons are not lighting,
 *                 dark — and the player stops transmitting after it, leaving
 *                 the rings to whatever normally drives them (usually the
 *                 ECU). Silence is the point.
 *   loop = true   the boot ends on an animation. The frames from loop_from_ms
 *                 are one turn of it, and the player repeats them until told
 *                 to stop, because replaying an animation once and stopping
 *                 leaves the keypad frozen on whichever frame it ended on.
 *
 * See rdm7-desktop/docs/KEYPAD_LIGHTSHOW_2026-09.md §6 and ADR-0059.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded so a bad or hostile file cannot eat the heap: at Studio's 12 fps
 * ceiling this is ~30 s of animation, and the longest boot it will let you
 * build is 8 steps of 8 s with a 4 s tail. */
#define KEYPAD_LIGHTS_MAX_FRAMES 384

typedef struct {
	bool     present;      /* a file is stored                                */
	bool     playing;      /* the player task is running right now            */
	bool     enabled;      /* play it at power-up                             */
	bool     loop;         /* the tail is an animation, not a hand-back       */
	uint16_t frames;       /* how many frames the tape holds                  */
	uint16_t ms;           /* how long it runs, boot + one turn of the tail   */
	uint8_t  node;         /* CANopen node the frames are addressed to        */
	uint16_t baud;         /* the bitrate the file was made for, in kbit      */
	char     keypad[24];   /* which keypad it was made for, for the UI        */
	char     reason[96];   /* why it did not play, when it did not            */
} keypad_lights_status_t;

/* Read the stored file (if any) into memory. Safe to call before CAN is up;
 * it touches only LittleFS. Called once from app_main. */
esp_err_t keypad_lights_init(void);

/* Replace the stored file. `json` is the document Studio uploads; it is
 * validated and only then written, so a bad upload can never replace a
 * working one. Returns ESP_ERR_INVALID_ARG with nothing changed if it does
 * not parse or carries no usable frames. */
esp_err_t keypad_lights_store(const char *json, size_t len);

/* Forget it. Removes the file and stops any playback. */
esp_err_t keypad_lights_forget(void);

/* Play the stored boot now. Non-blocking: spawns a short-lived task that
 * transmits on the tape's own clock. Playing again while playing restarts.
 * `why` (optional) receives the refusal when it returns an error. */
esp_err_t keypad_lights_play(char *why, size_t why_len);

/* Stop a playback in progress. Returns quickly; the task exits on its own. */
void keypad_lights_stop(void);

/* Play it at power-up, or do not. Persisted with the file. */
esp_err_t keypad_lights_set_enabled(bool enabled);

/* Called once from the boot sequence, after CAN is up and the keypad has had
 * time to finish its own start-up show. Plays only if a file is stored, it is
 * enabled, and the dash's bus is on the bitrate the file was made for. */
void keypad_lights_boot_play(void);

void keypad_lights_get_status(keypad_lights_status_t *out);

#ifdef __cplusplus
}
#endif
