/* test_obd2_pid_table.c — the REAL Mode 01 PID table vs SAE J1979.
 *
 * Unlike the other obd2 native tests (which hand-mirror static parsing
 * logic), this one links the actual production table: obd2_pids.c has no
 * ESP-IDF/LVGL dependencies, so it compiles host-side directly (run.ps1
 * adds it as an extra source; the three custom-PID registry externs it
 * references live in obd2.c and are stubbed below).
 *
 * ── Why this test exists ────────────────────────────────────────────────
 *
 * The Mode 01 multi-PID response walker advances by (1 + response length)
 * per PID. The response length comes from this table. A wrong entry
 * desyncs the walk: every PID after the mis-sized one in a batched
 * response is either dropped or — worse — a data byte gets read as a PID
 * echo and decodes garbage into whatever signal it aliases.
 *
 * That wasn't hypothetical: PID 0x14 (O2 sensor B1S1, in the DEFAULT
 * 30-PID starter set) was declared bytes=1 while J1979 says the response
 * carries 2 data bytes (voltage + trim). Any batch containing 0x14
 * silently broke every PID after it — on a real car that presented as
 * "some OBD values never show up". This file pins every PID's effective
 * response length to a hand-checked J1979 reference so a future table
 * addition with the same class of bug fails CI instead of failing in a
 * car.
 *
 * Source of truth: SAE J1979 / ISO 15031-5 (standard PIDs) and SAE
 * J1979-DA (0x63+ / diesel PIDs).
 */
#include "unity.h"

#include "../../main/can/obd2.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* ── Stubs for the custom-PID registry (lives in obd2.c, not linked) ─── */

uint8_t obd2_custom_count(void) { return 0; }
const obd2_pid_def_t *obd2_custom_at(uint8_t index) { (void)index; return NULL; }
const obd2_pid_def_t *obd2_custom_find_svc(uint8_t service, uint16_t pid) {
    (void)service; (void)pid; return NULL;
}

/* Fail with a printf-style message (the tiny unity shim has no _MESSAGE
 * assert variants). `return`s out of the calling test on failure, matching
 * TEST_FAIL semantics. */
#define FAIL_FMT(...) \
    do { char _m[128]; snprintf(_m, sizeof(_m), __VA_ARGS__); TEST_FAIL(_m); } while (0)

/* ── J1979 reference: Mode 01 response DATA byte counts ────────────────
 *
 * Hand-transcribed from SAE J1979 (rev 2017) Appendix B / ISO 15031-5.
 * Every Mode 01 PID the firmware table defines MUST appear here — the
 * test fails on any unlisted PID so new table rows are forced to add a
 * verified reference length (that's the point).
 */
typedef struct { uint16_t pid; uint8_t len; } ref_len_t;

static const ref_len_t J1979_MODE01_LEN[] = {
    { 0x01, 4 },  /* monitor status: MIL+count + readiness B-D */
    { 0x03, 2 },  /* fuel system 1 + 2 status */
    { 0x04, 1 }, { 0x05, 1 }, { 0x06, 1 }, { 0x07, 1 }, { 0x08, 1 },
    { 0x09, 1 }, { 0x0A, 1 }, { 0x0B, 1 },
    { 0x0C, 2 },  /* RPM */
    { 0x0D, 1 }, { 0x0E, 1 }, { 0x0F, 1 },
    { 0x10, 2 },  /* MAF */
    { 0x11, 1 },
    { 0x14, 2 },  /* O2 B1S1: voltage + short trim — THE 0x14 regression */
    { 0x1C, 1 },
    { 0x1F, 2 },
    { 0x21, 2 }, { 0x22, 2 }, { 0x23, 2 },
    { 0x2C, 1 }, { 0x2D, 1 }, { 0x2F, 1 },
    { 0x30, 1 }, { 0x31, 2 },
    { 0x32, 2 },  /* evap vapor pressure, SIGNED 16-bit */
    { 0x33, 1 },
    { 0x34, 4 },  /* wide-range O2: lambda + current */
    { 0x42, 2 }, { 0x43, 2 }, { 0x44, 2 }, { 0x45, 1 }, { 0x46, 1 },
    { 0x47, 1 }, { 0x48, 1 }, { 0x49, 1 }, { 0x4A, 1 }, { 0x4B, 1 },
    { 0x4C, 1 }, { 0x4D, 2 }, { 0x4E, 2 },
    { 0x51, 1 }, { 0x52, 1 },
    { 0x5A, 1 }, { 0x5B, 1 }, { 0x5C, 1 }, { 0x5D, 2 }, { 0x5E, 2 },
    { 0x61, 1 }, { 0x62, 1 }, { 0x63, 2 },
    /* J1979-DA diesel block (lengths per the -DA supplement) */
    { 0x6D, 6 },   /* fuel pressure control: support + cmd + actual + temp */
    { 0x70, 10 },  /* boost pressure control */
    { 0x71, 6 },   /* VGT control */
    { 0x77, 5 },   /* charge air cooler temp: support + 4 sensors */
    { 0x78, 9 },   /* EGT bank 1: support + 4x 16-bit sensors */
    { 0x7A, 7 },   /* DPF bank 1: support + 3x 16-bit pressures */
};
#define REF_COUNT (sizeof(J1979_MODE01_LEN) / sizeof(J1979_MODE01_LEN[0]))

static const ref_len_t *ref_find(uint16_t pid) {
    for (size_t i = 0; i < REF_COUNT; i++) {
        if (J1979_MODE01_LEN[i].pid == pid) return &J1979_MODE01_LEN[i];
    }
    return NULL;
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

/* Every Mode 01 def's effective response length must match J1979 — and
 * every Mode 01 def must HAVE a reference entry (forces new rows to come
 * with a verified length). Known-imprecise J1979-DA rows are exempted
 * from the exact match (walk safety comes from the polled-PID gate), but
 * still must be present in the reference list. */
static void test_mode01_response_lengths_match_j1979(void) {
    /* PIDs where competing J1979-DA revisions disagree; table value must
     * be at least the decode span but the exact byte count is best-effort.
     * (Both are packed → sent solo → a mismatch can't desync a batch, and
     * the walker's polled-PID gate stops trailing-byte misreads.) */
    static const uint16_t inexact[] = { 0x6D, 0x71 };

    for (int i = 0; i < OBD2_PIDS_COUNT; i++) {
        const obd2_pid_def_t *def = &OBD2_PIDS[i];
        if (obd2_def_service(def) != 0x01) continue;

        const ref_len_t *ref = ref_find(def->pid);
        if (!ref) {
            FAIL_FMT("Mode 01 PID 0x%02X missing from J1979 reference list "
                     "(add a verified length)", def->pid);
        }

        bool exempt = false;
        for (size_t k = 0; k < sizeof(inexact) / sizeof(inexact[0]); k++) {
            if (inexact[k] == def->pid) { exempt = true; break; }
        }
        if (exempt) continue;

        if (obd2_def_resp_len(def) != ref->len) {
            FAIL_FMT("PID 0x%02X: table response length %u != J1979 %u "
                     "(batch walker would desync)",
                     def->pid, obd2_def_resp_len(def), ref->len);
        }
    }
    TEST_ASSERT_TRUE(1);
}

/* The decode span can never exceed the response length — a sub-field (or
 * single-value read) past the ECU's actual data would read padding. */
static void test_decode_span_within_response(void) {
    for (int i = 0; i < OBD2_PIDS_COUNT; i++) {
        const obd2_pid_def_t *def = &OBD2_PIDS[i];
        uint8_t resp = obd2_def_resp_len(def);
        if (resp == 0) {
            FAIL_FMT("svc 0x%02X PID 0x%02X: zero response len",
                     obd2_def_service(def), def->pid);
        }

        if (def->sub_fields && def->sub_field_count > 0) {
            for (uint8_t j = 0; j < def->sub_field_count; j++) {
                uint16_t end = (uint16_t)def->sub_fields[j].byte_offset +
                               (uint16_t)def->sub_fields[j].bytes;
                if (end > resp) {
                    FAIL_FMT("PID 0x%02X sub-field %u ends past response "
                             "(%u > %u)", def->pid, j, end, resp);
                }
            }
        } else {
            if (def->bytes > resp) {
                FAIL_FMT("PID 0x%02X: decode bytes %u > response %u",
                         def->pid, def->bytes, resp);
            }
            if (def->bytes != 1 && def->bytes != 2) {
                FAIL_FMT("PID 0x%02X: single-value decode must be 1 or 2 "
                         "bytes (got %u)", def->pid, def->bytes);
            }
        }
    }
    TEST_ASSERT_TRUE(1);
}

/* No duplicate (service, pid) rows — a dup would make obd2_pid_find_svc
 * decode with whichever row happens to come first. */
static void test_no_duplicate_service_pid(void) {
    for (int i = 0; i < OBD2_PIDS_COUNT; i++) {
        for (int j = i + 1; j < OBD2_PIDS_COUNT; j++) {
            if (OBD2_PIDS[i].pid != OBD2_PIDS[j].pid) continue;
            if (obd2_def_service(&OBD2_PIDS[i]) ==
                obd2_def_service(&OBD2_PIDS[j])) {
                FAIL_FMT("duplicate def: svc 0x%02X PID 0x%02X",
                         obd2_def_service(&OBD2_PIDS[i]), OBD2_PIDS[i].pid);
            }
        }
    }
    TEST_ASSERT_TRUE(1);
}

/* Every def must emit at least one signal (a name, or ≥1 named sub-field). */
static void test_every_def_emits_a_signal(void) {
    for (int i = 0; i < OBD2_PIDS_COUNT; i++) {
        const obd2_pid_def_t *def = &OBD2_PIDS[i];
        bool has_signal = (def->signal_name != NULL);
        if (!has_signal && def->sub_fields) {
            for (uint8_t j = 0; j < def->sub_field_count; j++) {
                if (def->sub_fields[j].signal_name) { has_signal = true; break; }
            }
        }
        if (!has_signal) {
            FAIL_FMT("svc 0x%02X PID 0x%02X emits no signal",
                     obd2_def_service(def), def->pid);
        }
    }
    TEST_ASSERT_TRUE(1);
}

/* Decode spot-checks against the J1979 formulas. */
static void test_decode_formula_spot_checks(void) {
    const obd2_pid_def_t *rpm = obd2_pid_find_svc(0x01, 0x0C);
    TEST_ASSERT_NOT_NULL(rpm);
    TEST_ASSERT_EQUAL_INT(2, rpm->bytes);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, rpm->scale);

    const obd2_pid_def_t *coolant = obd2_pid_find_svc(0x01, 0x05);
    TEST_ASSERT_NOT_NULL(coolant);
    TEST_ASSERT_EQUAL_FLOAT(-40.0f, coolant->offset);

    const obd2_pid_def_t *fuel = obd2_pid_find_svc(0x01, 0x2F);
    TEST_ASSERT_NOT_NULL(fuel);
    TEST_ASSERT_EQUAL_FLOAT(100.0f / 255.0f, fuel->scale);

    /* 0x14 decodes only byte A (voltage) but consumes 2 bytes. */
    const obd2_pid_def_t *o2 = obd2_pid_find_svc(0x01, 0x14);
    TEST_ASSERT_NOT_NULL(o2);
    TEST_ASSERT_EQUAL_INT(1, o2->bytes);
    TEST_ASSERT_EQUAL_INT(2, obd2_def_resp_len(o2));

    /* 0x32 must decode SIGNED (two's complement / 4 Pa). */
    const obd2_pid_def_t *evap = obd2_pid_find_svc(0x01, 0x32);
    TEST_ASSERT_NOT_NULL(evap);
    TEST_ASSERT_NOT_NULL(evap->sub_fields);
    TEST_ASSERT_TRUE(evap->sub_fields[0].is_signed);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, evap->sub_fields[0].scale);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, evap->sub_fields[0].offset);
}

/* Encoded-tuple helpers roundtrip across all three encoding ranges. */
static void test_encode_decode_roundtrip(void) {
    uint32_t e = obd2_encode_pid(0x01, 0x0C);
    TEST_ASSERT_EQUAL_INT(0x01, obd2_decode_service(e));
    TEST_ASSERT_EQUAL_INT(0x0C, obd2_decode_pid(e));

    e = obd2_encode_pid(0x21, 0x80);
    TEST_ASSERT_EQUAL_INT(0x21, obd2_decode_service(e));
    TEST_ASSERT_EQUAL_INT(0x80, obd2_decode_pid(e));

    e = obd2_encode_pid(0x22, 0x115C);
    TEST_ASSERT_EQUAL_INT(0x22, obd2_decode_service(e));
    TEST_ASSERT_EQUAL_INT(0x115C, obd2_decode_pid(e));

    /* Legacy bare-byte layouts decode as Mode 01. */
    TEST_ASSERT_EQUAL_INT(0x01, obd2_decode_service(0x0Cu));
    TEST_ASSERT_EQUAL_INT(0x0C, obd2_decode_pid(0x0Cu));
}

/* Default starter set: fits the enabled cap and is all Mode 01 (the
 * OBD2-primary preset seeds bare PID bytes, which decode as Mode 01). */
static void test_default_set_sane(void) {
    int defaults = 0;
    for (int i = 0; i < OBD2_PIDS_COUNT; i++) {
        if (!OBD2_PIDS[i].default_enabled) continue;
        defaults++;
        if (obd2_def_service(&OBD2_PIDS[i]) != 0x01) {
            FAIL_FMT("default-enabled PID 0x%02X is not Mode 01",
                     OBD2_PIDS[i].pid);
        }
    }
    TEST_ASSERT_TRUE(defaults > 0);
    TEST_ASSERT_TRUE(defaults <= OBD2_MAX_ENABLED);
}

/* 29-bit ISO 15765-4 id mapping. */
static void test_29bit_id_mapping(void) {
    TEST_ASSERT_EQUAL_HEX(0x18DB33F1u, OBD2_REQUEST_ID_FUNC_29);
    /* Response from engine ECU 0x10 → FC / physical request back at it. */
    TEST_ASSERT_EQUAL_HEX(0x18DA10F1u, OBD2_REQUEST_29_FOR(0x10));
    /* Response-window match. */
    TEST_ASSERT_EQUAL_HEX(OBD2_RESPONSE_29_BASE,
                          0x18DAF110u & OBD2_RESPONSE_29_MASK);
    /* An 11-bit response id must NOT match the 29-bit window. */
    TEST_ASSERT_TRUE((0x7E8u & OBD2_RESPONSE_29_MASK) != OBD2_RESPONSE_29_BASE);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_mode01_response_lengths_match_j1979);
    RUN_TEST(test_decode_span_within_response);
    RUN_TEST(test_no_duplicate_service_pid);
    RUN_TEST(test_every_def_emits_a_signal);
    RUN_TEST(test_decode_formula_spot_checks);
    RUN_TEST(test_encode_decode_roundtrip);
    RUN_TEST(test_default_set_sane);
    RUN_TEST(test_29bit_id_mapping);
    return UNITY_END();
}
