/* test_hardening_fixes.c — regression coverage for the 2026-07 pre-launch
 * hardening sweep.
 *
 * The defects these lock in live in obd2.c / signal.c / can_manager.c /
 * web_server_capture.c, which all #include the full ESP-IDF + LVGL surface and
 * so can't be compiled on-host without the large mock investment the other
 * OBD2 test file documents. Following that file's convention, each test here
 * MIRRORS the exact decision logic of the firmware fix (a small self-contained
 * helper), so a future edit that regresses the logic fails here. Keep the
 * helpers in lock-step with their cited source-of-truth.
 */
#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

/* ── #16: OBD2 ISO-TP consecutive-frame DLC clamp ─────────────────────────
 * Source: main/can/obd2.c obd2_rx_handler ("if (dlc > 8) dlc = 8;") + the CF
 * branch ("avail = (dlc > 1) ? dlc - 1 : 0; take = min(remaining, avail)").
 * The TWAI driver hands up the raw 4-bit DLC (0-15) but only fills 8 data
 * bytes, so without the clamp the CF memcpy reads up to data[14] — 7 bytes
 * past the 8-byte frame. This mirror proves the clamp caps the source read at
 * data[7] for every possible raw DLC. */
static uint8_t cf_source_bytes(uint8_t raw_dlc, uint16_t remaining) {
	uint8_t dlc = raw_dlc;
	if (dlc > 8) dlc = 8;                              /* the fix */
	uint8_t avail = (dlc > 1) ? (uint8_t)(dlc - 1) : 0;
	uint8_t take  = (remaining < avail) ? (uint8_t)remaining : avail;
	return take;                                       /* bytes read from &data[1] */
}

static void test_dlc_clamp_bounds_all_raw_dlcs(void) {
	/* For every raw DLC 0..15 with plenty of remaining buffer, the CF read
	 * must never exceed 7 source bytes (data[1..7]). */
	for (uint8_t raw = 0; raw <= 15; raw++) {
		uint8_t take = cf_source_bytes(raw, 4096);
		TEST_ASSERT_TRUE(take <= 7);   /* CF read must stay within data[1..7] */
	}
}

static void test_dlc_clamp_unclamped_would_overflow(void) {
	/* Guard against a regression that drops the clamp: a raw DLC of 15 with
	 * the clamp yields 7; without it would be 14 (the OOB read). */
	TEST_ASSERT_EQUAL_INT(7, cf_source_bytes(15, 4096));
	TEST_ASSERT_EQUAL_INT(7, cf_source_bytes(9,  4096));   /* 9-1 clamped to 7 */
	TEST_ASSERT_EQUAL_INT(7, cf_source_bytes(8,  4096));   /* normal max */
	TEST_ASSERT_EQUAL_INT(3, cf_source_bytes(4,  4096));   /* normal */
}

static void test_dlc_clamp_respects_remaining(void) {
	/* take is still bounded by remaining buffer space. */
	TEST_ASSERT_EQUAL_INT(2, cf_source_bytes(8, 2));
	TEST_ASSERT_EQUAL_INT(0, cf_source_bytes(8, 0));
}

/* ── #17: signal dispatch skips can_id==0 / bit_length==0 signals ─────────
 * Source: main/widgets/signal.c signal_dispatch_frame
 * ("if (sig->can_id == 0 || sig->bit_length == 0) continue;").
 * OBD2/internal signals register with can_id=0 (the "no CAN binding"
 * sentinel); a real 0x000 frame must NOT match and force-update them. */
static bool signal_should_dispatch(uint32_t sig_can_id, uint8_t sig_bit_length,
                                   uint32_t frame_can_id) {
	if (sig_can_id == 0 || sig_bit_length == 0) return false;   /* the fix */
	return sig_can_id == frame_can_id;
}

static void test_dispatch_skips_internal_signal_on_zero_frame(void) {
	/* Internal/OBD2 signal (can_id=0, bit_length=0) must be skipped for a
	 * 0x000 frame — the bug it prevents. */
	TEST_ASSERT_FALSE(signal_should_dispatch(0, 0, 0x000));
}

static void test_dispatch_skips_zero_binding_for_any_frame(void) {
	TEST_ASSERT_FALSE(signal_should_dispatch(0, 0, 0x201));
	TEST_ASSERT_FALSE(signal_should_dispatch(0, 8, 0x000));  /* can_id 0 sentinel */
}

static void test_dispatch_matches_real_can_signal(void) {
	/* A genuine CAN signal still matches its own frame and no other. */
	TEST_ASSERT_TRUE(signal_should_dispatch(0x201, 16, 0x201));
	TEST_ASSERT_FALSE(signal_should_dispatch(0x201, 16, 0x202));
}

/* ── #15: TWAI filter falls back to accept-all for extended (29-bit) IDs ───
 * Source: main/can/can_manager.c build_twai_filter_from_signals
 * ("if (sig->can_id > 0x7FFu) { accept-all; return; }"). The standard-frame
 * acceptance filter can't encode a 29-bit ID, and masking it to 11 bits would
 * filter the real frames out entirely, so any extended ID forces accept-all. */
static bool filter_is_accept_all(const uint32_t *ids, int n) {
	for (int i = 0; i < n; i++) {
		if (ids[i] == 0) continue;                 /* unbound signal */
		if (ids[i] > 0x7FFu) return true;          /* the fix -> accept-all */
	}
	return false;   /* all standard IDs -> a narrowed filter is built */
}

static void test_filter_accept_all_when_extended_id_present(void) {
	uint32_t ids[] = { 0x201, 0x7DF, 0x18FEF100u /* 29-bit */ };
	TEST_ASSERT_TRUE(filter_is_accept_all(ids, 3));
}

static void test_filter_narrows_for_all_standard_ids(void) {
	uint32_t ids[] = { 0x201, 0x7DF, 0x7E8 };
	TEST_ASSERT_FALSE(filter_is_accept_all(ids, 3));
}

static void test_filter_boundary_11bit_max_is_standard(void) {
	uint32_t ids[] = { 0x7FF };                    /* max standard ID */
	TEST_ASSERT_FALSE(filter_is_accept_all(ids, 1));
	uint32_t ext[] = { 0x800 };                    /* first extended value */
	TEST_ASSERT_TRUE(filter_is_accept_all(ext, 1));
}

/* ── #0: /api/screenshot/hash overflow-safe width/height clamp ────────────
 * Source: main/net/web_server_capture.c
 * ("if (w <= 0 || w > SCREEN_W - x) w = SCREEN_W - x;"). The old test was
 * "x + w > SCREEN_W", which overflows to negative for a large attacker w and
 * sails past the clamp, letting the FNV loop read far off the framebuffer.
 * x is pre-clamped to [0, SCREEN_W-1], so SCREEN_W - x never underflows. */
#define TEST_SCREEN_W 800
#define TEST_SCREEN_H 480
static int clamp_w(int x, int w) {
	if (w <= 0 || w > TEST_SCREEN_W - x) w = TEST_SCREEN_W - x;   /* the fix */
	return w;
}

static void test_screenshot_clamp_rejects_intmax_width(void) {
	/* The exploit input: x=1, w=INT_MAX must clamp to the remaining width. */
	int w = clamp_w(1, INT_MAX);
	TEST_ASSERT_EQUAL_INT(TEST_SCREEN_W - 1, w);
	TEST_ASSERT_TRUE(1 + w <= TEST_SCREEN_W);   /* no OOB, computed without overflow */
}

static void test_screenshot_clamp_passes_valid_width(void) {
	TEST_ASSERT_EQUAL_INT(500, clamp_w(100, 500));   /* 100+500=600 <= 800 */
	TEST_ASSERT_EQUAL_INT(TEST_SCREEN_W, clamp_w(0, TEST_SCREEN_W));
}

static void test_screenshot_clamp_never_exceeds_bounds(void) {
	/* For a spread of x and hostile w, x + clamped_w never exceeds SCREEN_W. */
	int xs[] = { 0, 1, 400, 799 };
	int ws[] = { -5, 0, 1, 800, 100000, INT_MAX };
	for (unsigned i = 0; i < sizeof(xs)/sizeof(xs[0]); i++) {
		for (unsigned j = 0; j < sizeof(ws)/sizeof(ws[0]); j++) {
			int w = clamp_w(xs[i], ws[j]);
			TEST_ASSERT_TRUE(w >= 0);
			TEST_ASSERT_TRUE(xs[i] + w <= TEST_SCREEN_W);
		}
	}
}

/* ── #23: strict 1-byte Mode 04 clear-ack is accepted ─────────────────────
 * Source: main/can/obd2.c obd2_rx_handler SF guard ("len_bytes < 1") plus
 * _process_full_message ("len==1 && msg[0]==0x44 && s_clear_req.active"). Some
 * ECUs answer a DTC clear with just the bare 0x44 echo (a 1-byte SF); the old
 * "len_bytes < 2" guard dropped it, reporting a successful clear as a failure. */
static bool sf_guard_accepts(uint8_t len_bytes) {
	return !(len_bytes < 1 || len_bytes > 7);      /* the relaxed guard */
}
static bool clear_ack_accepted(uint16_t len, uint8_t b0, bool clear_active) {
	return len == 1 && b0 == 0x44 && clear_active;  /* the 1-byte handler */
}

static void test_sf_guard_accepts_one_byte(void) {
	TEST_ASSERT_TRUE(sf_guard_accepts(1));   /* the fix — was rejected before */
	TEST_ASSERT_TRUE(sf_guard_accepts(7));
	TEST_ASSERT_FALSE(sf_guard_accepts(0));  /* empty SF still invalid */
	TEST_ASSERT_FALSE(sf_guard_accepts(8));  /* > 7 invalid */
}

static void test_clear_ack_one_byte_accepted_when_pending(void) {
	TEST_ASSERT_TRUE(clear_ack_accepted(1, 0x44, true));
}

static void test_clear_ack_one_byte_ignored_when_not_pending(void) {
	/* A stray 1-byte 0x44 with no clear request in flight is not consumed. */
	TEST_ASSERT_FALSE(clear_ack_accepted(1, 0x44, false));
	/* And a non-0x44 single byte never matches. */
	TEST_ASSERT_FALSE(clear_ack_accepted(1, 0x41, true));
}

int main(void) {
	UNITY_BEGIN();

	/* #16 OBD2 DLC clamp */
	RUN_TEST(test_dlc_clamp_bounds_all_raw_dlcs);
	RUN_TEST(test_dlc_clamp_unclamped_would_overflow);
	RUN_TEST(test_dlc_clamp_respects_remaining);

	/* #17 signal dispatch can_id==0 guard */
	RUN_TEST(test_dispatch_skips_internal_signal_on_zero_frame);
	RUN_TEST(test_dispatch_skips_zero_binding_for_any_frame);
	RUN_TEST(test_dispatch_matches_real_can_signal);

	/* #15 extended-ID filter fallback */
	RUN_TEST(test_filter_accept_all_when_extended_id_present);
	RUN_TEST(test_filter_narrows_for_all_standard_ids);
	RUN_TEST(test_filter_boundary_11bit_max_is_standard);

	/* #0 screenshot hash overflow-safe clamp */
	RUN_TEST(test_screenshot_clamp_rejects_intmax_width);
	RUN_TEST(test_screenshot_clamp_passes_valid_width);
	RUN_TEST(test_screenshot_clamp_never_exceeds_bounds);

	/* #23 1-byte Mode 04 clear-ack */
	RUN_TEST(test_sf_guard_accepts_one_byte);
	RUN_TEST(test_clear_ack_one_byte_accepted_when_pending);
	RUN_TEST(test_clear_ack_one_byte_ignored_when_not_pending);

	return UNITY_END();
}
