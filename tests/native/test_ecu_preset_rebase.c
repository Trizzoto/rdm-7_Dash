/* test_ecu_preset_rebase.c — host coverage of ECU preset re-basing.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * A Link generic dash stream is not on a fixed id: the tuner types the id
 * into PCLink. A car running two dashes off one bus therefore needs a second
 * generic stream on a spare id, with the RDM decoding it exactly as it would
 * the default — same signals, same order. That is what re-basing does, and
 * getting the arithmetic wrong means every channel silently decodes garbage
 * (or nothing) with no error raised anywhere.
 *
 * The shift itself is three lines; the parts worth pinning down are the
 * edges: unsupported slots must NOT move (shifting can_id 0 would invent a
 * binding the ECU never broadcasts), the stream must stay inside the 11-bit
 * id space with all of its rows, and a preset whose ids are fixed must not
 * be shifted at all.
 *
 * Mirrors the math rather than linking ecu_presets.c, for the reason
 * test_ecu_preset_match.c gives next door: that file pulls layout_manager,
 * cJSON, obd2 and can_id_tracker in behind it. Source of truth, must stay
 * in lockstep:
 *
 *   main/layout/ecu_presets.c
 *     ecu_preset_id_span():                max(can_id - base_id) over the
 *                                          supported rows; 0 if base_id==0
 *     ecu_preset_apply_to_layout_based():  shift applies only when
 *                                          preset->base_id != 0 AND
 *                                          base_id != 0 AND the base differs;
 *                                          rejects base > 0x7FF and
 *                                          base + span > 0x7FF;
 *                                          rows with can_id 0 stay 0
 */
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ECU_BASE_ID_MAX 0x7FFu
#define ROWS_MAX 24

/* ── Host mirror ────────────────────────────────────────────────────── */

typedef struct {
    uint32_t base_id;              /* 0 = ids are fixed */
    uint32_t can_id[ROWS_MAX];     /* 0 = slot unsupported */
    int      n;
} mirror_preset_t;

static uint32_t mirror_id_span(const mirror_preset_t *p) {
    if (!p || p->base_id == 0) return 0;
    uint32_t span = 0;
    for (int i = 0; i < p->n; i++) {
        uint32_t id = p->can_id[i];
        if (id == 0 || id < p->base_id) continue;
        uint32_t off = id - p->base_id;
        if (off > span) span = off;
    }
    return span;
}

/* Returns 0 on success and fills out[]; -1 on the INVALID_ARG paths. */
static int mirror_rebase(const mirror_preset_t *p, uint32_t base_id,
                         uint32_t *out) {
    int32_t delta = 0;
    if (p->base_id != 0 && base_id != 0 && base_id != p->base_id) {
        uint32_t span = mirror_id_span(p);
        if (base_id > ECU_BASE_ID_MAX || base_id + span > ECU_BASE_ID_MAX)
            return -1;
        delta = (int32_t)base_id - (int32_t)p->base_id;
    }
    for (int i = 0; i < p->n; i++) {
        out[i] = p->can_id[i];
        if (delta && out[i])
            out[i] = (uint32_t)((int32_t)out[i] + delta);
    }
    return 0;
}

/* Link Generic Dash as shipped: one multiplexed id, one unsupported slot. */
static mirror_preset_t link_generic(void) {
    mirror_preset_t p = { .base_id = 0x3E8, .n = 5 };
    p.can_id[0] = 0x3E8;  /* RPM      */
    p.can_id[1] = 0x3E8;  /* MAP      */
    p.can_id[2] = 0x3E8;  /* COOLANT  */
    p.can_id[3] = 0;      /* FUEL_TRIM — not in Generic Dash */
    p.can_id[4] = 0x3E8;  /* GEAR     */
    return p;
}

/* A multi-id stream, to prove spacing survives rather than collapsing. */
static mirror_preset_t multi_id(void) {
    mirror_preset_t p = { .base_id = 0x5F0, .n = 4 };
    p.can_id[0] = 0x5F0;
    p.can_id[1] = 0x5F3;
    p.can_id[2] = 0;
    p.can_id[3] = 0x5FF;
    return p;
}

/* ── Tests ──────────────────────────────────────────────────────────── */

static void test_span_is_zero_for_single_id_stream(void) {
    mirror_preset_t p = link_generic();
    TEST_ASSERT_EQUAL_HEX(0, mirror_id_span(&p));
}

static void test_span_spans_widest_row(void) {
    mirror_preset_t p = multi_id();
    TEST_ASSERT_EQUAL_HEX(0xF, mirror_id_span(&p));   /* 0x5FF - 0x5F0 */
}

static void test_span_zero_when_base_not_configurable(void) {
    mirror_preset_t p = multi_id();
    p.base_id = 0;
    TEST_ASSERT_EQUAL_HEX(0, mirror_id_span(&p));
}

/* The customer case: second generic stream on 1300 decimal. */
static void test_link_rebases_to_1300(void) {
    mirror_preset_t p = link_generic();
    uint32_t out[ROWS_MAX];
    TEST_ASSERT_EQUAL_INT(0, mirror_rebase(&p, 1300, out));
    TEST_ASSERT_EQUAL_HEX(1300, out[0]);
    TEST_ASSERT_EQUAL_HEX(1300, out[1]);
    TEST_ASSERT_EQUAL_HEX(1300, out[2]);
    TEST_ASSERT_EQUAL_HEX(1300, out[4]);
}

/* An unsupported slot must stay unbound — shifting 0 would invent a
 * binding on an id the ECU never sends. */
static void test_unsupported_slot_stays_zero(void) {
    mirror_preset_t p = link_generic();
    uint32_t out[ROWS_MAX];
    TEST_ASSERT_EQUAL_INT(0, mirror_rebase(&p, 1300, out));
    TEST_ASSERT_EQUAL_HEX(0, out[3]);
}

static void test_multi_id_keeps_its_spacing(void) {
    mirror_preset_t p = multi_id();
    uint32_t out[ROWS_MAX];
    TEST_ASSERT_EQUAL_INT(0, mirror_rebase(&p, 0x400, out));
    TEST_ASSERT_EQUAL_HEX(0x400, out[0]);
    TEST_ASSERT_EQUAL_HEX(0x403, out[1]);
    TEST_ASSERT_EQUAL_HEX(0,     out[2]);
    TEST_ASSERT_EQUAL_HEX(0x40F, out[3]);
}

static void test_rebase_downwards(void) {
    mirror_preset_t p = link_generic();
    uint32_t out[ROWS_MAX];
    TEST_ASSERT_EQUAL_INT(0, mirror_rebase(&p, 0x100, out));
    TEST_ASSERT_EQUAL_HEX(0x100, out[0]);
}

static void test_same_base_is_a_no_op(void) {
    mirror_preset_t p = link_generic();
    uint32_t out[ROWS_MAX];
    TEST_ASSERT_EQUAL_INT(0, mirror_rebase(&p, 0x3E8, out));
    TEST_ASSERT_EQUAL_HEX(0x3E8, out[0]);
    TEST_ASSERT_EQUAL_HEX(0,     out[3]);
}

static void test_zero_base_means_use_the_default(void) {
    mirror_preset_t p = link_generic();
    uint32_t out[ROWS_MAX];
    TEST_ASSERT_EQUAL_INT(0, mirror_rebase(&p, 0, out));
    TEST_ASSERT_EQUAL_HEX(0x3E8, out[0]);
}

/* An OEM broadcast has fixed ids; a base must not shift it. */
static void test_fixed_id_preset_is_never_shifted(void) {
    mirror_preset_t p = multi_id();
    p.base_id = 0;
    uint32_t out[ROWS_MAX];
    TEST_ASSERT_EQUAL_INT(0, mirror_rebase(&p, 0x400, out));
    TEST_ASSERT_EQUAL_HEX(0x5F0, out[0]);
    TEST_ASSERT_EQUAL_HEX(0x5FF, out[3]);
}

static void test_base_past_11_bits_is_rejected(void) {
    mirror_preset_t p = link_generic();
    uint32_t out[ROWS_MAX];
    TEST_ASSERT_EQUAL_INT(-1, mirror_rebase(&p, 0x800, out));
}

static void test_highest_single_id_base_is_allowed(void) {
    mirror_preset_t p = link_generic();
    uint32_t out[ROWS_MAX];
    TEST_ASSERT_EQUAL_INT(0, mirror_rebase(&p, 0x7FF, out));
    TEST_ASSERT_EQUAL_HEX(0x7FF, out[0]);
}

/* The span, not just the base, has to fit: 0x7FF is fine for a one-id
 * stream and past the end for a stream that occupies sixteen. */
static void test_span_must_also_fit_under_the_ceiling(void) {
    mirror_preset_t p = multi_id();
    uint32_t out[ROWS_MAX];
    TEST_ASSERT_EQUAL_INT(-1, mirror_rebase(&p, 0x7FF, out));
    TEST_ASSERT_EQUAL_INT(-1, mirror_rebase(&p, 0x7F1, out));
    TEST_ASSERT_EQUAL_INT(0,  mirror_rebase(&p, 0x7F0, out));
    TEST_ASSERT_EQUAL_HEX(0x7FF, out[3]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_span_is_zero_for_single_id_stream);
    RUN_TEST(test_span_spans_widest_row);
    RUN_TEST(test_span_zero_when_base_not_configurable);
    RUN_TEST(test_link_rebases_to_1300);
    RUN_TEST(test_unsupported_slot_stays_zero);
    RUN_TEST(test_multi_id_keeps_its_spacing);
    RUN_TEST(test_rebase_downwards);
    RUN_TEST(test_same_base_is_a_no_op);
    RUN_TEST(test_zero_base_means_use_the_default);
    RUN_TEST(test_fixed_id_preset_is_never_shifted);
    RUN_TEST(test_base_past_11_bits_is_rejected);
    RUN_TEST(test_highest_single_id_base_is_allowed);
    RUN_TEST(test_span_must_also_fit_under_the_ceiling);
    return UNITY_END();
}
