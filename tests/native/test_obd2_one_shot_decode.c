/* test_obd2_one_shot_decode.c — host-side decode for OBD2 VIN / ECU-name /
 * DTC read one-shot responses.
 *
 * ── Why these tests exist ────────────────────────────────────────────────
 *
 * obd2.c has 6 one-shot request/response state machines (test_pid, DTC
 * read x3 modes, DTC clear, VIN, freeze-frame, ECU-name) that share a lot
 * of arm/TX/timeout/NRC boilerplate — a real refactor-audit finding
 * (collapse into one generic dispatch table). Before attempting that
 * consolidation, the response-side BYTE PARSING for each type needs
 * lock-step coverage, same as test_obd2_freeze_frame.c already gives
 * Mode 02. This file covers the three pieces that test doesn't: VIN
 * (Mode 09 PID 0x02), ECU name (Mode 09 PID 0x0A), and DTC pair decoding
 * (Modes 03/07/0A).
 *
 * IMPORTANT SCOPE NOTE: this covers byte-level decode only, NOT the
 * arm/timeout/NRC state-machine glue the planned consolidation would
 * actually touch. obd2.c #includes can_manager.h, which pulls in
 * driver/twai.h + esp_err.h + FreeRTOS — mocking that whole surface just
 * to compile the real state machine on-host is a much bigger investment
 * than this file represents. Testing the state-machine glue itself needs
 * either that mock investment or a real ECU/dongle on the bench (the
 * bench simulator only covers Mode 01 polling — see obd2.c's
 * _sim_consume_tx, which rejects anything but Mode 01 requests).
 *
 * This scope note aged out mid-write: hand-tracing the VIN/ECU-name
 * mirrors against the real code (rather than just eyeballing it) caught
 * two real bugs in main/can/obd2.c's leading-padding strip — a
 * NUL-terminated pointer walk that could never actually strip a leading
 * NUL byte (despite the code comment claiming it did), and a fixed-size
 * take-window applied before the strip that truncated the VIN's tail by
 * however many padding bytes preceded it. Both were small, isolated to
 * post-copy buffer cleanup (no state-machine/control-flow changes), had
 * this file's tests ready to validate them, and are now fixed in
 * obd2.c — see the VIN branch's comments there for the detail.
 *
 * Source-of-truth references (must be kept in lockstep):
 *   main/can/obd2.c — VIN branch (search "Mode 09 PID 0x02"),
 *                      ECU-name branch (search "Mode 09 PID 0x0A"),
 *                      DTC decode (_decode_dtc_pair, _dtc_decode_payload,
 *                      and the "maybe_count" heuristic in the Mode
 *                      03/07/0A branch of _process_full_message).
 *
 * obd2.h itself has zero ESP-IDF/LVGL dependencies (verified: only
 * <stdbool.h>/<stdint.h>), so it's included directly here for the
 * obd2_dtc_t struct shape and OBD2_MAX_DTCS constant — only the parsing
 * LOGIC (which lives in static functions inside obd2.c, unreachable
 * without the full mock investment above) is mirrored by hand.
 */
#include "unity.h"

#include "../../main/can/obd2.h"

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── VIN decode (mirrors obd2.c's Mode 09 PID 0x02 branch) ─────────────
 * Wire format: 0x49 0x02 [DI byte, optional] <ASCII bytes>
 * DI byte (0x01-0x05, "number of data items") is present only when the
 * remaining length matches DI*17+1 exactly — VIN is always 17 chars, so
 * this length check disambiguates "DI byte present" from "first VIN char
 * happens to be a small number". `p`/`plen` mirror msg+2 / len-2 in the
 * real code (i.e. the payload starting right after the service+PID
 * echo bytes). */
static void parse_vin_payload(const uint8_t *p, uint16_t plen, char out_vin[18]) {
    if (plen > 0 && p[0] >= 0x01 && p[0] <= 0x05 && plen >= (uint16_t)(p[0] * 17 + 1)) {
        p++;
        plen--;
    }
    /* Strip any further leading non-printable padding (e.g. raw NUL fill)
     * BEFORE imposing the fixed 17-byte VIN window — this test caught a
     * real bug in main/can/obd2.c where a NUL-terminated `*start` pointer
     * walk ran AFTER the window was taken, so it (a) never actually
     * stripped a leading NUL (0x00 looks like end-of-string to that loop
     * guard) and (b) even once fixed to scan by index, padding taken
     * before the strip still ate into the 17-byte budget and truncated
     * the real VIN's tail. Stripping first, then taking, fixes both. */
    while (plen > 0 && (p[0] < 0x20 || p[0] > 0x7E)) {
        p++;
        plen--;
    }
    uint8_t take = (plen < 17) ? (uint8_t)plen : 17;
    memcpy(out_vin, p, take);
    out_vin[take] = '\0';
}

/* ── ECU-name decode (mirrors obd2.c's Mode 09 PID 0x0A branch) ─────────
 * Same DI-byte convention as VIN, but WITHOUT the length-match gate (ECU
 * name has no fixed length, so the DI check is just "byte 0 is 1-5") —
 * and it trims TRAILING whitespace/non-printable too, which VIN does not
 * (VIN is fixed-width so ECUs don't pad it). These two differences are
 * exactly the kind of detail a "let's just reuse the VIN parser" merge
 * during consolidation could silently drop. */
static void parse_ecuname_payload(const uint8_t *p, uint16_t plen,
                                  char *out_name, size_t out_cap) {
    if (plen > 0 && p[0] >= 0x01 && p[0] <= 0x05) {
        p++;
        plen--;
    }
    /* Strip leading non-printable padding before imposing the
     * output-buffer window — same reasoning as parse_vin_payload above. */
    while (plen > 0 && (p[0] < 0x20 || p[0] > 0x7E)) {
        p++;
        plen--;
    }
    uint16_t take = (plen < (uint16_t)(out_cap - 1)) ? plen : (uint16_t)(out_cap - 1);
    memcpy(out_name, p, take);
    out_name[take] = '\0';

    size_t blen = strlen(out_name);
    while (blen > 0 && (out_name[blen - 1] <= 0x20)) {
        out_name[--blen] = '\0';
    }
}

/* ── DTC pair decode (mirrors obd2.c's _decode_dtc_pair) ─────────────────
 * Per SAE J2012: top 2 bits of byte A select the category letter, next 2
 * bits the first digit (0-3), remaining 12 bits are 3 hex digits. */
static void decode_dtc_pair(uint8_t a, uint8_t b, char out[6]) {
    static const char categories[4] = { 'P', 'C', 'B', 'U' };
    static const char hex[17]       = "0123456789ABCDEF";
    out[0] = categories[(a >> 6) & 0x03];
    out[1] = (char)('0' + ((a >> 4) & 0x03));
    out[2] = hex[a & 0x0F];
    out[3] = hex[(b >> 4) & 0x0F];
    out[4] = hex[b & 0x0F];
    out[5] = '\0';
}

/* ── DTC payload walk (mirrors obd2.c's _dtc_decode_payload) ────────────
 * Walks 2-byte pairs, stops at 0x00 0x00 padding or OBD2_MAX_DTCS. */
static uint8_t decode_dtc_payload(const uint8_t *p, uint16_t len,
                                  obd2_dtc_t out[OBD2_MAX_DTCS]) {
    uint8_t count = 0;
    while (len >= 2 && count < OBD2_MAX_DTCS) {
        if (p[0] == 0x00 && p[1] == 0x00) break;
        decode_dtc_pair(p[0], p[1], out[count].code);
        out[count].status = 0;
        count++;
        p   += 2;
        len -= 2;
    }
    return count;
}

/* ── "maybe_count" heuristic (mirrors the Mode 03/07/0A branch's own
 * disambiguation in _process_full_message) ─────────────────────────────
 * Some ECUs prefix the DTC pairs with an explicit count byte; others
 * don't. Heuristic: if count*2+1 == plen, byte 0 IS the count (skip it).
 * `p`/`plen` mirror msg+1 / len-1 (payload right after the service echo
 * byte — DTC responses have no PID byte, unlike VIN/ECU-name). */
static uint8_t decode_dtc_response(const uint8_t *p, uint16_t plen,
                                   obd2_dtc_t out[OBD2_MAX_DTCS]) {
    if (plen >= 1) {
        uint8_t maybe_count = p[0];
        if ((uint16_t)(maybe_count * 2 + 1) == plen) {
            p++;
            plen--;
        }
    }
    return decode_dtc_payload(p, plen, out);
}

/* ── Tests: VIN ──────────────────────────────────────────────────────── */

static void test_vin_no_di_byte(void) {
    /* Wire: 0x49 0x02 <17 ASCII bytes>, payload here is just the 17 bytes
     * (msg[0]/msg[1] already stripped by the caller in the real code). */
    const uint8_t payload[] = "1HGCM82633A004352"; /* 17 chars, no NUL counted */
    char vin[18] = {0};
    parse_vin_payload(payload, 17, vin);
    TEST_ASSERT_EQUAL_STRING("1HGCM82633A004352", vin);
}

static void test_vin_with_di_byte(void) {
    /* DI byte = 0x01 ("1 data item"), plen = 1*17+1 = 18 exactly. */
    uint8_t payload[18];
    payload[0] = 0x01;
    memcpy(&payload[1], "1HGCM82633A004352", 17);
    char vin[18] = {0};
    parse_vin_payload(payload, 18, vin);
    TEST_ASSERT_EQUAL_STRING("1HGCM82633A004352", vin);
}

static void test_vin_di_byte_not_mistaken_for_char(void) {
    /* A VIN payload where byte 0 happens to be in 0x01-0x05's range as an
     * ASCII char (impossible for real VINs — they're always alnum) would
     * be mis-detected as a DI byte only if the LENGTH also matches. Confirm
     * the length gate protects a 17-byte (no-DI) payload starting with a
     * low byte value from being wrongly treated as DI-prefixed. */
    uint8_t payload[17];
    payload[0] = 0x03;   /* NOT a DI byte here — plen is 17, not 3*17+1 */
    memcpy(&payload[1], "HGCM82633A0043520", 16);
    char vin[18] = {0};
    parse_vin_payload(payload, 17, vin);
    /* First byte 0x03 is non-printable and gets stripped by the
     * leading-nonprintable trim, same as real-world NUL-padded VINs.
     * `take` caps at 17 bytes total (byte 0 counts against that budget
     * since it isn't stripped as a DI byte), so only 16 real chars ever
     * make it into the buffer — the 17th source char is never copied. */
    TEST_ASSERT_EQUAL_STRING("HGCM82633A004352", vin);
}

static void test_vin_strips_leading_null_padding(void) {
    /* Some ECUs pre-pad with raw NUL bytes not recognized as a DI byte
     * (only 0x01-0x05 with a matching length counts as one — see
     * test_vin_di_byte_not_mistaken_for_char). Writing this test against
     * the ORIGINAL obd2.c surfaced two real bugs, both now fixed there:
     * (1) the leading-strip used a NUL-terminated `*start` pointer walk,
     * which treats byte 0x00 as end-of-string and never actually strips
     * it despite the code comment claiming it does; (2) the strip ran
     * AFTER the fixed 17-byte window was already taken, so even a
     * corrected strip would silently truncate the tail of the real VIN
     * by however many padding bytes preceded it. Both are fixed by
     * stripping leading non-printable padding first, then taking 17
     * bytes of whatever remains. */
    uint8_t payload[20] = {0};
    memcpy(&payload[3], "1HGCM82633A004352", 17);
    char vin[18] = {0};
    parse_vin_payload(payload, sizeof(payload), vin);
    TEST_ASSERT_EQUAL_STRING("1HGCM82633A004352", vin);
}

static void test_vin_short_payload_truncates_cleanly(void) {
    const uint8_t payload[] = "1HGCM8";
    char vin[18] = {0};
    parse_vin_payload(payload, 6, vin);
    TEST_ASSERT_EQUAL_STRING("1HGCM8", vin);
}

/* ── Tests: ECU name ─────────────────────────────────────────────────── */

static void test_ecuname_no_di_byte(void) {
    const uint8_t payload[] = "ECM-EngineControl";
    char name[24] = {0};
    parse_ecuname_payload(payload, (uint16_t)strlen((const char *)payload), name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("ECM-EngineControl", name);
}

static void test_ecuname_with_di_byte_no_length_gate(void) {
    /* Unlike VIN, the DI check for ECU name has NO length-match
     * requirement — byte 0 in 1-5 is always treated as a DI byte
     * regardless of the remaining length. */
    uint8_t payload[8];
    payload[0] = 0x01;
    memcpy(&payload[1], "TCM", 3);
    char name[24] = {0};
    parse_ecuname_payload(payload, 4, name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("TCM", name);
}

static void test_ecuname_trims_trailing_whitespace(void) {
    /* VIN does NOT do this trailing trim — this test exists specifically
     * to catch a "merge VIN and ECU-name into one decoder" refactor that
     * drops this difference. */
    uint8_t payload[] = { 'E', 'C', 'M', ' ', ' ', 0x00, 0x00 };
    char name[24] = {0};
    parse_ecuname_payload(payload, sizeof(payload), name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("ECM", name);
}

static void test_ecuname_strips_leading_and_trims_trailing(void) {
    uint8_t payload[] = { 0x00, 0x00, 'B', 'C', 'M', 0x20, 0x00 };
    char name[24] = {0};
    parse_ecuname_payload(payload, sizeof(payload), name, sizeof(name));
    TEST_ASSERT_EQUAL_STRING("BCM", name);
}

static void test_ecuname_respects_output_capacity(void) {
    /* Real buffer is 24 bytes (up to 20 chars + slack + NUL per obd2.c's
     * s_ecuname_req.name comment) — verify truncation doesn't overflow. */
    uint8_t payload[40];
    memset(payload, 'X', sizeof(payload));
    char name[8] = {0};
    parse_ecuname_payload(payload, sizeof(payload), name, sizeof(name));
    TEST_ASSERT_EQUAL_INT(7, (int)strlen(name));  /* cap-1 chars + NUL */
}

/* ── Tests: DTC pair decode ──────────────────────────────────────────── */

static void test_dtc_pair_powertrain_category(void) {
    /* P0420 — classic catalyst-efficiency code. Category bits 00 = 'P',
     * digit bits 00 = '0', then 0x420 in hex nibbles. */
    char code[6];
    decode_dtc_pair(0x04, 0x20, code);
    TEST_ASSERT_EQUAL_STRING("P0420", code);
}

static void test_dtc_pair_all_categories(void) {
    char code[6];
    decode_dtc_pair(0x00, 0x00, code);
    TEST_ASSERT_EQUAL_INT('P', code[0]);
    decode_dtc_pair(0x40, 0x00, code);
    TEST_ASSERT_EQUAL_INT('C', code[0]);
    decode_dtc_pair(0x80, 0x00, code);
    TEST_ASSERT_EQUAL_INT('B', code[0]);
    decode_dtc_pair(0xC0, 0x00, code);
    TEST_ASSERT_EQUAL_INT('U', code[0]);
}

static void test_dtc_pair_digit_encoding(void) {
    char code[6];
    decode_dtc_pair(0x00, 0x00, code);
    TEST_ASSERT_EQUAL_INT('0', code[1]);
    decode_dtc_pair(0x10, 0x00, code);
    TEST_ASSERT_EQUAL_INT('1', code[1]);
    decode_dtc_pair(0x20, 0x00, code);
    TEST_ASSERT_EQUAL_INT('2', code[1]);
    decode_dtc_pair(0x30, 0x00, code);
    TEST_ASSERT_EQUAL_INT('3', code[1]);
}

static void test_dtc_pair_hex_digits(void) {
    /* U0100 — a common "lost communication with ECM" network code. */
    char code[6];
    decode_dtc_pair(0xC1, 0x00, code);
    TEST_ASSERT_EQUAL_STRING("U0100", code);
}

/* ── Tests: DTC payload walk ─────────────────────────────────────────── */

static void test_dtc_payload_single_code(void) {
    uint8_t payload[] = { 0x04, 0x20 };
    obd2_dtc_t out[OBD2_MAX_DTCS];
    uint8_t count = decode_dtc_payload(payload, sizeof(payload), out);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_STRING("P0420", out[0].code);
}

static void test_dtc_payload_multiple_codes(void) {
    uint8_t payload[] = { 0x04, 0x20, 0xC1, 0x00, 0x01, 0x71 };
    obd2_dtc_t out[OBD2_MAX_DTCS];
    uint8_t count = decode_dtc_payload(payload, sizeof(payload), out);
    TEST_ASSERT_EQUAL_INT(3, count);
    TEST_ASSERT_EQUAL_STRING("P0420", out[0].code);
    TEST_ASSERT_EQUAL_STRING("U0100", out[1].code);
    TEST_ASSERT_EQUAL_STRING("P0171", out[2].code);
}

static void test_dtc_payload_stops_at_zero_pad(void) {
    /* Real ECUs pad unused slots with 0x00 0x00 — the walk must stop
     * there, not decode padding as a spurious "P0000". */
    uint8_t payload[] = { 0x04, 0x20, 0x00, 0x00, 0xC1, 0x00 };
    obd2_dtc_t out[OBD2_MAX_DTCS];
    uint8_t count = decode_dtc_payload(payload, sizeof(payload), out);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_STRING("P0420", out[0].code);
}

static void test_dtc_payload_empty(void) {
    obd2_dtc_t out[OBD2_MAX_DTCS];
    uint8_t count = decode_dtc_payload(NULL, 0, out);
    TEST_ASSERT_EQUAL_INT(0, count);
}

static void test_dtc_payload_respects_max_cap(void) {
    /* OBD2_MAX_DTCS codes' worth of non-padding pairs, plus one more that
     * must be silently dropped (real ECUs with more codes than the cap
     * need a follow-up Mode 03 with a starting offset — not implemented,
     * matches obd2.h's own documented limitation). */
    uint8_t payload[(OBD2_MAX_DTCS + 1) * 2];
    for (int i = 0; i <= OBD2_MAX_DTCS; i++) {
        payload[i * 2]     = 0x01;                 /* never 0x00 0x00 */
        payload[i * 2 + 1] = (uint8_t)(i + 1);
    }
    obd2_dtc_t out[OBD2_MAX_DTCS];
    uint8_t count = decode_dtc_payload(payload, sizeof(payload), out);
    TEST_ASSERT_EQUAL_INT(OBD2_MAX_DTCS, count);
}

/* ── Tests: the "maybe_count" heuristic ──────────────────────────────── */

static void test_dtc_response_with_explicit_count_byte(void) {
    /* Newer SAE J1979: byte 0 is an explicit count. count=2, so
     * count*2+1 == 5 == plen — heuristic must skip the count byte. */
    uint8_t payload[] = { 0x02, 0x04, 0x20, 0xC1, 0x00 };
    obd2_dtc_t out[OBD2_MAX_DTCS];
    uint8_t count = decode_dtc_response(payload, sizeof(payload), out);
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_STRING("P0420", out[0].code);
    TEST_ASSERT_EQUAL_STRING("U0100", out[1].code);
}

static void test_dtc_response_without_count_byte(void) {
    /* Older / some ECUs: no count byte, pairs start immediately.
     * plen=4, byte0=0x04 -> 0x04*2+1=9 != 4, so heuristic correctly does
     * NOT skip anything. */
    uint8_t payload[] = { 0x04, 0x20, 0xC1, 0x00 };
    obd2_dtc_t out[OBD2_MAX_DTCS];
    uint8_t count = decode_dtc_response(payload, sizeof(payload), out);
    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_STRING("P0420", out[0].code);
    TEST_ASSERT_EQUAL_STRING("U0100", out[1].code);
}

static void test_dtc_response_single_code_no_count_byte(void) {
    /* Edge case that motivates the heuristic in the first place: a SINGLE
     * DTC with no count byte has plen=2; a hypothetical count-byte read
     * of payload[0]=0x04 would compute 0x04*2+1=9 != 2, so it's correctly
     * NOT treated as a count byte even though it's a plausible-looking
     * value. */
    uint8_t payload[] = { 0x04, 0x20 };
    obd2_dtc_t out[OBD2_MAX_DTCS];
    uint8_t count = decode_dtc_response(payload, sizeof(payload), out);
    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_STRING("P0420", out[0].code);
}

int main(void) {
    UNITY_BEGIN();

    /* VIN */
    RUN_TEST(test_vin_no_di_byte);
    RUN_TEST(test_vin_with_di_byte);
    RUN_TEST(test_vin_di_byte_not_mistaken_for_char);
    RUN_TEST(test_vin_strips_leading_null_padding);
    RUN_TEST(test_vin_short_payload_truncates_cleanly);

    /* ECU name */
    RUN_TEST(test_ecuname_no_di_byte);
    RUN_TEST(test_ecuname_with_di_byte_no_length_gate);
    RUN_TEST(test_ecuname_trims_trailing_whitespace);
    RUN_TEST(test_ecuname_strips_leading_and_trims_trailing);
    RUN_TEST(test_ecuname_respects_output_capacity);

    /* DTC pair decode */
    RUN_TEST(test_dtc_pair_powertrain_category);
    RUN_TEST(test_dtc_pair_all_categories);
    RUN_TEST(test_dtc_pair_digit_encoding);
    RUN_TEST(test_dtc_pair_hex_digits);

    /* DTC payload walk */
    RUN_TEST(test_dtc_payload_single_code);
    RUN_TEST(test_dtc_payload_multiple_codes);
    RUN_TEST(test_dtc_payload_stops_at_zero_pad);
    RUN_TEST(test_dtc_payload_empty);
    RUN_TEST(test_dtc_payload_respects_max_cap);

    /* maybe_count heuristic */
    RUN_TEST(test_dtc_response_with_explicit_count_byte);
    RUN_TEST(test_dtc_response_without_count_byte);
    RUN_TEST(test_dtc_response_single_code_no_count_byte);

    return UNITY_END();
}
