/* test_ecu_preset_match.c — host coverage of ECU preset match-score math.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * ecu_preset_match_score() is the heart of the "preset detected on this
 * bus" UX: both the on-device ECU picker and the web modal use it to
 * decide whether to draw a preset's blue indicator and whether to
 * surface or hide that preset in Manual mode. Wrong math = wrong
 * highlights = user picks the wrong vehicle preset.
 *
 * The function lives entangled with can_bus_test (firmware-side) and
 * the full ECU_PRESETS table (~25-row ecu_signal_row_t struct per
 * preset, pulls in ESP-IDF for can_id types). Mirroring the math
 * host-side keeps the test in lockstep without dragging that whole
 * surface in.
 *
 * Source-of-truth reference (must stay in lockstep):
 *
 *   main/layout/ecu_presets.c, ecu_preset_match_score():
 *     1. Collect unique non-zero can_id values from preset->rows[].
 *     2. Read the latest can_bus_test report's unique_ids at the
 *        recommended bitrate (skipped if no scan run).
 *     3. score = round( 100 * |intersection| / |preset_unique| ).
 *     4. Clamp to 0..100.
 *
 * Threshold (ECU_PRESET_MATCH_THRESHOLD) is 30. Tests assert the
 * boundary at 30% lands as expected.
 */
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ── Host mirror (verbatim modulo the firmware-side data plumbing) ──────
 *
 * The firmware reads the preset's rows[] and the bus_test report's
 * unique_ids[] from globals. The test feeds both as arrays so each
 * test case is hermetic.
 */

#define MAX_PRESET_IDS  32
#define MAX_DETECTED    32

static int score_for(const uint32_t *preset_ids_raw, int preset_id_count_raw,
                     const uint32_t *detected, int detected_count) {
    if (!preset_ids_raw || preset_id_count_raw <= 0) return 0;

    /* Dedup preset IDs first. The firmware walks rows[] which can
     * contain the same can_id under multiple signals (e.g. Ford BA/BF
     * has 4+ signals on 0x427). Each unique ID counts once. */
    uint32_t preset[MAX_PRESET_IDS];
    int preset_count = 0;
    for (int i = 0; i < preset_id_count_raw && preset_count < MAX_PRESET_IDS; i++) {
        uint32_t id = preset_ids_raw[i];
        if (id == 0) continue;          /* SIG_UNSUPPORTED */
        bool dup = false;
        for (int j = 0; j < preset_count; j++) {
            if (preset[j] == id) { dup = true; break; }
        }
        if (!dup) preset[preset_count++] = id;
    }
    if (preset_count == 0) return 0;
    if (!detected || detected_count <= 0) return 0;

    int hits = 0;
    for (int i = 0; i < preset_count; i++) {
        for (int j = 0; j < detected_count; j++) {
            if (preset[i] == detected[j]) { hits++; break; }
        }
    }

    int score = (hits * 100 + preset_count / 2) / preset_count;
    if (score > 100) score = 100;
    return score;
}

/* The threshold mirror — change in lockstep with ECU_PRESET_MATCH_THRESHOLD
 * in main/layout/ecu_presets.h. */
#define TEST_MATCH_THRESHOLD 30

/* ── Empty / degenerate inputs ──────────────────────────────────────────── */

static void test_null_preset_returns_zero(void) {
    uint32_t detected[] = { 0x100, 0x200 };
    TEST_ASSERT_EQUAL_INT(0, score_for(NULL, 0, detected, 2));
}

static void test_empty_preset_returns_zero(void) {
    uint32_t detected[] = { 0x100, 0x200 };
    TEST_ASSERT_EQUAL_INT(0, score_for(detected, 0, detected, 2));
}

static void test_all_unsupported_slots_returns_zero(void) {
    /* SIG_UNSUPPORTED entries are encoded as can_id == 0. A preset whose
     * rows are all sentinel (e.g. the OBD2 placeholder) has no broadcast
     * IDs to match against and must score 0. */
    uint32_t preset[] = { 0, 0, 0, 0, 0 };
    uint32_t detected[] = { 0x100, 0x200, 0x300 };
    TEST_ASSERT_EQUAL_INT(0, score_for(preset, 5, detected, 3));
}

static void test_no_scan_data_returns_zero(void) {
    /* When can_bus_test_get_report() returns NULL or recommended_bitrate
     * is < 0, the firmware short-circuits to 0. Same here. */
    uint32_t preset[] = { 0x100, 0x200, 0x300 };
    TEST_ASSERT_EQUAL_INT(0, score_for(preset, 3, NULL, 0));

    uint32_t detected[] = { 0x100 };
    TEST_ASSERT_EQUAL_INT(0, score_for(preset, 3, detected, 0));
}

/* ── Intersection math ──────────────────────────────────────────────────── */

static void test_full_overlap_is_100(void) {
    uint32_t preset[]   = { 0x100, 0x200, 0x300 };
    uint32_t detected[] = { 0x100, 0x200, 0x300, 0x999 };
    TEST_ASSERT_EQUAL_INT(100, score_for(preset, 3, detected, 4));
}

static void test_no_overlap_is_0(void) {
    uint32_t preset[]   = { 0x100, 0x200, 0x300 };
    uint32_t detected[] = { 0x400, 0x500, 0x600 };
    TEST_ASSERT_EQUAL_INT(0, score_for(preset, 3, detected, 3));
}

static void test_one_of_five_is_20_percent(void) {
    /* 1/5 = 20%. Below the 30% threshold — would NOT be flagged
     * as detected on the UI. */
    uint32_t preset[]   = { 0x100, 0x200, 0x300, 0x400, 0x500 };
    uint32_t detected[] = { 0x100 };
    int score = score_for(preset, 5, detected, 1);
    TEST_ASSERT_EQUAL_INT(20, score);
    TEST_ASSERT_TRUE(score < TEST_MATCH_THRESHOLD);
}

static void test_two_of_five_is_40_percent(void) {
    /* 2/5 = 40%. Above threshold — UI flags as detected. */
    uint32_t preset[]   = { 0x100, 0x200, 0x300, 0x400, 0x500 };
    uint32_t detected[] = { 0x100, 0x300 };
    int score = score_for(preset, 5, detected, 2);
    TEST_ASSERT_EQUAL_INT(40, score);
    TEST_ASSERT_TRUE(score >= TEST_MATCH_THRESHOLD);
}

static void test_one_of_three_is_33_with_rounding(void) {
    /* 1/3 = 33.33%. Round-half-up gives 33 (above threshold). */
    uint32_t preset[]   = { 0x100, 0x200, 0x300 };
    uint32_t detected[] = { 0x100 };
    int score = score_for(preset, 3, detected, 1);
    TEST_ASSERT_EQUAL_INT(33, score);
    TEST_ASSERT_TRUE(score >= TEST_MATCH_THRESHOLD);
}

static void test_threshold_boundary_exactly_30_percent(void) {
    /* 3/10 = 30% — at the threshold. The firmware uses `>=` so this
     * counts as detected. Lock that in here. */
    uint32_t preset[]   = { 0x100, 0x200, 0x300, 0x400, 0x500,
                            0x600, 0x700, 0x800, 0x900, 0xA00 };
    uint32_t detected[] = { 0x100, 0x500, 0x900 };
    int score = score_for(preset, 10, detected, 3);
    TEST_ASSERT_EQUAL_INT(30, score);
    TEST_ASSERT_TRUE(score >= TEST_MATCH_THRESHOLD);
}

static void test_just_under_threshold_29_percent(void) {
    /* 2/7 = 28.57% — rounds to 29, below threshold. */
    uint32_t preset[]   = { 0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700 };
    uint32_t detected[] = { 0x100, 0x500 };
    int score = score_for(preset, 7, detected, 2);
    TEST_ASSERT_EQUAL_INT(29, score);
    TEST_ASSERT_TRUE(score < TEST_MATCH_THRESHOLD);
}

/* ── De-duplication behaviours ──────────────────────────────────────────── */

static void test_preset_duplicates_count_once(void) {
    /* Ford BA/BF has many signals on 0x427 — the row[] table has the
     * same can_id repeated. The de-dup pass means a preset of
     * [0x427, 0x427, 0x427, 0x500, 0x600] has 3 unique IDs, not 5.
     * Two hits on (0x500, 0x600) of 3 unique = 67%, not 40%. */
    uint32_t preset[]   = { 0x427, 0x427, 0x427, 0x500, 0x600 };
    uint32_t detected[] = { 0x500, 0x600 };
    int score = score_for(preset, 5, detected, 2);
    TEST_ASSERT_EQUAL_INT(67, score);
}

static void test_detected_duplicates_no_double_count(void) {
    /* The intersection inner-loop breaks after the first hit, so
     * detected[] containing the same ID twice can't inflate the
     * count. Defensive — bus_test already de-duplicates, but if a
     * future change ever feeds raw frame IDs the test will catch
     * any double-counting regression. */
    uint32_t preset[]   = { 0x100, 0x200 };
    uint32_t detected[] = { 0x100, 0x100, 0x100, 0x100 };
    /* Only one of the preset's 2 IDs is in the detected set,
     * regardless of how many times 0x100 appears. 1/2 = 50%. */
    int score = score_for(preset, 2, detected, 4);
    TEST_ASSERT_EQUAL_INT(50, score);
}

/* ── Clamp ──────────────────────────────────────────────────────────────── */

static void test_score_clamped_at_100(void) {
    /* Can't construct an actual >100 input given the math, but the
     * clamp is there as a defensive guard. Asserts that the boundary
     * case at exactly 100 doesn't get clamped down. */
    uint32_t preset[]   = { 0x100 };
    uint32_t detected[] = { 0x100, 0x200, 0x300 };
    TEST_ASSERT_EQUAL_INT(100, score_for(preset, 1, detected, 3));
}

/* ── Real-world preset shapes (sanity, not exhaustive) ──────────────────── */

static void test_realistic_ford_bf_preset_matches_when_bus_quiet(void) {
    /* Approximation of the Ford BA/BF preset's unique IDs: 0x353, 0x44D,
     * 0x427, 0x437, 0x553, 0x207 (6 distinct after de-dup of the 16-row
     * signals table). A bus with only the engine PCM broadcasting
     * (0x207 and 0x427) lights up 2/6 = 33% — above threshold. */
    uint32_t preset[]   = { 0x353, 0x44D, 0x427, 0x437, 0x553, 0x207 };
    uint32_t detected[] = { 0x207, 0x427, 0xFFE };  /* + one unrelated ID */
    int score = score_for(preset, 6, detected, 3);
    TEST_ASSERT_EQUAL_INT(33, score);
    TEST_ASSERT_TRUE(score >= TEST_MATCH_THRESHOLD);
}

static void test_realistic_wrong_preset_misses(void) {
    /* Same bus, but scoring against an entirely different ECU
     * (e.g. Haltech Nexus uses 0x360..0x3E1). No overlap. */
    uint32_t preset[]   = { 0x360, 0x36B, 0x372, 0x373, 0x376, 0x3E0, 0x3E1 };
    uint32_t detected[] = { 0x207, 0x427, 0xFFE };
    int score = score_for(preset, 7, detected, 3);
    TEST_ASSERT_EQUAL_INT(0, score);
    TEST_ASSERT_TRUE(score < TEST_MATCH_THRESHOLD);
}

/* ── Runner ─────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* degenerate inputs */
    RUN_TEST(test_null_preset_returns_zero);
    RUN_TEST(test_empty_preset_returns_zero);
    RUN_TEST(test_all_unsupported_slots_returns_zero);
    RUN_TEST(test_no_scan_data_returns_zero);

    /* intersection math + threshold boundaries */
    RUN_TEST(test_full_overlap_is_100);
    RUN_TEST(test_no_overlap_is_0);
    RUN_TEST(test_one_of_five_is_20_percent);
    RUN_TEST(test_two_of_five_is_40_percent);
    RUN_TEST(test_one_of_three_is_33_with_rounding);
    RUN_TEST(test_threshold_boundary_exactly_30_percent);
    RUN_TEST(test_just_under_threshold_29_percent);

    /* de-duplication */
    RUN_TEST(test_preset_duplicates_count_once);
    RUN_TEST(test_detected_duplicates_no_double_count);

    /* clamp + real-world shapes */
    RUN_TEST(test_score_clamped_at_100);
    RUN_TEST(test_realistic_ford_bf_preset_matches_when_bus_quiet);
    RUN_TEST(test_realistic_wrong_preset_misses);

    return UNITY_END();
}
