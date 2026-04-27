/* test_can_decode.c — exercise can_extract_bits / can_pack_bits.
 *
 * `can_decode.c` is dependency-free (no LVGL, FreeRTOS, ESP-IDF), so we
 * include the source directly. No mocks needed.
 */
#include "unity.h"
#include "../../main/can/can_decode.c"

/* ── Reference frames ─────────────────────────────────────────────────────
 * A handful of named CAN frames we exercise repeatedly. Built once here
 * so individual tests stay focused on the semantics of the call. */
static const uint8_t FRAME_ZEROS[8]  = {0};
static const uint8_t FRAME_ONES[8]   = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint8_t FRAME_INCR[8]   = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08};
static const uint8_t FRAME_RPM[8]    = {0x10, 0x68, 0,0,0,0,0,0}; /* RPM=4200 little-endian */
static const uint8_t FRAME_NEG_TEMP[8] = {0xFE, 0,0,0,0,0,0,0};   /* -2 little-endian, 8 bits signed */

/* ── Endian × signedness × alignment matrix ───────────────────────────── */

static void test_extract_intel_byte_aligned_unsigned(void) {
	/* Intel (little-endian), 16 bits at offset 0 → 0x6810 = 26640 */
	int64_t v = can_extract_bits(FRAME_RPM, 0, 16, 1, false);
	TEST_ASSERT_EQUAL_INT(26640, v);
}

static void test_extract_intel_8bit_unsigned(void) {
	int64_t v = can_extract_bits(FRAME_INCR, 0, 8, 1, false);
	TEST_ASSERT_EQUAL_INT(0x01, v);
}

static void test_extract_intel_8bit_signed_negative(void) {
	/* 0xFE as signed 8-bit = -2 */
	int64_t v = can_extract_bits(FRAME_NEG_TEMP, 0, 8, 1, true);
	TEST_ASSERT_EQUAL_INT(-2, v);
}

static void test_extract_intel_8bit_signed_positive(void) {
	/* 0x7F as signed 8-bit = 127 */
	const uint8_t f[8] = {0x7F, 0,0,0,0,0,0,0};
	int64_t v = can_extract_bits(f, 0, 8, 1, true);
	TEST_ASSERT_EQUAL_INT(127, v);
}

static void test_extract_motorola_byte_aligned_unsigned(void) {
	/* Motorola (big-endian), 16 bits at offset 0 → 0x0102 = 258 */
	int64_t v = can_extract_bits(FRAME_INCR, 0, 16, 0, false);
	TEST_ASSERT_EQUAL_INT(0x0102, v);
}

static void test_extract_motorola_32bit_byte_aligned(void) {
	int64_t v = can_extract_bits(FRAME_INCR, 0, 32, 0, false);
	TEST_ASSERT_EQUAL_INT(0x01020304, v);
}

static void test_extract_intel_32bit_byte_aligned(void) {
	int64_t v = can_extract_bits(FRAME_INCR, 0, 32, 1, false);
	TEST_ASSERT_EQUAL_INT(0x04030201, v);
}

static void test_extract_64bit_full_frame(void) {
	int64_t v = can_extract_bits(FRAME_ONES, 0, 64, 1, false);
	TEST_ASSERT_EQUAL_HEX(0xFFFFFFFFFFFFFFFFULL, (uint64_t)v);
}

static void test_extract_zero_frame(void) {
	int64_t v = can_extract_bits(FRAME_ZEROS, 0, 32, 1, false);
	TEST_ASSERT_EQUAL_INT(0, v);
}

/* ── Sub-byte extraction ─────────────────────────────────────────────── */

static void test_extract_intel_4bit_low_nibble(void) {
	/* byte 0 = 0x10, low 4 bits at offset 0 = 0 */
	int64_t v = can_extract_bits(FRAME_RPM, 0, 4, 1, false);
	TEST_ASSERT_EQUAL_INT(0, v);
}

static void test_extract_intel_4bit_high_nibble(void) {
	/* byte 0 = 0x10, high 4 bits at offset 4 = 1 */
	int64_t v = can_extract_bits(FRAME_RPM, 4, 4, 1, false);
	TEST_ASSERT_EQUAL_INT(1, v);
}

static void test_extract_motorola_3bit_unaligned(void) {
	/* byte 0 = 0x01 = 0b00000001. Motorola, offset 5, length 3 = bits [5..7] = 0b001 = 1. */
	int64_t v = can_extract_bits(FRAME_INCR, 5, 3, 0, false);
	TEST_ASSERT_EQUAL_INT(1, v);
}

/* ── Signed extraction at non-byte boundaries ──────────────────────── */

static void test_extract_intel_signed_12bit(void) {
	/* 12-bit signed value 0xFFE = -2 (sign bit at bit 11 set, plus low bits) */
	const uint8_t f[8] = {0xFE, 0xFF, 0,0,0,0,0,0};
	int64_t v = can_extract_bits(f, 0, 12, 1, true);
	TEST_ASSERT_EQUAL_INT(-2, v);
}

static void test_extract_intel_unsigned_same_pattern(void) {
	/* Same bytes, unsigned: 0xFFE = 4094 */
	const uint8_t f[8] = {0xFE, 0xFF, 0,0,0,0,0,0};
	int64_t v = can_extract_bits(f, 0, 12, 1, false);
	TEST_ASSERT_EQUAL_INT(4094, v);
}

/* ── Bounds and edge cases ────────────────────────────────────────────── */

static void test_extract_zero_length_returns_zero(void) {
	/* Documented contract: length 0 → 0 */
	int64_t v = can_extract_bits(FRAME_ONES, 0, 0, 1, false);
	TEST_ASSERT_EQUAL_INT(0, v);
}

static void test_extract_overflow_length_returns_zero(void) {
	int64_t v = can_extract_bits(FRAME_ONES, 0, 65, 1, false);
	TEST_ASSERT_EQUAL_INT(0, v);
}

static void test_extract_offset_plus_length_overflow(void) {
	/* offset 60 + length 8 = 68 bits → past the 64-bit frame → returns 0 */
	int64_t v = can_extract_bits(FRAME_ONES, 60, 8, 1, false);
	TEST_ASSERT_EQUAL_INT(0, v);
}

/* ── pack → extract round-trip ───────────────────────────────────────── */

static void test_pack_extract_intel_roundtrip(void) {
	uint8_t buf[8] = {0};
	can_pack_bits(buf, 8, 16, 0xABCD, 1);
	int64_t v = can_extract_bits(buf, 8, 16, 1, false);
	TEST_ASSERT_EQUAL_HEX(0xABCD, v);
}

static void test_pack_extract_motorola_roundtrip(void) {
	uint8_t buf[8] = {0};
	can_pack_bits(buf, 0, 16, 0x1234, 0);
	int64_t v = can_extract_bits(buf, 0, 16, 0, false);
	TEST_ASSERT_EQUAL_HEX(0x1234, v);
}

static void test_pack_intel_at_offset(void) {
	/* Pack 0xFF at offset 8, length 8 — should land in byte[1] */
	uint8_t buf[8] = {0};
	can_pack_bits(buf, 8, 8, 0xFF, 1);
	TEST_ASSERT_EQUAL_HEX(0x00, buf[0]);
	TEST_ASSERT_EQUAL_HEX(0xFF, buf[1]);
	TEST_ASSERT_EQUAL_HEX(0x00, buf[2]);
}

/* ── Runner ──────────────────────────────────────────────────────────── */

int main(void) {
	UNITY_BEGIN();

	RUN_TEST(test_extract_intel_byte_aligned_unsigned);
	RUN_TEST(test_extract_intel_8bit_unsigned);
	RUN_TEST(test_extract_intel_8bit_signed_negative);
	RUN_TEST(test_extract_intel_8bit_signed_positive);
	RUN_TEST(test_extract_motorola_byte_aligned_unsigned);
	RUN_TEST(test_extract_motorola_32bit_byte_aligned);
	RUN_TEST(test_extract_intel_32bit_byte_aligned);
	RUN_TEST(test_extract_64bit_full_frame);
	RUN_TEST(test_extract_zero_frame);

	RUN_TEST(test_extract_intel_4bit_low_nibble);
	RUN_TEST(test_extract_intel_4bit_high_nibble);
	RUN_TEST(test_extract_motorola_3bit_unaligned);

	RUN_TEST(test_extract_intel_signed_12bit);
	RUN_TEST(test_extract_intel_unsigned_same_pattern);

	RUN_TEST(test_extract_zero_length_returns_zero);
	RUN_TEST(test_extract_overflow_length_returns_zero);
	RUN_TEST(test_extract_offset_plus_length_overflow);

	RUN_TEST(test_pack_extract_intel_roundtrip);
	RUN_TEST(test_pack_extract_motorola_roundtrip);
	RUN_TEST(test_pack_intel_at_offset);

	return UNITY_END();
}
