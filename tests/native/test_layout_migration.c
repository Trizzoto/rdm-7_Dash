/* test_layout_migration.c — host-side coverage of layout-JSON schema gating.
 *
 * ── Why these tests live here ────────────────────────────────────────────
 *
 * The real implementation lives in:
 *
 *   main/layout/layout_manager.c   (schema_version gate)
 *
 * That source file transitively pulls in LVGL, FreeRTOS, ESP-IDF, the signal
 * registry, the widget system, and LittleFS — none of which we want or need
 * on the host. The actual check under test is a handful of lines that read
 * a cJSON tree and either rejects it or accepts-with-warning, so we
 * re-implement that check here verbatim as a static helper and exercise it.
 *
 * This is a pragmatic test — it locks in the **observable behaviour** of
 * the gate. If the firmware-side logic drifts, the helper here will go out
 * of sync, but a copy-pasted few-line predicate that the firmware copy is
 * known to mirror is far more valuable than no test at all. If/when the
 * gate ever gets extracted into a pure helper file (e.g. layout_manager_
 * schema.c), this test should be updated to include the real source
 * directly — the assertions would carry over unchanged.
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
 * ── Historical note ──────────────────────────────────────────────────────
 *
 * An earlier revision of this file also exercised an inline migration of
 * the RPM-bar limiter_effect enum (collapse of the old 7-value circles+bar
 * enum into the current 3-value 0=None / 1=Bar Flash / 2=Bar Solid). That
 * migration was deleted from widget_rpm_bar.c on 2026-04-27 (the circles
 * UI was removed as a feature, all field-deployed devices have long since
 * re-saved their layouts in the new shape, and the migration ladder had a
 * non-idempotency defect). The corresponding tests went with it. The
 * remaining check in widget_rpm_bar.c is just a clamp — out-of-range values
 * silently degrade to None.
 *
 * ── cJSON dependency ─────────────────────────────────────────────────────
 *
 * The system has no `libcjson` package installed. cJSON v1.7.18 (MIT) is
 * vendored under tests/native/cjson/ and built alongside the test. The
 * Makefile + run.ps1 pick it up automatically.
 */
#include "unity.h"
#include "cjson/cJSON.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Mirror of LAYOUT_SCHEMA_VERSION from main/layout/layout_manager.h.
 *
 * Tests below pin behaviour against this value; if the firmware bumps the
 * schema, this constant should be bumped to match (the assertions still
 * hold for any value ≥ 1). */
#define TEST_LAYOUT_SCHEMA_VERSION 13

/* ── Helper under test ───────────────────────────────────────────────────
 *
 * Mirrors the firmware code byte-for-byte (modulo the ESP_LOG calls).
 * Keep in lockstep with the source-of-truth reference above. */

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

    return UNITY_END();
}
