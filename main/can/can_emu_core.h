/*
 * can_emu_core.h — the part of "the dash as a keypad or IO box" that is pure
 * data: parse a device description, keep the state of its controls, and build
 * the bytes of every frame it sends. No ESP-IDF, no LVGL, no FreeRTOS — the
 * native tests link this file as it ships (tests/native/test_can_emu.c).
 *
 * The runtime half (timer, transmit, guards, storage, channels) is can_emu.c.
 * Why any of this exists: docs/CAN_EMULATION_PLAN_2026-09.md and ADR-0077.
 *
 * A DEVICE is what the ECU thinks is on its bus — a Haltech IO Box, a keypad,
 * or anything a customer's spec describes. It has:
 *
 *   controls  things a finger presses. momentary (on while held), latch (each
 *             press flips it), select (several buttons share one input, each
 *             holding it at its own state — a cruise-control voltage ladder).
 *   send      frames it transmits, each built from fields. A field takes its
 *             value from a control's state (a table: one value per state), a
 *             constant, a counter, or a live channel — in the ECU's own units,
 *             with scale/offset turning that into raw bits.
 *   listen    frames the ECU sends back, decoded into ordinary channels.
 *
 * JSON keys for bit positions are the same as the layout's signals[]
 * (can_id, bit_start, bit_length, endian 0=big 1=little, scale, offset), so
 * the channel decode editor's bit picker describes a field exactly.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CEMU_MAX_DEVICES   4
#define CEMU_MAX_CONTROLS  16
#define CEMU_MAX_STATES    8
#define CEMU_MAX_SEND      8
#define CEMU_MAX_FIELDS    12
#define CEMU_MAX_LISTEN    6
#define CEMU_MAX_CHANNELS  8

#define CEMU_ID_LEN        16
#define CEMU_NAME_LEN      32
#define CEMU_STATE_LEN     12
#define CEMU_SIG_LEN       32

/* Fastest a frame may repeat, and slowest a periodic one may. A Haltech IO
 * box sends every 20 ms; nothing needs 5. */
#define CEMU_MIN_EVERY_MS  10
#define CEMU_MAX_EVERY_MS  5000

typedef enum {
	CEMU_KIND_MOMENTARY = 0,
	CEMU_KIND_LATCH     = 1,
	CEMU_KIND_SELECT    = 2,
} cemu_kind_t;

typedef enum {
	CEMU_SRC_CONST   = 0,
	CEMU_SRC_CONTROL = 1,
	CEMU_SRC_COUNTER = 2,
	CEMU_SRC_CHANNEL = 3,
} cemu_src_t;

typedef struct {
	char    id[CEMU_ID_LEN];
	char    name[CEMU_NAME_LEN];
	uint8_t kind;                                  /* cemu_kind_t */
	uint8_t n_states;                              /* 2 for momentary/latch */
	char    states[CEMU_MAX_STATES][CEMU_STATE_LEN]; /* states[0] is the rest state */
	bool    remember;                              /* latch: survive power-off */

	/* runtime */
	uint8_t  latched;                              /* latch: 0 or 1 */
	uint8_t  holds[CEMU_MAX_STATES];               /* fingers holding each state */
	uint32_t seq[CEMU_MAX_STATES];                 /* when each state was last pressed */
} cemu_control_t;

typedef struct {
	uint8_t bit_start, bit_length, endian;
	uint8_t src;                                   /* cemu_src_t */
	uint8_t control;                               /* index, CEMU_SRC_CONTROL */
	float   scale, offset;
	float   min, max;                              /* ECU units; max < min = the field's own range */
	float   values[CEMU_MAX_STATES];               /* per control state, ECU units */
	float   value;                                 /* CONST; CHANNEL when no data */
	char    channel[CEMU_SIG_LEN];
	uint32_t counter;                              /* runtime, CEMU_SRC_COUNTER */
} cemu_field_t;

typedef struct {
	uint32_t can_id;
	bool     extd;
	uint8_t  dlc;
	uint16_t every_ms;
	uint8_t  base[8];                              /* bytes under the fields */
	uint8_t  n_fields;
	cemu_field_t fields[CEMU_MAX_FIELDS];

	/* runtime (can_emu.c) */
	uint32_t due_ms;
	uint8_t  last[8];
	uint32_t sent, failed;
} cemu_send_t;

typedef struct {
	char    name[CEMU_SIG_LEN];
	char    unit[8];
	uint8_t bit_start, bit_length, endian;
	bool    is_signed;
	float   scale, offset;
} cemu_channel_t;

typedef struct {
	uint32_t can_id;
	bool     extd;
	uint8_t  n_channels;
	cemu_channel_t channels[CEMU_MAX_CHANNELS];
} cemu_listen_t;

typedef enum {
	CEMU_ST_OFF      = 0,   /* switched off by the user */
	CEMU_ST_SENDING  = 1,
	CEMU_ST_WAITING  = 2,   /* listening before its first frame */
	CEMU_ST_PAUSED   = 3,   /* simulator, bus scan, backing off */
	CEMU_ST_REFUSED  = 4,   /* wrong bitrate, ID clash, forbidden ID */
} cemu_status_t;

typedef struct {
	char     id[CEMU_ID_LEN];
	char     name[CEMU_NAME_LEN];
	char     template_id[24];
	bool     enabled;
	uint16_t bitrate_k;                            /* 0 = any */

	uint8_t n_controls;
	cemu_control_t controls[CEMU_MAX_CONTROLS];
	uint8_t n_send;
	cemu_send_t send[CEMU_MAX_SEND];
	uint8_t n_listen;
	cemu_listen_t listen[CEMU_MAX_LISTEN];

	/* runtime (can_emu.c) */
	uint8_t  status;                               /* cemu_status_t */
	char     reason[112];
	uint32_t clash_id;
	uint32_t clash_ms;
	uint32_t start_ms;
	uint16_t consec_fail;
	bool     fail_logged;
	uint32_t backoff_until_ms;
} cemu_device_t;

typedef struct cemu_model_s {
	uint8_t       n_devices;
	cemu_device_t devices[CEMU_MAX_DEVICES];
	uint32_t      seq;                             /* press order, for select */
} cemu_model_t;

/* Parse a whole can_emu.json. On failure returns false and writes a sentence
 * a person can act on into err ("Device 2, frame 0x2C0: bit 60 + 16 bits runs
 * past the end of an 8-byte frame."). `out` is fully overwritten either way. */
bool cemu_parse(const char *json, cemu_model_t *out, char *err, size_t err_len);

/* "device:control" or "device:control:state". state_idx is -1 when the ref
 * names no state. False when the device or control doesn't exist, or the
 * state isn't one of the control's. */
bool cemu_resolve(const cemu_model_t *m, const char *ref,
                  int *dev_idx, int *ctl_idx, int *state_idx);

/* A finger goes down / comes up. For momentary and select `state` is the
 * state held (-1 means "on", state 1); a latch flips on press and ignores
 * release. */
void cemu_press(cemu_model_t *m, cemu_control_t *c, int state);
void cemu_release(cemu_control_t *c, int state);

/* Set a latch outright (a switch widget knows which way it went). */
void cemu_set_latch(cemu_control_t *c, bool on);

/* Every momentary and select control back to rest. Latches keep their state. */
void cemu_release_all(cemu_model_t *m);

/* The state a control is in right now: for select, the most recently pressed
 * state still held, else 0. */
uint8_t cemu_control_state(const cemu_control_t *c);

/* Live channel lookup for CEMU_SRC_CHANNEL fields. Return false for "no data". */
typedef bool (*cemu_channel_fn)(const char *name, float *value, void *user);

/* Build one frame's bytes. Advances counter fields. Returns the DLC. */
uint8_t cemu_compose(const cemu_device_t *d, cemu_send_t *s,
                     cemu_channel_fn fn, void *user, uint8_t out[8]);

/* The same bytes without advancing anything — what the frame would say right
 * now, for a status readout that must not disturb the real sequence. */
uint8_t cemu_peek(const cemu_device_t *d, const cemu_send_t *s,
                  cemu_channel_fn fn, void *user, uint8_t out[8]);

/* Engineering value → raw field bits: round((v - offset) / scale), clamped to
 * what the field can hold (unsigned). */
uint32_t cemu_to_raw(float v, float scale, float offset, uint8_t bit_length);

/* Write `value` into [bit_start, bit_length) REPLACING whatever bits were
 * there (can_pack_bits ORs; a frame built over a base can't). */
void cemu_put_bits(uint8_t *data, uint8_t bit_start, uint8_t bit_length,
                   uint32_t value, uint8_t endian);

/* Make a device from a template in can_emu_templates.json: "{V}"/"{v}" in any
 * string become the variant key (upper/lower case), and every can_id moves by
 * the variant's id_offset (Box B is Box A + 1). `id` replaces the device id
 * when given (so a second copy doesn't clash). Returns a malloc'd JSON object
 * string, or NULL with err set. The web editor does the same in JS. */
char *cemu_template_device(const char *templates_json, const char *template_id,
                           const char *variant_key, const char *id,
                           char *err, size_t err_len);

/* ── Old button/toggle outputs ─────────────────────────────────────────────
 * A widget's own tx_can_id/tx_bit_* fields, kept working, but composed per ID
 * so two widgets on one frame no longer wipe each other's bits. */
#define CEMU_MAX_LEGACY 32

typedef struct {
	void    *owner;                /* the widget; NULL = free slot */
	uint32_t can_id;
	uint8_t  bit_start, bit_length, endian;
	uint8_t  rate_hz;              /* 0 = once per change */
	bool     on;
	uint8_t  off_burst;            /* OFF frames still owed */
} cemu_legacy_t;

/* Bytes of the frame on `can_id` with every legacy output that writes into it. */
void cemu_legacy_compose(const cemu_legacy_t *slots, int n, uint32_t can_id,
                         uint8_t out[8]);

#ifdef __cplusplus
}
#endif
