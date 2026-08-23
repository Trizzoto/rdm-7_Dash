/* test_ecu_preset_rebase.c — host coverage of ECU preset CAN-ID rebasing.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * Some ECUs lay their dash stream out as a contiguous run of frames and
 * let the tuning software retransmit that same stream from a different
 * base id. The Link G4+/G4X Generic Dash is the motivating case: it ships
 * at 0x3E8..0x3F0, but a car running a second dash that already claims
 * 0x3E8 needs the RDM-7 pointed at a relocated copy.
 *
 * ecu_preset_rebase() shifts every non-zero can_id by the same delta. Get
 * the delta or the bounds wrong and the dash silently decodes nothing (or,
 * worse, decodes a neighbouring frame as RPM), so the arithmetic and every
 * rejection path are worth pinning down.
 *
 * Source-of-truth reference (must stay in lockstep):
 *
 *   main/layout/ecu_presets.c, ecu_preset_rebase():
 *     1. Reject preset->base_id == 0 (ids are fixed, not relocatable).
 *     2. Reject new_base outside 1 .. ECU_PRESET_MAX_STD_CAN_ID.
 *     3. delta = new_base - base_id, signed (streams can move down).
 *     4. Validate ALL rows before mutating: every shifted non-zero id must
 *        land in 1 .. ECU_PRESET_MAX_STD_CAN_ID.
 *     5. Copy, then shift every non-zero can_id. can_id == 0 is
 *        SIG_UNSUPPORTED and stays 0.
 *
 * Same host-mirror convention as test_ecu_preset_match.c: the real
 * function is entangled with cJSON and the layout manager, so the math is
 * reproduced here rather than dragging that surface into a host build.
 */
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ECU_PRESET_MAX_STD_CAN_ID 0x7FFU
#define ROWS_MAX 24

/* Mirrors the fields of ecu_preset_t / ecu_signal_row_t that rebasing
 * touches. The real structs carry scale/offset/units too — untouched by a
 * rebase, so they'd only be noise here. */
typedef struct {
    uint32_t base_id;              /* 0 = fixed ids, not rebasable */
    uint32_t can_id[ROWS_MAX];     /* 0 = SIG_UNSUPPORTED */
    int      row_count;
} preset_t;

/* Returns true on success (ESP_OK), false on ESP_ERR_INVALID_ARG. */
static bool rebase(const preset_t *src, uint32_t new_base, preset_t *out) {
    if (!src || !out) return false;
    if (src->base_id == 0) return false;
    if (new_base == 0 || new_base > ECU_PRESET_MAX_STD_CAN_ID) return false;

    int32_t delta = (int32_t)new_base - (int32_t)src->base_id;

    for (int i = 0; i < src->row_count; i++) {
        uint32_t id = src->can_id[i];
        if (id == 0) continue;
        int64_t shifted = (int64_t)id + delta;
        if (shifted < 1 || shifted > ECU_PRESET_MAX_STD_CAN_ID) return false;
    }

    *out = *src;
    out->base_id = new_base;
    for (int i = 0; i < out->row_count; i++) {
        if (out->can_id[i] == 0) continue;
        out->can_id[i] = (uint32_t)((int32_t)out->can_id[i] + delta);
    }
    return true;
}

/* The Link Generic Dash id set, in ECU_SIG_* slot order, exactly as
 * main/layout/ecu_presets.c declares it. Two slots are SIG_UNSUPPORTED. */
static preset_t link_generic_dash(void) {
    preset_t p = {
        .base_id = 0x3E8,
        .row_count = 15,
        .can_id = {
            0x3E8,  /* RPM             */
            0x3E8,  /* MAP             */
            0x3E9,  /* THROTTLE        */
            0x3EA,  /* COOLANT_TEMP    */
            0x3EB,  /* INTAKE_AIR_TEMP */
            0x3EE,  /* LAMBDA          */
            0x3F0,  /* OIL_TEMP        */
            0x3F0,  /* OIL_PRESSURE    */
            0x3EF,  /* FUEL_PRESSURE   */
            0x3EC,  /* IGNITION        */
            0x3F0,  /* VEHICLE_SPEED   */
            0x3EC,  /* GEAR            */
            0x3EB,  /* BATTERY_VOLTAGE */
            0,      /* FUEL_TRIM  — unsupported */
            0,      /* EGT        — unsupported */
        },
    };
    return p;
}

/* ── The motivating case ─────────────────────────────────────────────── */

/* Cameron's car: another dash owns 0x3E8, so the Link retransmits the same
 * Generic Dash stream from 1300 (0x514) and the RDM-7 follows it there. */
void test_link_rebased_to_1300(void) {
    preset_t src = link_generic_dash(), out;
    TEST_ASSERT_TRUE(rebase(&src, 0x514, &out));

    TEST_ASSERT_EQUAL_HEX(0x514, out.base_id);
    TEST_ASSERT_EQUAL_HEX(0x514, out.can_id[0]);   /* RPM      3E8 -> 514 */
    TEST_ASSERT_EQUAL_HEX(0x514, out.can_id[1]);   /* MAP      shares 514 */
    TEST_ASSERT_EQUAL_HEX(0x515, out.can_id[2]);   /* THROTTLE 3E9 -> 515 */
    TEST_ASSERT_EQUAL_HEX(0x516, out.can_id[3]);   /* COOLANT  3EA -> 516 */
    TEST_ASSERT_EQUAL_HEX(0x51C, out.can_id[6]);   /* OIL_TEMP 3F0 -> 51C */
}

/* Frames that shared an id before must still share it after — the offsets
 * within a frame are unchanged, so a broken delta would split them. */
void test_shared_frames_stay_shared(void) {
    preset_t src = link_generic_dash(), out;
    TEST_ASSERT_TRUE(rebase(&src, 0x514, &out));

    TEST_ASSERT_EQUAL_HEX(out.can_id[0],  out.can_id[1]);   /* RPM  + MAP  */
    TEST_ASSERT_EQUAL_HEX(out.can_id[6],  out.can_id[7]);   /* OILT + OILP */
    TEST_ASSERT_EQUAL_HEX(out.can_id[6],  out.can_id[10]);  /* + speed     */
    TEST_ASSERT_EQUAL_HEX(out.can_id[9],  out.can_id[11]);  /* IGN  + gear */
    TEST_ASSERT_EQUAL_HEX(out.can_id[4],  out.can_id[12]);  /* IAT  + batt */
}

/* Every gap in the stock layout must survive: 0x3EC -> 0x3EE skips 0x3ED,
 * so the rebased stream must skip the matching slot too. */
void test_gaps_preserved(void) {
    preset_t src = link_generic_dash(), out;
    TEST_ASSERT_TRUE(rebase(&src, 0x514, &out));

    for (int i = 0; i < src.row_count; i++) {
        if (src.can_id[i] == 0) continue;
        TEST_ASSERT_EQUAL_HEX(src.can_id[i] - 0x3E8, out.can_id[i] - 0x514);
    }
}

void test_unsupported_slots_stay_zero(void) {
    preset_t src = link_generic_dash(), out;
    TEST_ASSERT_TRUE(rebase(&src, 0x514, &out));

    TEST_ASSERT_EQUAL_HEX(0, out.can_id[13]);  /* FUEL_TRIM */
    TEST_ASSERT_EQUAL_HEX(0, out.can_id[14]);  /* EGT       */
}

/* ── Direction and identity ──────────────────────────────────────────── */

void test_rebase_downward(void) {
    preset_t src = link_generic_dash(), out;
    TEST_ASSERT_TRUE(rebase(&src, 0x100, &out));

    TEST_ASSERT_EQUAL_HEX(0x100, out.can_id[0]);   /* RPM      */
    TEST_ASSERT_EQUAL_HEX(0x108, out.can_id[6]);   /* OIL_TEMP */
}

void test_rebase_to_stock_base_is_identity(void) {
    preset_t src = link_generic_dash(), out;
    TEST_ASSERT_TRUE(rebase(&src, 0x3E8, &out));

    for (int i = 0; i < src.row_count; i++)
        TEST_ASSERT_EQUAL_HEX(src.can_id[i], out.can_id[i]);
}

/* ── Rejection paths ─────────────────────────────────────────────────── */

void test_fixed_id_preset_rejected(void) {
    preset_t src = link_generic_dash(), out;
    src.base_id = 0;                    /* e.g. an OEM preset */
    TEST_ASSERT_FALSE(rebase(&src, 0x514, &out));
}

void test_base_zero_rejected(void) {
    preset_t src = link_generic_dash(), out;
    TEST_ASSERT_FALSE(rebase(&src, 0, &out));
}

void test_base_above_11_bit_rejected(void) {
    preset_t src = link_generic_dash(), out;
    TEST_ASSERT_FALSE(rebase(&src, 0x800, &out));
}

/* The base itself fits in 11 bits but the top of the stream (base+8)
 * would not — the whole rebase must be refused, not silently clipped. */
void test_top_of_stream_overflow_rejected(void) {
    preset_t src = link_generic_dash(), out;
    TEST_ASSERT_FALSE(rebase(&src, 0x7FF, &out));

    /* 0x7F7 is the highest base that still fits: 0x7F7 + 8 == 0x7FF. */
    TEST_ASSERT_TRUE(rebase(&src, 0x7F7, &out));
    TEST_ASSERT_EQUAL_HEX(0x7FF, out.can_id[6]);   /* OIL_TEMP, the top */
}

/* Validation runs over every row BEFORE anything is written, so a refused
 * rebase must leave the caller's preset byte-identical. */
void test_rejected_rebase_does_not_mutate_source(void) {
    preset_t src = link_generic_dash();
    preset_t before = src;
    preset_t out;

    TEST_ASSERT_FALSE(rebase(&src, 0x7FF, &out));
    TEST_ASSERT_EQUAL_HEX(before.base_id, src.base_id);
    for (int i = 0; i < before.row_count; i++)
        TEST_ASSERT_EQUAL_HEX(before.can_id[i], src.can_id[i]);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_link_rebased_to_1300);
    RUN_TEST(test_shared_frames_stay_shared);
    RUN_TEST(test_gaps_preserved);
    RUN_TEST(test_unsupported_slots_stay_zero);
    RUN_TEST(test_rebase_downward);
    RUN_TEST(test_rebase_to_stock_base_is_identity);
    RUN_TEST(test_fixed_id_preset_rejected);
    RUN_TEST(test_base_zero_rejected);
    RUN_TEST(test_base_above_11_bit_rejected);
    RUN_TEST(test_top_of_stream_overflow_rejected);
    RUN_TEST(test_rejected_rebase_does_not_mutate_source);
    return UNITY_END();
}
