/* test_layout_migration.c — host-side coverage of layout-JSON schema gating
 *                            and inline migration helpers.
 *
 * ── Why these tests live here ────────────────────────────────────────────
 *
 * The real implementations live in:
 *
 *   main/layout/layout_manager.c   (schema_version gate)
 *   main/widgets/widget_rpm_bar.c  (RPM-bar limiter_effect enum collapse)
 *
 * Both source files transitively pull in LVGL, FreeRTOS, ESP-IDF, the signal
 * registry, the widget system, and LittleFS — none of which we want or need
 * on the host. The actual checks under test are tiny, however (literally a
 * handful of lines that read a cJSON tree and either reject it or rewrite a
 * field), so we re-implement those checks here verbatim as static helpers
 * and exercise *them*.
 *
 * This is a pragmatic test — it locks in the **observable behaviour** of
 * those gates and migrations. If the firmware-side logic drifts the helpers
 * here will go out of sync, but a copy-pasted few-line predicate that the
 * firmware copy is known to mirror is far more valuable than no test at
 * all. If/when those checks ever get extracted into a pure helper file
 * (e.g. `layout_manager_schema.c`), this test should be updated to include
 * the real source directly — the assertions would carry over unchanged.
 *
 * Source-of-truth reference (must be kept in lockstep on changes):
 *
 *   layout_manager.c, _instantiate_widgets / layout_manager_load:
 *     cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
 *     int schema_ver = cJSON_IsNumber(sv) ? sv->valueint : 0;
 *     if (schema_ver < 1)                       → FAIL (reject)
 *     if (schema_ver > LAYOUT_SCHEMA_VERSION)   → WARN, continue
 *     else                                      → accept
 *
 *   widget_rpm_bar.c, rpm_bar_from_json (after reading limiter_effect):
 *     if (rd->limiter_effect == 2 || rd->limiter_effect == 3)
 *         rd->limiter_effect = 1;                // → Bar Flash
 *     else if (rd->limiter_effect == 5 || rd->limiter_effect == 6)
 *         rd->limiter_effect = 2;                // → Bar Solid
 *     else if (rd->limiter_effect > 2)
 *         rd->limiter_effect = 0;                // → None (1, 4, 7, …)
 *
 * ── cJSON dependency ─────────────────────────────────────────────────────
 *
 * The system has no `libcjson` package installed. cJSON v1.7.19 (MIT) is
 * vendored under tests/native/cjson/ and built alongside the test. The
 * Makefile + run.ps1 pick it up automatically.
 */
#include "unity.h"
#include "cjson/cJSON.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Mirror of LAYOUT_SCHEMA_VERSION from main/layout/layout_manager.h.
 *
 * Tests below pin behaviour against this value; if the firmware bumps the
 * schema, this constant should be bumped to match (the assertions still
 * hold for any value ≥ 1). */
#define TEST_LAYOUT_SCHEMA_VERSION 13

/* ── Helpers under test ──────────────────────────────────────────────────
 *
 * These mirror the firmware code byte-for-byte (modulo the ESP_LOG calls).
 * Keep them in lockstep with the source-of-truth references above. */

/* Returns 0 (accept), -1 (reject — invalid/missing version),
 * or 1 (accept-with-warning — newer than firmware).
 *
 * Mirrors the schema-version gate at the top of layout_manager_load(). */
static int schema_gate(const cJSON *root) {
    cJSON *sv = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    int schema_ver = cJSON_IsNumber(sv) ? sv->valueint : 0;
    if (schema_ver < 1) {
        return -1;  /* ESP_FAIL in firmware */
    }
    if (schema_ver > TEST_LAYOUT_SCHEMA_VERSION) {
        return 1;   /* ESP_LOGW + continue */
    }
    return 0;       /* accepted */
}

/* Mirrors the limiter_effect migration tail of rpm_bar_from_json().
 * Takes the raw value as it came off the JSON; returns the post-migration
 * value that gets stored in rpm_bar_data_t.limiter_effect. */
static uint8_t migrate_limiter_effect(uint8_t raw) {
    uint8_t v = raw;
    if (v == 2 || v == 3)        v = 1;
    else if (v == 5 || v == 6)   v = 2;
    else if (v > 2)              v = 0;
    return v;
}

/* ── Schema-gate tests ──────────────────────────────────────────────────── */

static void test_schema_missing_is_rejected(void) {
    /* No schema_version field at all → treated as 0 → rejected. */
    cJSON *root = cJSON_Parse("{\"widgets\": []}");
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(-1, schema_gate(root));
    cJSON_Delete(root);
}

static void test_schema_zero_is_rejected(void) {
    cJSON *root = cJSON_Parse("{\"schema_version\": 0, \"widgets\": []}");
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(-1, schema_gate(root));
    cJSON_Delete(root);
}

static void test_schema_negative_is_rejected(void) {
    cJSON *root = cJSON_Parse("{\"schema_version\": -1, \"widgets\": []}");
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(-1, schema_gate(root));
    cJSON_Delete(root);
}

static void test_schema_string_value_is_rejected(void) {
    /* "13" as a string fails cJSON_IsNumber → schema_ver=0 → reject.
     * Documents that legacy/sloppy producers can't sneak by on stringly-typed
     * versions. */
    cJSON *root = cJSON_Parse("{\"schema_version\": \"13\", \"widgets\": []}");
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(-1, schema_gate(root));
    cJSON_Delete(root);
}

static void test_schema_v1_loads(void) {
    /* The earliest accepted schema. */
    cJSON *root = cJSON_Parse("{\"schema_version\": 1, \"widgets\": []}");
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(0, schema_gate(root));
    cJSON_Delete(root);
}

static void test_schema_current_version_loads(void) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"schema_version\": %d, \"widgets\": []}",
             TEST_LAYOUT_SCHEMA_VERSION);
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(0, schema_gate(root));
    cJSON_Delete(root);
}

static void test_schema_one_below_current_loads(void) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"schema_version\": %d, \"widgets\": []}",
             TEST_LAYOUT_SCHEMA_VERSION - 1);
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(0, schema_gate(root));
    cJSON_Delete(root);
}

static void test_schema_newer_than_firmware_loads_with_warning(void) {
    char buf[64];
    snprintf(buf, sizeof(buf),
             "{\"schema_version\": %d, \"widgets\": []}",
             TEST_LAYOUT_SCHEMA_VERSION + 1);
    cJSON *root = cJSON_Parse(buf);
    TEST_ASSERT_NOT_NULL(root);
    /* gate returns 1 — firmware logs a warning and continues. */
    TEST_ASSERT_EQUAL_INT(1, schema_gate(root));
    cJSON_Delete(root);
}

static void test_schema_far_future_still_loads_with_warning(void) {
    cJSON *root = cJSON_Parse("{\"schema_version\": 9999, \"widgets\": []}");
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(1, schema_gate(root));
    cJSON_Delete(root);
}

/* ── RPM-bar limiter_effect migration tests ─────────────────────────────
 *
 * Tests below pin the *actual* observed behaviour of the migration code
 * in widget_rpm_bar.c, not the docstring above it. Inspecting the C ladder:
 *
 *     if (v == 2 || v == 3) v = 1;
 *     else if (v == 5 || v == 6) v = 2;
 *     else if (v > 2) v = 0;
 *
 * gives the real mapping:
 *
 *     0 → 0   (None — passthrough)
 *     1 → 1   (NB: stays 1, becomes new "Bar Flash" — see WART below)
 *     2 → 1   (old "Bar Flash" → new "Bar Flash")
 *     3 → 1   (old "Bar+Circles Flash" → new "Bar Flash")
 *     4 → 0   (old "Circles Flash" → None)
 *     5 → 2   (old "Bar Solid" → new "Bar Solid")
 *     6 → 2   (old "Bar+Circles Solid" → new "Bar Solid")
 *     7 → 0   (old "Circles Solid" → None)
 *
 * ── WART #1: input 1 ("old Circles only") leaks through unchanged ──────
 *
 * The source comment claims "(1, 4, 7) are now None" but the `v > 2` guard
 * never catches 1, so a layout saved before the rework with limiter_effect=1
 * will be loaded as new-enum "Bar Flash" (= 1) rather than None (= 0).
 * Pinning the real behaviour here both documents the gap and prevents a
 * silent change to the migration logic.
 *
 * ── WART #2: migration is NOT idempotent ───────────────────────────────
 *
 * Re-running the migration on an already-migrated value of 2 will rewrite
 * it as 1 (because the first arm catches v == 2). Loading a v13-saved
 * layout, then re-saving without the trailing migration step, is therefore
 * the only way to keep "Bar Solid" stable. The test below pins this so
 * future refactors don't accidentally change the property without intent.
 */

static void test_limiter_passthrough_none(void) {
    TEST_ASSERT_EQUAL_INT(0, migrate_limiter_effect(0));
}

static void test_limiter_old_circles_only_falls_through_to_one(void) {
    /* WART #1: real code does NOT collapse 1 → 0 despite the comment. */
    TEST_ASSERT_EQUAL_INT(1, migrate_limiter_effect(1));
}

static void test_limiter_old_bar_flash_maps_to_bar_flash(void) {
    TEST_ASSERT_EQUAL_INT(1, migrate_limiter_effect(2));
}

static void test_limiter_old_bar_circles_flash_maps_to_bar_flash(void) {
    TEST_ASSERT_EQUAL_INT(1, migrate_limiter_effect(3));
}

static void test_limiter_old_circles_flash_collapses_to_none(void) {
    TEST_ASSERT_EQUAL_INT(0, migrate_limiter_effect(4));
}

static void test_limiter_old_bar_solid_maps_to_bar_solid(void) {
    TEST_ASSERT_EQUAL_INT(2, migrate_limiter_effect(5));
}

static void test_limiter_old_bar_circles_solid_maps_to_bar_solid(void) {
    TEST_ASSERT_EQUAL_INT(2, migrate_limiter_effect(6));
}

static void test_limiter_old_circles_solid_collapses_to_none(void) {
    TEST_ASSERT_EQUAL_INT(0, migrate_limiter_effect(7));
}

/* New-enum value 1 (Bar Flash) is unstable under re-migration — feeding it
 * back through the same function rewrites it via the `v == 2` arm? No: input
 * 1 stays 1 (no arm catches it). Document this. */
static void test_limiter_new_bar_flash_is_stable(void) {
    /* 1 → 1 → 1 (idempotent for this value) */
    TEST_ASSERT_EQUAL_INT(1, migrate_limiter_effect(1));
    TEST_ASSERT_EQUAL_INT(1, migrate_limiter_effect(migrate_limiter_effect(1)));
}

static void test_limiter_new_bar_solid_NOT_idempotent_via_5(void) {
    /* WART #2: 5 → 2 (first pass), 2 → 1 (second pass, because the arm
     * catches v == 2). Re-running the migration corrupts the value. */
    uint8_t once  = migrate_limiter_effect(5);
    uint8_t twice = migrate_limiter_effect(once);
    TEST_ASSERT_EQUAL_INT(2, once);
    TEST_ASSERT_EQUAL_INT(1, twice);  /* ← drifts, see WART #2 */
}

/* ── End-to-end: parse JSON → migrate → verify ─────────────────────────── */

static void test_end_to_end_layout_with_legacy_limiter(void) {
    /* Legacy v8-era layout: limiter_effect=3 ("Bar+Circles Flash") and a
     * still-supported schema. After parse + migrate the effect should be 1. */
    const char *json =
        "{"
        "  \"schema_version\": 11,"
        "  \"widgets\": ["
        "    {"
        "      \"type\": \"rpm_bar\","
        "      \"config\": {"
        "        \"redline\": 6500,"
        "        \"limiter_effect\": 3,"
        "        \"limiter_value\": 7500"
        "      }"
        "    }"
        "  ]"
        "}";
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);

    /* Schema gate accepts. */
    TEST_ASSERT_EQUAL_INT(0, schema_gate(root));

    /* Walk to the rpm_bar's limiter_effect field. */
    cJSON *widgets = cJSON_GetObjectItemCaseSensitive(root, "widgets");
    TEST_ASSERT_TRUE(cJSON_IsArray(widgets));
    cJSON *first = cJSON_GetArrayItem(widgets, 0);
    TEST_ASSERT_NOT_NULL(first);
    cJSON *cfg = cJSON_GetObjectItemCaseSensitive(first, "config");
    TEST_ASSERT_NOT_NULL(cfg);
    cJSON *eff = cJSON_GetObjectItemCaseSensitive(cfg, "limiter_effect");
    TEST_ASSERT_TRUE(cJSON_IsNumber(eff));

    uint8_t migrated = migrate_limiter_effect((uint8_t)eff->valueint);
    TEST_ASSERT_EQUAL_INT(1, migrated);

    cJSON_Delete(root);
}

static void test_end_to_end_missing_schema_then_no_migration(void) {
    /* If the schema gate would reject, we never get to the migration step.
     * This test just locks in that ordering for documentation. */
    const char *json =
        "{"
        "  \"widgets\": ["
        "    { \"type\": \"rpm_bar\", \"config\": { \"limiter_effect\": 3 } }"
        "  ]"
        "}";
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(-1, schema_gate(root));
    cJSON_Delete(root);
}

/* ── Runner ─────────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* schema_version gate */
    RUN_TEST(test_schema_missing_is_rejected);
    RUN_TEST(test_schema_zero_is_rejected);
    RUN_TEST(test_schema_negative_is_rejected);
    RUN_TEST(test_schema_string_value_is_rejected);
    RUN_TEST(test_schema_v1_loads);
    RUN_TEST(test_schema_current_version_loads);
    RUN_TEST(test_schema_one_below_current_loads);
    RUN_TEST(test_schema_newer_than_firmware_loads_with_warning);
    RUN_TEST(test_schema_far_future_still_loads_with_warning);

    /* RPM-bar limiter_effect inline migration */
    RUN_TEST(test_limiter_passthrough_none);
    RUN_TEST(test_limiter_old_circles_only_falls_through_to_one);
    RUN_TEST(test_limiter_old_bar_flash_maps_to_bar_flash);
    RUN_TEST(test_limiter_old_bar_circles_flash_maps_to_bar_flash);
    RUN_TEST(test_limiter_old_circles_flash_collapses_to_none);
    RUN_TEST(test_limiter_old_bar_solid_maps_to_bar_solid);
    RUN_TEST(test_limiter_old_bar_circles_solid_maps_to_bar_solid);
    RUN_TEST(test_limiter_old_circles_solid_collapses_to_none);
    RUN_TEST(test_limiter_new_bar_flash_is_stable);
    RUN_TEST(test_limiter_new_bar_solid_NOT_idempotent_via_5);

    /* End-to-end JSON walk */
    RUN_TEST(test_end_to_end_layout_with_legacy_limiter);
    RUN_TEST(test_end_to_end_missing_schema_then_no_migration);

    return UNITY_END();
}
