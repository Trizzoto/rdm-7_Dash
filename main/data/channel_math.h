/*
 * channel_math.h — derived ("calculated") channels.
 *
 * A math channel computes its value from other channels instead of a
 * CAN/OBD2/internal source:   boost = manifold_pressure - barometric_pressure.
 *
 * Two operands, optionally three:  (a op b) op2 c  — strictly left to right,
 * no precedence, because that is how the two-row form on screen reads. The
 * third term exists for the shape a two-operand expression cannot express:
 * fuel flow ÷ speed is L/km, and the figure a driver wants, L/100km, is that
 * × 100.
 *
 * Model:
 *   - Operands are CHANNEL ids. Each operand's live value is read from the
 *     signal that channel is bound to, so a re-decode/rebind of an operand
 *     keeps the math working.
 *   - The math channel owns a synthetic registry signal named MATH_<ID>
 *     (SIGNAL_SOURCE_INTERNAL). A 5 Hz LVGL timer evaluates every enabled
 *     math channel and pushes the result via signal_set_external_value(),
 *     so widgets bound to the channel update through the normal signal
 *     pipeline (peaks, staleness, zone evaluation all included).
 *   - If any operand is unbound or stale the evaluator skips the push;
 *     the output then goes stale through the regular 2 s signal timeout.
 *     Dividing by (near) zero does the same — which is why an economy
 *     channel reads stale when the car is stopped, rather than infinite.
 *   - Config persists in channels.json as
 *       "math": {"a","b","op"[,"c","op2"]}
 *     (additive keys — older firmware ignores them and reads the first
 *     two operands, and this firmware reads a two-operand block as one).
 *
 * Threading: everything here runs on the LVGL task (timer cb + API calls
 * from the web server already marshalled under the LVGL lock).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "channel_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	CH_MATH_ADD = 0,
	CH_MATH_SUB = 1,
	CH_MATH_MUL = 2,
	CH_MATH_DIV = 3,
} channel_math_op_t;

/* One math operand: a channel id OR a numeric constant. Constants carry no
 * unit — they adopt whatever the running expression is already in. */
typedef struct {
	const char *channel_id;   /* non-NULL/non-empty → channel operand */
	bool        is_const;
	float       value;        /* used when is_const */
} channel_math_operand_t;

/** Compose the channel's synthetic signal name ("MATH_<ID>", uppercased)
 *  into buf. Returns buf. */
const char *channel_math_signal_name(const channel_t *c, char *buf, size_t cap);

/**
 * Configure @p c as a math channel:  (a <op> b) <op2> cc.
 *
 * @p cc may be NULL for the two-operand form, in which case @p op2 is
 * ignored. At least one operand must be a channel; channel operands must
 * exist and must not reference @p c itself (chains through OTHER math
 * channels are fine — they evaluate one tick behind). Registers the
 * MATH_<ID> signal and binds the channel to it (persists via set_signal).
 * Returns false on validation failure.
 *
 * Unit semantics (evaluator). The expression carries a running unit, which
 * starts as the first operand's and is updated term by term:
 *   - + and − : an operand in a different but convertible unit is brought
 *     into the running unit first, so kPa − psi means what it should. The
 *     running unit is unchanged.
 *   - × and ÷ by a CONSTANT: scaling keeps the dimension, so the running
 *     unit is unchanged. This is what makes the × 100 in an L/100km
 *     expression harmless.
 *   - × and ÷ by a CHANNEL: the dimension is now something neither
 *     operand's unit names (L/h ÷ km/h is not L/h), so the running unit is
 *     dropped and stays dropped.
 *   - At the end, a still-known running unit is converted into THIS
 *     channel's units_native when that conversion is known; a dropped one
 *     is emitted as-is and the output unit is the user's to label.
 */
bool channel_math_set(channel_t *c, const channel_math_operand_t *a,
                      const channel_math_operand_t *b, uint8_t op,
                      const channel_math_operand_t *cc, uint8_t op2);

/**
 * Disable the math source on @p c and unbind it from the synthetic signal
 * (the channel reverts to unbound; the user can pick a normal source).
 */
bool channel_math_clear(channel_t *c);

/**
 * (Re-)register the MATH_<ID> signal for every math-enabled channel.
 * Called from channel_manager_register_decoded_signals() so every load /
 * reload path that re-registers CAN decodes also restores math outputs.
 * Safe to call repeatedly (signal_register upserts).
 */
void channel_math_register_signals(void);

/** Start the 5 Hz evaluator timer. Idempotent; runs for the life of the
 *  firmware (a tick over zero math channels is a no-op loop). */
void channel_math_start(void);

#ifdef __cplusplus
}
#endif
