/*
 * channel_math.h — derived ("calculated") channels.
 *
 * A math channel computes its value from two other channels instead of a
 * CAN/OBD2/internal source:   boost = manifold_pressure - barometric_pressure.
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
 *   - If either operand is unbound or stale the evaluator skips the push;
 *     the output then goes stale through the regular 2 s signal timeout.
 *   - Config persists in channels.json as  "math": {"a","b","op"}  (additive
 *     key — older firmware ignores it).
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

/* One math operand: a channel id OR a numeric constant. Constants are
 * interpreted in the resolved unit of the other (channel) operand. */
typedef struct {
	const char *channel_id;   /* non-NULL/non-empty → channel operand */
	bool        is_const;
	float       value;        /* used when is_const */
} channel_math_operand_t;

/** Compose the channel's synthetic signal name ("MATH_<ID>", uppercased)
 *  into buf. Returns buf. */
const char *channel_math_signal_name(const channel_t *c, char *buf, size_t cap);

/**
 * Configure @p c as a math channel: a <op> b. At least one operand must be
 * a channel; channel operands must exist and must not reference @p c itself
 * (chains through OTHER math channels are fine — they evaluate one tick
 * behind). Registers the MATH_<ID> signal and binds the channel to it
 * (persists via set_signal). Returns false on validation failure.
 *
 * Unit semantics (evaluator):
 *   - channel ∘ channel with different but convertible native units:
 *     B is converted into A's unit before the op.
 *   - For + and −, or whenever one operand is a constant, the result is
 *     then converted from the channel-operand unit into THIS channel's
 *     units_native (when convertible) so the output unit is honoured.
 *   - channel × / ÷ channel results are NOT output-converted (the product's
 *     dimension differs from either operand; a linear convert would
 *     silently mis-scale). The output unit is the user's responsibility.
 *   - Unknown/unit-less pairs pass through unchanged.
 */
bool channel_math_set(channel_t *c, const channel_math_operand_t *a,
                      const channel_math_operand_t *b, uint8_t op);

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
