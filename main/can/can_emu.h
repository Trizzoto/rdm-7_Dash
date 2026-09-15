/*
 * can_emu.h — the dash plays a CAN device: a Haltech IO Box, a keypad, or
 * anything a spec describes (ADR-0077, docs/CAN_EMULATION_PLAN_2026-09.md).
 *
 * The devices live in /lfs/can_emu.json — per car, not per layout, so moving
 * from the street page to the track page never unplugs the ECU's IO box. The
 * bytes are built by can_emu_core.c; this file owns the clock, the bus and
 * the rules about when not to use it:
 *
 *  - One LVGL timer. If the UI freezes, frames stop, and the ECU sees its box
 *    go missing — never a box stuck sending "SET".
 *  - Single-shot, non-blocking transmits. A periodic frame that misses a slot
 *    is resent by the next; the render thread never waits on the bus.
 *  - A device doesn't start on the wrong bitrate, on an ID the dash must not
 *    send (OBD2 requests, the RDM device bus), or while something else is
 *    already sending one of its IDs — the ESP32 never hears its own frames,
 *    so hearing one means a real box is fitted.
 *  - The simulator never reaches an ECU: while it runs, a field fed from a
 *    channel sends its "no data" value. Buttons still work.
 *
 * Buttons, toggles and (later) the keypad widget press controls by name —
 * "device:control" or "device:control:state".
 *
 * Unless marked otherwise, call on the LVGL task or with the LVGL lock held.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAN_EMU_PATH       "/lfs/can_emu.json"
#define CAN_EMU_MAX_BYTES  (24 * 1024)
#define CEMU_FILTER_MAX    32

/** Load the stored devices once, start the clock, and (every call) register
 *  the channels devices listen to. From dashboard_init, before widgets. */
void can_emu_init(void);

/** Validate and store a whole can_emu.json, then apply it. Any task. On a
 *  refusal nothing changes and `err` says why, in words. */
esp_err_t can_emu_store(const char *json, char *err, size_t err_len);

/** The stored file (malloc'd; "{"v":1,"devices":[]}" when there is none). Any task. */
char *can_emu_read_file(void);

/** The built-in templates (can_emu_templates.json, embedded). Any task. */
const char *can_emu_templates(void);

/** Add a device from a template (Box A/B = variant), switched on. The id is
 *  made unique. LVGL task. */
esp_err_t can_emu_add_template(const char *template_id, const char *variant,
                               char *err, size_t err_len);

/** Switch a device on or off, or remove it (rewrites the stored file). */
esp_err_t can_emu_set_enabled(const char *device_id, bool enabled);
esp_err_t can_emu_remove(const char *device_id);

/* ── controls ─────────────────────────────────────────────────────────── */

/** Does this ref name a control that exists right now? */
bool can_emu_ref_exists(const char *ref);

/** Is it a latch (each press flips it; the engine remembers which way)? */
bool can_emu_ref_is_latch(const char *ref);

/** A finger down / up on a control. False when the ref doesn't resolve. */
bool can_emu_press(const char *ref);
void can_emu_release(const char *ref);

/** Set a latch outright. */
bool can_emu_set_latch(const char *ref, bool on);

/** Is the control named by ref in that ref's state right now? (For a latch or
 *  momentary without a state: is it on.) */
bool can_emu_is_active(const char *ref);

/** Hold-to-test from Studio / the settings popup: held=true presses and keeps
 *  holding for 1.5 s after the last call; held=false lets go. */
bool can_emu_test_hold(const char *ref, bool held);

/** Every momentary and select control back to rest. */
void can_emu_release_all(void);

/* ── status ───────────────────────────────────────────────────────────── */

typedef struct {
	uint8_t devices;
	uint8_t enabled;
	uint8_t sending;
	uint8_t problems;   /* refused */
	char    first_problem[112];
} can_emu_summary_t;

void can_emu_summary(can_emu_summary_t *out);

/** The live model, read-only, for the settings popup (LVGL task). NULL before
 *  init. Its layout changes whenever the file is re-applied — watch
 *  can_emu_generation() and rebuild rather than keeping pointers across it. */
struct cemu_model_s;
const struct cemu_model_s *can_emu_model(void);
uint32_t can_emu_generation(void);

/** Live status of every device: state, reason, per-frame counters and last
 *  bytes, control states, and the channels it listens to. */
cJSON *can_emu_status_json(void);

/** One request from HTTP or USB serial — a whole file, or an action:
 *  test / set / add / enable / remove / release_all (see web_server_can.c).
 *  `text` is the request's own JSON text (stored verbatim for a file). Any
 *  task WITHOUT the LVGL lock; takes it for actions. */
bool can_emu_request(const cJSON *req, const char *text, char *err, size_t err_len);

/** The reply both transports send: {ok, error?, bitrate_k, file, status,
 *  templates?}. Any task without the LVGL lock. */
cJSON *can_emu_report(bool ok, const char *err, bool templates);

/* ── hooks ─────────────────────────────────────────────────────────────── */

/** From can_process_queued_frames: every received frame. */
void can_emu_on_can_frame(uint32_t id, bool extd);

/** From build_twai_filter_from_signals: IDs the devices send, so a clash can
 *  be heard. Returns the count written; *has_ext set if any is 29-bit. Any
 *  task (reads a snapshot). */
int can_emu_filter_ids(uint32_t *ids, int max, bool *has_ext);

/* ── old button/toggle outputs, composed per ID ─────────────────────────── */

/** A widget's tx_* output turns on or off. rate_hz 0 sends once per change;
 *  turning off sends three OFF frames, as the widgets always did. */
void can_emu_legacy_set(void *owner, uint32_t can_id, uint8_t bit_start,
                        uint8_t bit_length, uint8_t endian, uint8_t rate_hz,
                        bool on);

/** The widget is going away. send_off: drive its bits OFF first. */
void can_emu_legacy_forget(void *owner, bool send_off);

#ifdef __cplusplus
}
#endif
