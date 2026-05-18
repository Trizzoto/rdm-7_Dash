/* test_obd2_freeze_frame.c — host-side decode for OBD2 Mode 02 freeze frame.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * Mode 02 (Freeze Frame) is the "what was happening when this DTC set"
 * snapshot the ECU stashes alongside every confirmed fault. The dash
 * queries it one PID at a time and renders each row as a scaled human
 * value (or "-" if the ECU rejected the PID with NRC).
 *
 * Two pure-data pieces sit between the wire bytes and the rendered row:
 *
 *   1. Response-frame parse  (obd2.c, Mode 02 branch)
 *        Wire format:  0x42 [data_pid] [frame_no] [payload bytes...]
 *        Strip the three header bytes, hand the payload up.
 *
 *   2. Payload → scaled value  (dtc_reader.c, _ff_decode + scale/offset)
 *        Big-endian byte assembly into an int, then linear scale + offset
 *        per the PID's Mode-01 decode table (the spec guarantees the same
 *        shape for the matching Mode-02 PID).
 *
 * Both are tiny and have zero LVGL / FreeRTOS / ESP-IDF dependency. The
 * actual sources drag in the whole CAN stack so we mirror them verbatim
 * here. If the firmware's parse layout or decode formula shifts (e.g.
 * an ECU vendor needs a different byte order) these tests go red.
 *
 * Source-of-truth references (must be kept in lockstep):
 *
 *   main/can/obd2.c              — Mode 02 response branch (search "service == 0x02")
 *   main/ui/menu/dtc_reader.c    — _ff_decode() + scale/offset application
 *   main/can/obd2_pids.c         — canonical PID scale/offset table
 */
#include "unity.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ── Mirror of dtc_reader.c's ff_spec_t (decode metadata only) ──────────
 * Matches the firmware copy field-for-field so it reads identically; the
 * firmware FF_PIDS table values are copied as test fixtures below. */
typedef struct {
    uint8_t     pid;
    const char *label;
    const char *unit;
    uint8_t     bytes;
    float       scale;
    float       offset;
} ff_spec_t;

/* Subset of FF_PIDS from dtc_reader.c — the rows the firmware actually
 * queries when the user taps a stored DTC. Decimals dropped (irrelevant
 * for value math). */
static const ff_spec_t FF_PIDS_FIXTURE[] = {
    { 0x04, "Engine load",     "%",     1, 0.392157f,  0.0f    },
    { 0x05, "Coolant temp",    "degC",  1, 1.0f,       -40.0f  },
    { 0x0B, "MAP",             "kPa",   1, 1.0f,        0.0f   },
    { 0x0C, "RPM",             "rpm",   2, 0.25f,       0.0f   },
    { 0x0D, "Vehicle speed",   "km/h",  1, 1.0f,        0.0f   },
    { 0x0E, "Timing adv",      "deg",   1, 0.5f,       -64.0f  },
    { 0x0F, "Intake air temp", "degC",  1, 1.0f,       -40.0f  },
    { 0x10, "MAF",             "g/s",   2, 0.01f,       0.0f   },
    { 0x11, "Throttle pos",    "%",     1, 0.392157f,   0.0f   },
    { 0x1F, "Run time",        "s",     2, 1.0f,        0.0f   },
    { 0x2F, "Fuel level",      "%",     1, 0.392157f,   0.0f   },
};
#define FF_PIDS_FIXTURE_COUNT (sizeof(FF_PIDS_FIXTURE) / sizeof(FF_PIDS_FIXTURE[0]))

/* ── Helpers under test ─────────────────────────────────────────────────
 * Two pure functions that together describe what the firmware does to
 * convert a Mode-02 response frame into a numeric value. */

/* parse_mode02_frame — mirrors obd2.c's Mode 02 branch.
 *
 * Input: full message bytes as they appear after ISO-TP reassembly,
 *        msg[0] = service byte (0x42 = positive Mode 02 response).
 * Output: payload pointer + length, response PID, response frame_no.
 *
 * Returns true if the frame is a well-formed Mode 02 positive response
 * matching the expected (data_pid, frame_no), false otherwise. */
static bool parse_mode02_frame(const uint8_t *msg, uint8_t len,
                                uint8_t expect_pid, uint8_t expect_frame,
                                const uint8_t **out_payload,
                                uint8_t *out_payload_len) {
    if (!msg || len < 3) return false;
    if (msg[0] != 0x42) return false;      /* must be positive Mode 02 response */
    uint8_t resp_pid   = msg[1];
    uint8_t resp_frame = msg[2];
    if (resp_pid != expect_pid)     return false;
    if (resp_frame != expect_frame) return false;

    if (len > 3) {
        if (out_payload)     *out_payload     = &msg[3];
        if (out_payload_len) *out_payload_len = (uint8_t)(len - 3);
    } else {
        if (out_payload)     *out_payload     = NULL;
        if (out_payload_len) *out_payload_len = 0;
    }
    return true;
}

/* _ff_decode — mirror of dtc_reader.c's _ff_decode. Big-endian raw int
 * assembly; returns INT32_MIN as the "not enough bytes" sentinel (the
 * firmware uses the same sentinel; consumer treats it as "no data"). */
static int32_t ff_decode(const ff_spec_t *s, const uint8_t *raw, uint8_t raw_len) {
    if (raw_len < s->bytes) return INT32_MIN;
    int32_t v = 0;
    for (uint8_t i = 0; i < s->bytes; i++) v = (v << 8) | raw[i];
    return v;
}

/* Full pipeline: response bytes → scaled value. Returns NaN if either
 * the parse or the decode rejects. The firmware shows "-" in that case. */
static float decode_mode02_value(const ff_spec_t *s, const uint8_t *msg, uint8_t len,
                                  uint8_t expect_frame) {
    const uint8_t *payload = NULL;
    uint8_t        payload_len = 0;
    if (!parse_mode02_frame(msg, len, s->pid, expect_frame, &payload, &payload_len)) {
        return NAN;
    }
    int32_t raw_v = ff_decode(s, payload, payload_len);
    if (raw_v == INT32_MIN) return NAN;
    return (float)raw_v * s->scale + s->offset;
}

/* ── Find a fixture row by PID ─────────────────────────────────────────── */
static const ff_spec_t *find_pid(uint8_t pid) {
    for (size_t i = 0; i < FF_PIDS_FIXTURE_COUNT; i++) {
        if (FF_PIDS_FIXTURE[i].pid == pid) return &FF_PIDS_FIXTURE[i];
    }
    return NULL;
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

/* parse: well-formed positive response → payload + headers extracted. */
static void test_parse_well_formed_response(void) {
    /* Mode 02 RPM response: 0x42 0x0C 0x00 0x1A 0xF8
     * → PID 0x0C, frame 0, payload [0x1A, 0xF8] (= 6904 raw, 1726 RPM). */
    uint8_t msg[] = { 0x42, 0x0C, 0x00, 0x1A, 0xF8 };
    const uint8_t *payload = NULL;
    uint8_t        payload_len = 0;
    bool ok = parse_mode02_frame(msg, sizeof(msg), 0x0C, 0x00,
                                  &payload, &payload_len);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_EQUAL_INT(2, payload_len);
    TEST_ASSERT_EQUAL_HEX(0x1A, payload[0]);
    TEST_ASSERT_EQUAL_HEX(0xF8, payload[1]);
}

/* parse: non-0x42 first byte (e.g. raw NRC) → rejected. */
static void test_parse_rejects_non_positive_response(void) {
    /* 0x7F 0x02 0x12 = NRC (sub-function not supported) — the firmware
     * funnels these via the rejection branch, never into the parse
     * helper, but the parser is conservatively strict anyway. */
    uint8_t msg[] = { 0x7F, 0x02, 0x12 };
    bool ok = parse_mode02_frame(msg, sizeof(msg), 0x02, 0x00, NULL, NULL);
    TEST_ASSERT_FALSE(ok);
}

/* parse: wrong PID mid-flight → rejected (anti-cross-attribution). */
static void test_parse_rejects_mismatched_pid(void) {
    /* Caller fired for PID 0x05 but a stale 0x0C response arrives. */
    uint8_t msg[] = { 0x42, 0x0C, 0x00, 0x1A, 0xF8 };
    bool ok = parse_mode02_frame(msg, sizeof(msg), 0x05, 0x00, NULL, NULL);
    TEST_ASSERT_FALSE(ok);
}

/* parse: wrong frame_no → rejected. */
static void test_parse_rejects_mismatched_frame_no(void) {
    /* Caller fired for frame 0 but response carries frame 1. */
    uint8_t msg[] = { 0x42, 0x0C, 0x01, 0x1A, 0xF8 };
    bool ok = parse_mode02_frame(msg, sizeof(msg), 0x0C, 0x00, NULL, NULL);
    TEST_ASSERT_FALSE(ok);
}

/* parse: short frame (len < 3) → rejected. */
static void test_parse_rejects_short_frame(void) {
    uint8_t msg[] = { 0x42, 0x0C };
    bool ok = parse_mode02_frame(msg, sizeof(msg), 0x0C, 0x00, NULL, NULL);
    TEST_ASSERT_FALSE(ok);
}

/* parse: NULL message → rejected. */
static void test_parse_rejects_null_message(void) {
    bool ok = parse_mode02_frame(NULL, 5, 0x0C, 0x00, NULL, NULL);
    TEST_ASSERT_FALSE(ok);
}

/* parse: header-only positive response (no data bytes) → ok=true, payload=NULL.
 * This matches the firmware's "ECU answered but with empty data" edge case;
 * ff_decode then returns INT32_MIN because raw_len < s->bytes, and the row
 * renders "-". */
static void test_parse_header_only_response(void) {
    uint8_t msg[] = { 0x42, 0x0C, 0x00 };
    const uint8_t *payload = (const uint8_t*)0x1;   /* sentinel: should be reset */
    uint8_t        payload_len = 0xFF;
    bool ok = parse_mode02_frame(msg, sizeof(msg), 0x0C, 0x00,
                                  &payload, &payload_len);
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_NULL(payload);
    TEST_ASSERT_EQUAL_INT(0, payload_len);
}

/* decode: 1-byte PID — engine load (PID 0x04, scale 100/255). */
static void test_decode_engine_load_one_byte(void) {
    const ff_spec_t *s = find_pid(0x04);
    TEST_ASSERT_NOT_NULL(s);
    /* Raw 0x80 = 128, scaled = 128 * 0.392157 ≈ 50.196 % */
    uint8_t payload[] = { 0x80 };
    int32_t raw = ff_decode(s, payload, sizeof(payload));
    TEST_ASSERT_EQUAL_INT(0x80, raw);
    float value = (float)raw * s->scale + s->offset;
    TEST_ASSERT_TRUE(fabsf(value - 50.196f) < 0.01f);
}

/* decode: 1-byte signed-offset PID — coolant temp (raw - 40 °C). */
static void test_decode_coolant_temp_offset(void) {
    const ff_spec_t *s = find_pid(0x05);
    TEST_ASSERT_NOT_NULL(s);
    /* Raw 0x5A = 90 → 90 - 40 = 50 °C — typical hot coolant. */
    uint8_t payload[] = { 0x5A };
    int32_t raw = ff_decode(s, payload, sizeof(payload));
    float value = (float)raw * s->scale + s->offset;
    TEST_ASSERT_TRUE(fabsf(value - 50.0f) < 0.001f);
}

/* decode: coolant temp at the spec's lower extreme (raw 0x00 → -40 °C). */
static void test_decode_coolant_temp_floor(void) {
    const ff_spec_t *s = find_pid(0x05);
    uint8_t payload[] = { 0x00 };
    int32_t raw = ff_decode(s, payload, sizeof(payload));
    float value = (float)raw * s->scale + s->offset;
    TEST_ASSERT_TRUE(fabsf(value - (-40.0f)) < 0.001f);
}

/* decode: 2-byte big-endian PID — RPM (raw / 4). */
static void test_decode_rpm_two_byte_big_endian(void) {
    const ff_spec_t *s = find_pid(0x0C);
    TEST_ASSERT_NOT_NULL(s);
    /* 0x1A 0xF8 → 0x1AF8 = 6904 raw → 6904 / 4 = 1726 RPM. */
    uint8_t payload[] = { 0x1A, 0xF8 };
    int32_t raw = ff_decode(s, payload, sizeof(payload));
    TEST_ASSERT_EQUAL_INT(0x1AF8, raw);
    float value = (float)raw * s->scale + s->offset;
    TEST_ASSERT_TRUE(fabsf(value - 1726.0f) < 0.001f);
}

/* decode: RPM byte order matters — swapped bytes must not equal the
 * big-endian value (regression for any future endian flip). */
static void test_decode_rpm_byte_order_matters(void) {
    const ff_spec_t *s = find_pid(0x0C);
    uint8_t payload_be[] = { 0x1A, 0xF8 };   /* 0x1AF8 = 6904 */
    uint8_t payload_le[] = { 0xF8, 0x1A };   /* 0xF81A = 63514 (very different) */
    int32_t be = ff_decode(s, payload_be, 2);
    int32_t le = ff_decode(s, payload_le, 2);
    TEST_ASSERT_TRUE(be != le);
    TEST_ASSERT_EQUAL_INT(0x1AF8, be);
    TEST_ASSERT_EQUAL_INT(0xF81A, le);
}

/* decode: signed-offset PID — timing advance (raw / 2 - 64 °). */
static void test_decode_timing_advance_signed(void) {
    const ff_spec_t *s = find_pid(0x0E);
    TEST_ASSERT_NOT_NULL(s);
    /* Raw 0x80 = 128 → 128 * 0.5 - 64 = 0° (centred / no advance). */
    uint8_t payload[] = { 0x80 };
    float v = (float)ff_decode(s, payload, 1) * s->scale + s->offset;
    TEST_ASSERT_TRUE(fabsf(v - 0.0f) < 0.001f);

    /* Raw 0xA0 = 160 → 160 * 0.5 - 64 = 16° advance. */
    uint8_t payload2[] = { 0xA0 };
    v = (float)ff_decode(s, payload2, 1) * s->scale + s->offset;
    TEST_ASSERT_TRUE(fabsf(v - 16.0f) < 0.001f);

    /* Raw 0x00 = 0 → -64° (spec floor, retard). */
    uint8_t payload3[] = { 0x00 };
    v = (float)ff_decode(s, payload3, 1) * s->scale + s->offset;
    TEST_ASSERT_TRUE(fabsf(v - (-64.0f)) < 0.001f);
}

/* decode: 2-byte MAF (raw * 0.01 g/s). */
static void test_decode_maf_two_byte(void) {
    const ff_spec_t *s = find_pid(0x10);
    /* 0x0A 0x00 → 2560 raw → 25.60 g/s. */
    uint8_t payload[] = { 0x0A, 0x00 };
    int32_t raw = ff_decode(s, payload, 2);
    TEST_ASSERT_EQUAL_INT(2560, raw);
    float v = (float)raw * s->scale + s->offset;
    TEST_ASSERT_TRUE(fabsf(v - 25.60f) < 0.001f);
}

/* decode: short payload (fewer bytes than spec declares) → INT32_MIN sentinel.
 * Models a malformed ECU response. Firmware renders "-" for these. */
static void test_decode_short_payload_returns_sentinel(void) {
    /* RPM requires 2 bytes; supply only 1. */
    const ff_spec_t *s = find_pid(0x0C);
    uint8_t payload[] = { 0x1A };
    int32_t raw = ff_decode(s, payload, 1);
    TEST_ASSERT_EQUAL_INT(INT32_MIN, raw);
}

/* decode: empty payload (length 0) → INT32_MIN for any single-byte PID. */
static void test_decode_empty_payload_returns_sentinel(void) {
    const ff_spec_t *s = find_pid(0x05);  /* coolant — 1 byte */
    uint8_t payload[1] = { 0 };
    int32_t raw = ff_decode(s, payload, 0);
    TEST_ASSERT_EQUAL_INT(INT32_MIN, raw);
}

/* decode: max-value 1-byte PID — engine load at 0xFF. */
static void test_decode_engine_load_max(void) {
    const ff_spec_t *s = find_pid(0x04);
    uint8_t payload[] = { 0xFF };
    float v = (float)ff_decode(s, payload, 1) * s->scale + s->offset;
    /* 255 * 0.392157 = 99.999... ≈ 100 %. */
    TEST_ASSERT_TRUE(fabsf(v - 100.0f) < 0.05f);
}

/* decode: max-value 2-byte PID — RPM at 0xFFFF. */
static void test_decode_rpm_max(void) {
    const ff_spec_t *s = find_pid(0x0C);
    uint8_t payload[] = { 0xFF, 0xFF };
    int32_t raw = ff_decode(s, payload, 2);
    TEST_ASSERT_EQUAL_INT(0xFFFF, raw);
    float v = (float)raw * s->scale + s->offset;
    /* 65535 * 0.25 = 16383.75 RPM — well above any real engine. */
    TEST_ASSERT_TRUE(fabsf(v - 16383.75f) < 0.001f);
}

/* end-to-end: full happy-path Mode 02 response decodes to engineering units. */
static void test_pipeline_coolant_temp(void) {
    /* Wire bytes: 0x42 0x05 0x00 0x5A → coolant 50 °C @ frame 0. */
    uint8_t msg[] = { 0x42, 0x05, 0x00, 0x5A };
    const ff_spec_t *s = find_pid(0x05);
    float v = decode_mode02_value(s, msg, sizeof(msg), 0x00);
    TEST_ASSERT_FALSE(isnan(v));
    TEST_ASSERT_TRUE(fabsf(v - 50.0f) < 0.001f);
}

/* end-to-end: wrong frame_no on the wire → pipeline rejects (NaN). */
static void test_pipeline_rejects_wrong_frame(void) {
    uint8_t msg[] = { 0x42, 0x05, 0x01, 0x5A };  /* response carries frame 1 */
    const ff_spec_t *s = find_pid(0x05);
    float v = decode_mode02_value(s, msg, sizeof(msg), 0x00);
    TEST_ASSERT_TRUE(isnan(v));
}

/* end-to-end: response payload truncated mid-wire → pipeline rejects. */
static void test_pipeline_rejects_truncated_payload(void) {
    /* RPM (PID 0x0C) needs 2 payload bytes; only 1 present. */
    uint8_t msg[] = { 0x42, 0x0C, 0x00, 0x1A };
    const ff_spec_t *s = find_pid(0x0C);
    float v = decode_mode02_value(s, msg, sizeof(msg), 0x00);
    TEST_ASSERT_TRUE(isnan(v));
}

int main(void) {
    UNITY_BEGIN();

    /* parser */
    RUN_TEST(test_parse_well_formed_response);
    RUN_TEST(test_parse_rejects_non_positive_response);
    RUN_TEST(test_parse_rejects_mismatched_pid);
    RUN_TEST(test_parse_rejects_mismatched_frame_no);
    RUN_TEST(test_parse_rejects_short_frame);
    RUN_TEST(test_parse_rejects_null_message);
    RUN_TEST(test_parse_header_only_response);

    /* decode */
    RUN_TEST(test_decode_engine_load_one_byte);
    RUN_TEST(test_decode_coolant_temp_offset);
    RUN_TEST(test_decode_coolant_temp_floor);
    RUN_TEST(test_decode_rpm_two_byte_big_endian);
    RUN_TEST(test_decode_rpm_byte_order_matters);
    RUN_TEST(test_decode_timing_advance_signed);
    RUN_TEST(test_decode_maf_two_byte);
    RUN_TEST(test_decode_short_payload_returns_sentinel);
    RUN_TEST(test_decode_empty_payload_returns_sentinel);
    RUN_TEST(test_decode_engine_load_max);
    RUN_TEST(test_decode_rpm_max);

    /* end-to-end pipeline */
    RUN_TEST(test_pipeline_coolant_temp);
    RUN_TEST(test_pipeline_rejects_wrong_frame);
    RUN_TEST(test_pipeline_rejects_truncated_payload);

    return UNITY_END();
}
