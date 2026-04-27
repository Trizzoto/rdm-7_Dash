/* test_widget_rules.c — JSON round-trip coverage for widget_rules.c.
 *
 * widget_rules_from_json() and widget_rules_to_json() are pure data
 * functions: they read and write a "rules" array on a cJSON config
 * and walk a heap-allocated widget_rule_t buffer hung off the
 * widget_t. No LVGL, FreeRTOS, or hardware involvement.
 *
 * Mocking strategy
 * ────────────────
 *   • mocks/lvgl.h           — opaque lv_obj_t / lv_font_t / lv_color_t
 *                              shells. widget_types.h pulls <lvgl.h> in
 *                              for these but never dereferences them.
 *   • mocks/esp_log.h        — silences ESP_LOGx.
 *   • mocks/esp_heap_caps.h  — heap_caps_* → libc (widget_rules.c uses
 *                              plain calloc/free, but other firmware
 *                              code we may include later needs this).
 *   • Inline stubs below     — signal_find_by_name / signal_subscribe /
 *                              signal_unsubscribe / signal_get_by_index.
 *                              widget_rules_subscribe() and
 *                              widget_rules_free() exercise these; the
 *                              from_json/to_json paths do not.
 *
 * cJSON is vendored under tests/native/cjson/. */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Header-only mocks must be on the include path before widget_rules.c
 * pulls in its dependencies. The Makefile / run.ps1 add -Imocks. */

/* Vendor cJSON pulled in as TU so we don't depend on a system libcjson. */
#include "cjson/cJSON.h"

/* Real firmware sources / headers under test. */
#include "../../main/widgets/widget_types.h"
#include "../../main/widgets/signal.h"

/* ── Signal-layer stubs ───────────────────────────────────────────────────
 * widget_rules.c calls signal_find_by_name / signal_subscribe /
 * signal_unsubscribe / signal_get_by_index from its subscribe and
 * free paths. The from_json/to_json paths used by the round-trip
 * tests do NOT touch any of these — but the linker still needs a
 * definition once we link widget_rules.c.
 *
 * Behaviour:
 *   • signal_find_by_name returns -1 unless a fake table is set up by
 *     the test (none of these tests need a positive lookup).
 *   • signal_subscribe / signal_unsubscribe count calls so the
 *     subscribe/free smoke test can assert non-zero invocations.
 *   • signal_get_by_index returns NULL — the rule eval callback
 *     short-circuits on a null signal pointer. */

static int g_subscribe_calls   = 0;
static int g_unsubscribe_calls = 0;

int16_t signal_find_by_name(const char *name) { (void)name; return -1; }

signal_t *signal_get_by_index(uint16_t index) { (void)index; return NULL; }

uint16_t signal_get_count(void) { return 0; }

bool signal_subscribe(int16_t signal_index, signal_update_cb_t cb,
                      void *user_data) {
    (void)signal_index; (void)cb; (void)user_data;
    g_subscribe_calls++;
    return true;
}

bool signal_unsubscribe(int16_t signal_index, signal_update_cb_t cb,
                        void *user_data) {
    (void)signal_index; (void)cb; (void)user_data;
    g_unsubscribe_calls++;
    return true;
}

/* Pull the firmware source under test in directly (single-TU build).
 * Note: widget_rules.c relies on safe_strncpy from widget_types.h and
 * the rule structs declared there. */
#include "../../main/widgets/widget_rules.c"

/* ── Test helpers ─────────────────────────────────────────────────────── */

/* Allocate a stack-zero widget_t with the bare minimum fields touched
 * by widget_rules.c: rules, rule_count, last_rule_mask, root, id,
 * apply_overrides. We do NOT need a real LVGL object. */
static widget_t make_widget(void) {
    widget_t w = {0};
    safe_strncpy(w.id, "test_widget", sizeof(w.id));
    return w;
}

static void free_widget(widget_t *w) {
    /* widget_rules_free() handles the rules array AND signal
     * unsubscribe — same lifecycle the firmware uses. */
    widget_rules_free(w);
}

/* Build a single-override rule object as JSON. Caller owns the result. */
static cJSON *make_rule_obj(const char *signal_name, const char *op,
                            double threshold,
                            const char *field, const char *type,
                            cJSON *value /* takes ownership */) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "signal_name", signal_name);
    cJSON_AddStringToObject(r, "op", op);
    cJSON_AddNumberToObject(r, "threshold", threshold);

    cJSON *ovs = cJSON_AddArrayToObject(r, "overrides");
    cJSON *ov  = cJSON_CreateObject();
    cJSON_AddStringToObject(ov, "field", field);
    cJSON_AddStringToObject(ov, "type",  type);
    cJSON_AddItemToObject(ov, "value", value);
    cJSON_AddItemToArray(ovs, ov);
    return r;
}

/* Wrap a single rule object into a {"rules": [<rule>]} config. */
static cJSON *config_with_rule(cJSON *rule) {
    cJSON *cfg = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(cfg, "rules");
    cJSON_AddItemToArray(arr, rule);
    return cfg;
}

/* ── Test 1: Empty rules round-trip ───────────────────────────────────── */

static void test_empty_no_rules_key(void) {
    widget_t w = make_widget();
    cJSON *cfg = cJSON_CreateObject();   /* No "rules" key at all. */

    widget_rules_from_json(&w, cfg);

    TEST_ASSERT_NULL(w.rules);
    TEST_ASSERT_EQUAL_INT(0, w.rule_count);

    cJSON_Delete(cfg);
    free_widget(&w);
}

static void test_empty_to_json_emits_no_rules_key(void) {
    /* A widget with zero rules must NOT add a "rules" key at all
     * (defaults-only convention, otherwise the JSON budget bloats). */
    widget_t w = make_widget();
    cJSON *out = cJSON_CreateObject();

    widget_rules_to_json(&w, out);

    cJSON *rules = cJSON_GetObjectItemCaseSensitive(out, "rules");
    TEST_ASSERT_NULL(rules);

    cJSON_Delete(out);
}

static void test_empty_array_input_yields_zero_rules(void) {
    /* An explicit empty array should also produce zero rules. */
    widget_t w = make_widget();
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddArrayToObject(cfg, "rules");

    widget_rules_from_json(&w, cfg);

    TEST_ASSERT_NULL(w.rules);
    TEST_ASSERT_EQUAL_INT(0, w.rule_count);

    cJSON_Delete(cfg);
    free_widget(&w);
}

/* ── Test 2: Single rule, full round-trip ─────────────────────────────── */

static void test_single_rule_roundtrip_fields_preserved(void) {
    /* Build IN: { rules: [ {signal_name:"RPM", op:">", threshold:7000,
     *                       overrides:[{field:"border_color", type:"color",
     *                                   value:0xFF0000}] } ] } */
    cJSON *rule = make_rule_obj("RPM", ">", 7000.0,
                                "border_color", "color",
                                cJSON_CreateNumber(0xFF0000));
    cJSON *cfg_in = config_with_rule(rule);

    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);

    TEST_ASSERT_NOT_NULL(w.rules);
    TEST_ASSERT_EQUAL_INT(1, w.rule_count);
    TEST_ASSERT_EQUAL_STRING("RPM", w.rules[0].signal_name);
    TEST_ASSERT_EQUAL_INT(RULE_OP_GT, w.rules[0].op);
    TEST_ASSERT_EQUAL_FLOAT(7000.0f, w.rules[0].threshold);
    TEST_ASSERT_EQUAL_INT(1, w.rules[0].override_count);
    TEST_ASSERT_EQUAL_STRING("border_color", w.rules[0].overrides[0].field_name);
    TEST_ASSERT_EQUAL_INT(RULE_VAL_COLOR, w.rules[0].overrides[0].value_type);
    TEST_ASSERT_EQUAL_HEX(0xFF0000, w.rules[0].overrides[0].value.color);

    /* Round-trip OUT */
    cJSON *cfg_out = cJSON_CreateObject();
    widget_rules_to_json(&w, cfg_out);

    cJSON *rules_out = cJSON_GetObjectItemCaseSensitive(cfg_out, "rules");
    TEST_ASSERT_NOT_NULL(rules_out);
    TEST_ASSERT_TRUE(cJSON_IsArray(rules_out));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(rules_out));

    /* Re-parse OUT into a fresh widget — every field must match. */
    widget_t w2 = make_widget();
    widget_rules_from_json(&w2, cfg_out);

    TEST_ASSERT_EQUAL_INT(1, w2.rule_count);
    TEST_ASSERT_EQUAL_STRING(w.rules[0].signal_name,    w2.rules[0].signal_name);
    TEST_ASSERT_EQUAL_INT   (w.rules[0].op,             w2.rules[0].op);
    TEST_ASSERT_EQUAL_FLOAT (w.rules[0].threshold,      w2.rules[0].threshold);
    TEST_ASSERT_EQUAL_INT   (w.rules[0].override_count, w2.rules[0].override_count);
    TEST_ASSERT_EQUAL_STRING(w.rules[0].overrides[0].field_name,
                             w2.rules[0].overrides[0].field_name);
    TEST_ASSERT_EQUAL_INT   (w.rules[0].overrides[0].value_type,
                             w2.rules[0].overrides[0].value_type);
    TEST_ASSERT_EQUAL_HEX   (w.rules[0].overrides[0].value.color,
                             w2.rules[0].overrides[0].value.color);

    cJSON_Delete(cfg_in);
    cJSON_Delete(cfg_out);
    free_widget(&w);
    free_widget(&w2);
}

/* ── Test 3: Operator coverage ────────────────────────────────────────── */

static void roundtrip_operator(const char *op_str, rule_operator_t expected) {
    cJSON *rule = make_rule_obj("RPM", op_str, 100.0,
                                "fg", "color", cJSON_CreateNumber(0));
    cJSON *cfg_in = config_with_rule(rule);

    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);
    TEST_ASSERT_EQUAL_INT(expected, w.rules[0].op);

    cJSON *cfg_out = cJSON_CreateObject();
    widget_rules_to_json(&w, cfg_out);

    /* Pull op string back out and compare. */
    cJSON *rules_out = cJSON_GetObjectItemCaseSensitive(cfg_out, "rules");
    cJSON *rule_out  = cJSON_GetArrayItem(rules_out, 0);
    cJSON *op_back   = cJSON_GetObjectItemCaseSensitive(rule_out, "op");
    TEST_ASSERT_TRUE(cJSON_IsString(op_back));
    TEST_ASSERT_EQUAL_STRING(op_str, op_back->valuestring);

    cJSON_Delete(cfg_in);
    cJSON_Delete(cfg_out);
    free_widget(&w);
}

static void test_op_gt(void)    { roundtrip_operator(">",    RULE_OP_GT);  }
static void test_op_lt(void)    { roundtrip_operator("<",    RULE_OP_LT);  }
static void test_op_gte(void)   { roundtrip_operator(">=",   RULE_OP_GTE); }
static void test_op_lte(void)   { roundtrip_operator("<=",   RULE_OP_LTE); }
static void test_op_eq(void)    { roundtrip_operator("==",   RULE_OP_EQ);  }
static void test_op_neq(void)   { roundtrip_operator("!=",   RULE_OP_NEQ); }

/* The "range" operator emits range_min / range_max INSTEAD of threshold.
 * It still maps round-trip but the JSON shape diverges, so it has its
 * own dedicated test below. */

static void test_op_unknown_defaults_to_eq(void) {
    /* An unknown op string in the input must not crash and must default
     * to RULE_OP_EQ per the impl. */
    cJSON *rule = make_rule_obj("RPM", "WUT", 0.0,
                                "f", "number", cJSON_CreateNumber(0));
    cJSON *cfg_in = config_with_rule(rule);

    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);
    TEST_ASSERT_EQUAL_INT(RULE_OP_EQ, w.rules[0].op);

    cJSON_Delete(cfg_in);
    free_widget(&w);
}

/* ── Test 4: Override value-type coverage ─────────────────────────────── */

static void test_override_type_number(void) {
    cJSON *rule = make_rule_obj("RPM", ">", 0.0,
                                "border_width", "number",
                                cJSON_CreateNumber(3.5));
    cJSON *cfg_in = config_with_rule(rule);
    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);

    TEST_ASSERT_EQUAL_INT(RULE_VAL_NUMBER, w.rules[0].overrides[0].value_type);
    TEST_ASSERT_EQUAL_FLOAT(3.5f, w.rules[0].overrides[0].value.num);

    /* Round-trip and re-parse. */
    cJSON *cfg_out = cJSON_CreateObject();
    widget_rules_to_json(&w, cfg_out);
    widget_t w2 = make_widget();
    widget_rules_from_json(&w2, cfg_out);
    TEST_ASSERT_EQUAL_FLOAT(3.5f, w2.rules[0].overrides[0].value.num);

    cJSON_Delete(cfg_in); cJSON_Delete(cfg_out);
    free_widget(&w); free_widget(&w2);
}

static void test_override_type_color(void) {
    cJSON *rule = make_rule_obj("RPM", ">", 0.0,
                                "fg", "color",
                                cJSON_CreateNumber(0x00AABBCC));
    cJSON *cfg_in = config_with_rule(rule);
    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);

    TEST_ASSERT_EQUAL_INT(RULE_VAL_COLOR, w.rules[0].overrides[0].value_type);
    TEST_ASSERT_EQUAL_HEX(0x00AABBCC, w.rules[0].overrides[0].value.color);

    cJSON *cfg_out = cJSON_CreateObject();
    widget_rules_to_json(&w, cfg_out);
    widget_t w2 = make_widget();
    widget_rules_from_json(&w2, cfg_out);
    TEST_ASSERT_EQUAL_HEX(0x00AABBCC, w2.rules[0].overrides[0].value.color);

    cJSON_Delete(cfg_in); cJSON_Delete(cfg_out);
    free_widget(&w); free_widget(&w2);
}

static void test_override_type_bool_true(void) {
    cJSON *rule = make_rule_obj("RPM", ">", 0.0,
                                "visible", "bool",
                                cJSON_CreateBool(true));
    cJSON *cfg_in = config_with_rule(rule);
    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);

    TEST_ASSERT_EQUAL_INT(RULE_VAL_BOOL, w.rules[0].overrides[0].value_type);
    TEST_ASSERT_TRUE(w.rules[0].overrides[0].value.flag);

    cJSON *cfg_out = cJSON_CreateObject();
    widget_rules_to_json(&w, cfg_out);
    widget_t w2 = make_widget();
    widget_rules_from_json(&w2, cfg_out);
    TEST_ASSERT_TRUE(w2.rules[0].overrides[0].value.flag);

    cJSON_Delete(cfg_in); cJSON_Delete(cfg_out);
    free_widget(&w); free_widget(&w2);
}

static void test_override_type_bool_false(void) {
    cJSON *rule = make_rule_obj("RPM", ">", 0.0,
                                "visible", "bool",
                                cJSON_CreateBool(false));
    cJSON *cfg_in = config_with_rule(rule);
    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);

    TEST_ASSERT_FALSE(w.rules[0].overrides[0].value.flag);

    cJSON *cfg_out = cJSON_CreateObject();
    widget_rules_to_json(&w, cfg_out);
    widget_t w2 = make_widget();
    widget_rules_from_json(&w2, cfg_out);
    TEST_ASSERT_FALSE(w2.rules[0].overrides[0].value.flag);

    cJSON_Delete(cfg_in); cJSON_Delete(cfg_out);
    free_widget(&w); free_widget(&w2);
}

static void test_override_type_string(void) {
    cJSON *rule = make_rule_obj("RPM", ">", 0.0,
                                "label", "string",
                                cJSON_CreateString("DANGER"));
    cJSON *cfg_in = config_with_rule(rule);
    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);

    TEST_ASSERT_EQUAL_INT(RULE_VAL_STRING, w.rules[0].overrides[0].value_type);
    TEST_ASSERT_EQUAL_STRING("DANGER", w.rules[0].overrides[0].value.str);

    cJSON *cfg_out = cJSON_CreateObject();
    widget_rules_to_json(&w, cfg_out);
    widget_t w2 = make_widget();
    widget_rules_from_json(&w2, cfg_out);
    TEST_ASSERT_EQUAL_STRING("DANGER", w2.rules[0].overrides[0].value.str);

    cJSON_Delete(cfg_in); cJSON_Delete(cfg_out);
    free_widget(&w); free_widget(&w2);
}

/* ── Test 5: Multi-rule round-trip preserves count and order ──────────── */

static void test_multi_rule_count_and_order(void) {
    cJSON *cfg_in = cJSON_CreateObject();
    cJSON *arr    = cJSON_AddArrayToObject(cfg_in, "rules");

    /* 3 rules, distinct signal_names so we can verify ordering. */
    cJSON_AddItemToArray(arr,
        make_rule_obj("RPM",   ">",  7000.0, "fg", "color",
                      cJSON_CreateNumber(0xFF0000)));
    cJSON_AddItemToArray(arr,
        make_rule_obj("WATER", ">", 105.0, "label", "string",
                      cJSON_CreateString("HOT")));
    cJSON_AddItemToArray(arr,
        make_rule_obj("OIL",   "<",   30.0, "border_w", "number",
                      cJSON_CreateNumber(2.0)));

    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);

    TEST_ASSERT_EQUAL_INT(3, w.rule_count);
    TEST_ASSERT_EQUAL_STRING("RPM",   w.rules[0].signal_name);
    TEST_ASSERT_EQUAL_STRING("WATER", w.rules[1].signal_name);
    TEST_ASSERT_EQUAL_STRING("OIL",   w.rules[2].signal_name);
    TEST_ASSERT_EQUAL_INT(RULE_OP_GT, w.rules[0].op);
    TEST_ASSERT_EQUAL_INT(RULE_OP_GT, w.rules[1].op);
    TEST_ASSERT_EQUAL_INT(RULE_OP_LT, w.rules[2].op);

    /* Round-trip and verify shape preserved. */
    cJSON *cfg_out = cJSON_CreateObject();
    widget_rules_to_json(&w, cfg_out);
    widget_t w2 = make_widget();
    widget_rules_from_json(&w2, cfg_out);

    TEST_ASSERT_EQUAL_INT(3, w2.rule_count);
    TEST_ASSERT_EQUAL_STRING("RPM",   w2.rules[0].signal_name);
    TEST_ASSERT_EQUAL_STRING("WATER", w2.rules[1].signal_name);
    TEST_ASSERT_EQUAL_STRING("OIL",   w2.rules[2].signal_name);

    cJSON_Delete(cfg_in);
    cJSON_Delete(cfg_out);
    free_widget(&w);
    free_widget(&w2);
}

static void test_multi_rule_truncates_at_max(void) {
    /* MAX_WIDGET_RULES is 16; building 20 should yield 16. */
    cJSON *cfg_in = cJSON_CreateObject();
    cJSON *arr    = cJSON_AddArrayToObject(cfg_in, "rules");

    for (int i = 0; i < 20; i++) {
        cJSON_AddItemToArray(arr,
            make_rule_obj("RPM", ">", (double)(i * 100), "fg", "color",
                          cJSON_CreateNumber(0)));
    }

    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);
    TEST_ASSERT_EQUAL_INT(MAX_WIDGET_RULES, w.rule_count);

    cJSON_Delete(cfg_in);
    free_widget(&w);
}

/* ── Test 6: Range operator round-trip ────────────────────────────────── */

static void test_range_emits_min_max_not_threshold(void) {
    /* Build a range rule manually (helper assumes threshold-form). */
    cJSON *rule = cJSON_CreateObject();
    cJSON_AddStringToObject(rule, "signal_name", "AFR");
    cJSON_AddStringToObject(rule, "op", "range");
    cJSON_AddNumberToObject(rule, "range_min", 12.5);
    cJSON_AddNumberToObject(rule, "range_max", 14.7);
    cJSON *ovs = cJSON_AddArrayToObject(rule, "overrides");
    cJSON *ov  = cJSON_CreateObject();
    cJSON_AddStringToObject(ov, "field", "fg");
    cJSON_AddStringToObject(ov, "type",  "color");
    cJSON_AddNumberToObject(ov, "value", 0x00FF00);
    cJSON_AddItemToArray(ovs, ov);

    cJSON *cfg_in = config_with_rule(rule);

    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);

    TEST_ASSERT_EQUAL_INT(RULE_OP_RANGE, w.rules[0].op);
    TEST_ASSERT_EQUAL_FLOAT(12.5f, w.rules[0].range_min);
    TEST_ASSERT_EQUAL_FLOAT(14.7f, w.rules[0].range_max);

    /* Serialize: range op MUST emit range_min/range_max and NOT threshold. */
    cJSON *cfg_out = cJSON_CreateObject();
    widget_rules_to_json(&w, cfg_out);

    cJSON *rules_out = cJSON_GetObjectItemCaseSensitive(cfg_out, "rules");
    cJSON *rule_out  = cJSON_GetArrayItem(rules_out, 0);

    cJSON *rmin_back = cJSON_GetObjectItemCaseSensitive(rule_out, "range_min");
    cJSON *rmax_back = cJSON_GetObjectItemCaseSensitive(rule_out, "range_max");
    cJSON *thr_back  = cJSON_GetObjectItemCaseSensitive(rule_out, "threshold");

    TEST_ASSERT_NOT_NULL(rmin_back);
    TEST_ASSERT_NOT_NULL(rmax_back);
    TEST_ASSERT_NULL(thr_back);   /* threshold MUST be omitted for range */
    TEST_ASSERT_EQUAL_FLOAT(12.5, rmin_back->valuedouble);
    TEST_ASSERT_EQUAL_FLOAT(14.7, rmax_back->valuedouble);

    /* Re-parse confirms the round-trip. */
    widget_t w2 = make_widget();
    widget_rules_from_json(&w2, cfg_out);
    TEST_ASSERT_EQUAL_INT(RULE_OP_RANGE, w2.rules[0].op);
    TEST_ASSERT_EQUAL_FLOAT(12.5f, w2.rules[0].range_min);
    TEST_ASSERT_EQUAL_FLOAT(14.7f, w2.rules[0].range_max);

    cJSON_Delete(cfg_in);
    cJSON_Delete(cfg_out);
    free_widget(&w);
    free_widget(&w2);
}

/* ── Test 7: Threshold-form ops omit range_min/range_max in to_json ───── */

static void test_threshold_op_omits_range_keys(void) {
    /* The to_json implementation chooses threshold OR range_*, never
     * both. Confirm the converse: a non-range op must NOT emit range_*. */
    cJSON *rule = make_rule_obj("RPM", ">", 7000.0, "fg", "color",
                                cJSON_CreateNumber(0xFF0000));
    cJSON *cfg_in = config_with_rule(rule);
    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);

    cJSON *cfg_out = cJSON_CreateObject();
    widget_rules_to_json(&w, cfg_out);
    cJSON *rules_out = cJSON_GetObjectItemCaseSensitive(cfg_out, "rules");
    cJSON *rule_out  = cJSON_GetArrayItem(rules_out, 0);

    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItemCaseSensitive(rule_out, "threshold"));
    TEST_ASSERT_NULL    (cJSON_GetObjectItemCaseSensitive(rule_out, "range_min"));
    TEST_ASSERT_NULL    (cJSON_GetObjectItemCaseSensitive(rule_out, "range_max"));

    cJSON_Delete(cfg_in);
    cJSON_Delete(cfg_out);
    free_widget(&w);
}

/* ── Test 8: subscribe + free smoke test (touches signal stubs) ───────── */

static void test_subscribe_and_free_invokes_signal_stubs(void) {
    /* Build a widget with one rule whose signal name will be looked up.
     * signal_find_by_name returns -1 (default stub), so subscribe is
     * skipped — signal_subscribe MUST NOT be called.
     * However, widget_rules_free must still tolerate the path with
     * negative signal_index and never call signal_unsubscribe. */
    g_subscribe_calls = 0;
    g_unsubscribe_calls = 0;

    cJSON *rule = make_rule_obj("RPM", ">", 7000.0, "fg", "color",
                                cJSON_CreateNumber(0xFF0000));
    cJSON *cfg_in = config_with_rule(rule);

    widget_t w = make_widget();
    widget_rules_from_json(&w, cfg_in);
    TEST_ASSERT_EQUAL_INT(1, w.rule_count);
    TEST_ASSERT_EQUAL_INT(-1, w.rules[0].signal_index);

    widget_rules_subscribe(&w);
    /* signal_find_by_name returns -1 → no subscribe calls. */
    TEST_ASSERT_EQUAL_INT(0, g_subscribe_calls);

    widget_rules_free(&w);
    /* And free skips unsubscribe for negative indices. */
    TEST_ASSERT_EQUAL_INT(0, g_unsubscribe_calls);
    TEST_ASSERT_NULL(w.rules);
    TEST_ASSERT_EQUAL_INT(0, w.rule_count);

    cJSON_Delete(cfg_in);
}

/* ── Test 9: Defaults-only emission for to_json with no rules ─────────── */

static void test_to_json_with_null_rules_no_emit(void) {
    /* Mirrors test_empty_to_json_emits_no_rules_key but with explicit
     * w.rules == NULL (the post-free or fresh-widget state). */
    widget_t w = make_widget();
    w.rules = NULL;
    w.rule_count = 0;

    cJSON *out = cJSON_CreateObject();
    widget_rules_to_json(&w, out);

    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(out, "rules"));

    cJSON_Delete(out);
}

/* ── Runner ───────────────────────────────────────────────────────────── */

int main(void) {
    UNITY_BEGIN();

    /* Empty / no-rules round-trip */
    RUN_TEST(test_empty_no_rules_key);
    RUN_TEST(test_empty_to_json_emits_no_rules_key);
    RUN_TEST(test_empty_array_input_yields_zero_rules);
    RUN_TEST(test_to_json_with_null_rules_no_emit);

    /* Single rule round-trip */
    RUN_TEST(test_single_rule_roundtrip_fields_preserved);

    /* Operator coverage */
    RUN_TEST(test_op_gt);
    RUN_TEST(test_op_lt);
    RUN_TEST(test_op_gte);
    RUN_TEST(test_op_lte);
    RUN_TEST(test_op_eq);
    RUN_TEST(test_op_neq);
    RUN_TEST(test_op_unknown_defaults_to_eq);

    /* Override type coverage */
    RUN_TEST(test_override_type_number);
    RUN_TEST(test_override_type_color);
    RUN_TEST(test_override_type_bool_true);
    RUN_TEST(test_override_type_bool_false);
    RUN_TEST(test_override_type_string);

    /* Multi-rule */
    RUN_TEST(test_multi_rule_count_and_order);
    RUN_TEST(test_multi_rule_truncates_at_max);

    /* Range operator and defaults-only emission for non-range */
    RUN_TEST(test_range_emits_min_max_not_threshold);
    RUN_TEST(test_threshold_op_omits_range_keys);

    /* subscribe / free smoke test (touches signal-layer stubs) */
    RUN_TEST(test_subscribe_and_free_invokes_signal_stubs);

    return UNITY_END();
}
