/* test_can_mux_focus.c — the CAN id tracker's per-mux-value focus buffer.
 *
 * ── Why this exists ─────────────────────────────────────────────────────
 *
 * The main tracker table keeps ONE data[8] per CAN id: the most recent frame,
 * whatever its mux value. That is fine for a bus overview and actively
 * misleading when you are building a multiplexed channel — on a stream
 * cycling 14 payloads through one id, a poll catches a random one, so the
 * editor's live decode preview would show a different quantity every time it
 * refreshed. Someone tuning bit_start against that preview is being shown
 * confident garbage, which is the same failure the decode gate exists to
 * prevent, just moved into the UI.
 *
 * The focus buffer keeps the latest frame for EACH mux value on one id. This
 * links the REAL can_id_tracker.c (with the mocks/ esp_attr + esp_timer
 * shims) so the shipped bucketing is what gets asserted, not a copy of it.
 *
 * The invariants that matter:
 *   - frames land in the bucket named by their own mux field, read exactly
 *     the way signal_dispatch_frame() reads it (signal's endian, unsigned);
 *   - re-aiming the buffer DISCARDS what it held, because those payloads
 *     describe the old geometry;
 *   - re-requesting the SAME geometry does not discard, or a UI polling once
 *     a second would wipe the buckets it is about to render;
 *   - a frame too short to contain the mux field is dropped, not bucketed
 *     from out-of-bounds bytes.
 */
#include "unity.h"
#include "../../main/can/can_id_tracker.c"

#include <string.h>
#include <stdio.h>

/* The tracker's statics are file-scope and this TU includes the .c directly,
 * so reach in and reset between cases rather than exposing a test-only API. */
static void reset_all(void) {
	can_id_tracker_reset();
	can_id_tracker_clear_focus();
	/* reset()/clear_focus() only publish a request; the recording task adopts
	 * it on the next frame. Drive one through an id nobody looks at. */
	uint8_t junk[8] = {0};
	can_id_tracker_record(0x7FF, false, junk, 8);
	can_id_tracker_reset();
}

/* One Link-style Generic Dash frame: byte 0 is the frame index, the three
 * 16-bit Intel words after it are that frame's payload. */
static void send_link(uint8_t frame_idx, uint16_t w1, uint16_t w2, uint16_t w3) {
	uint8_t d[8];
	d[0] = frame_idx;
	d[1] = 0;
	d[2] = (uint8_t)(w1 & 0xFF); d[3] = (uint8_t)(w1 >> 8);
	d[4] = (uint8_t)(w2 & 0xFF); d[5] = (uint8_t)(w2 >> 8);
	d[6] = (uint8_t)(w3 & 0xFF); d[7] = (uint8_t)(w3 >> 8);
	can_id_tracker_record(0x3E8, false, d, 8);
}

static const can_mux_slot_t *slot_for(uint16_t mux) {
	for (uint8_t i = 0; i < can_id_tracker_focus_count(); i++) {
		const can_mux_slot_t *s = can_id_tracker_focus_get(i);
		if (s && s->mux_value == mux) return s;
	}
	return NULL;
}

/* ── The core promise: one bucket per frame index, each holding ITS payload,
 * not whichever frame happened to arrive last. ───────────────────────────── */
static void test_buckets_by_frame_index(void) {
	reset_all();
	can_id_tracker_set_focus(0x3E8, false, 0, 8, 1);

	send_link(0, 6500, 101, 0);      /* RPM 6500, MAP 101 */
	send_link(2, 0, 0, 95);          /* coolant 95 */
	send_link(8, 110, 420, 0);       /* oil temp 110, oil press 420 */
	send_link(0, 6600, 102, 0);      /* frame 0 again — must REPLACE, not add */

	TEST_ASSERT_EQUAL_UINT(3, can_id_tracker_focus_count());

	const can_mux_slot_t *f0 = slot_for(0);
	TEST_ASSERT_NOT_NULL(f0);
	/* The newer frame 0 won, and it did not disturb the others. */
	TEST_ASSERT_EQUAL_UINT(6600, (uint16_t)(f0->data[2] | (f0->data[3] << 8)));
	TEST_ASSERT_EQUAL_UINT(2, f0->rx_count);

	const can_mux_slot_t *f2 = slot_for(2);
	TEST_ASSERT_NOT_NULL(f2);
	TEST_ASSERT_EQUAL_UINT(95, (uint16_t)(f2->data[6] | (f2->data[7] << 8)));

	const can_mux_slot_t *f8 = slot_for(8);
	TEST_ASSERT_NOT_NULL(f8);
	TEST_ASSERT_EQUAL_UINT(110, (uint16_t)(f8->data[2] | (f8->data[3] << 8)));
	TEST_ASSERT_EQUAL_UINT(420, (uint16_t)(f8->data[4] | (f8->data[5] << 8)));

	/* Without the focus buffer this is all you would have had: the last frame
	 * on the id, which here is a frame 0 — so a preview built on the main
	 * table would decode frame 0's bytes no matter which index you asked for. */
	TEST_ASSERT_EQUAL_UINT(0x3E8, can_id_tracker_focus_id());
}

/* An unfocused id must not be bucketed — the buffer is for ONE id. */
static void test_other_ids_ignored(void) {
	reset_all();
	can_id_tracker_set_focus(0x3E8, false, 0, 8, 1);

	uint8_t other[8] = {3, 0, 0xAA, 0xBB, 0, 0, 0, 0};
	can_id_tracker_record(0x5F0, false, other, 8);
	TEST_ASSERT_EQUAL_UINT(0, can_id_tracker_focus_count());

	send_link(3, 1, 2, 3);
	TEST_ASSERT_EQUAL_UINT(1, can_id_tracker_focus_count());
}

/* Re-aiming discards: the collected payloads describe the OLD geometry, and
 * showing them under the new one would be a preview that lies. */
static void test_retarget_discards(void) {
	reset_all();
	can_id_tracker_set_focus(0x3E8, false, 0, 8, 1);
	send_link(0, 1, 2, 3);
	send_link(1, 4, 5, 6);
	TEST_ASSERT_EQUAL_UINT(2, can_id_tracker_focus_count());

	/* Different mux width — same id, but every bucket label is now wrong. */
	can_id_tracker_set_focus(0x3E8, false, 0, 4, 1);
	send_link(0, 7, 8, 9);
	TEST_ASSERT_EQUAL_UINT(1, can_id_tracker_focus_count());

	/* Different id entirely. */
	can_id_tracker_set_focus(0x5F0, false, 0, 8, 1);
	send_link(0, 1, 2, 3);                   /* 0x3E8 — not focused any more */
	TEST_ASSERT_EQUAL_UINT(0, can_id_tracker_focus_count());
}

/* ...but asking for the SAME thing is a no-op. A UI re-aims on every poll;
 * if that discarded, the buckets would be empty every time it rendered. */
static void test_same_request_is_idempotent(void) {
	reset_all();
	can_id_tracker_set_focus(0x3E8, false, 0, 8, 1);
	send_link(0, 1, 2, 3);
	send_link(4, 4, 5, 6);
	TEST_ASSERT_EQUAL_UINT(2, can_id_tracker_focus_count());

	for (int i = 0; i < 20; i++)
		can_id_tracker_set_focus(0x3E8, false, 0, 8, 1);
	send_link(0, 9, 9, 9);

	TEST_ASSERT_EQUAL_UINT(2, can_id_tracker_focus_count());
	TEST_ASSERT_NOT_NULL(slot_for(4));      /* survived the re-requests */
}

/* A frame that cannot contain the mux field is dropped. Bucketing it would
 * mean labelling it from bytes the frame never carried. */
static void test_short_frame_dropped(void) {
	reset_all();
	/* Mux field at bits 32-39 = byte 4, so a 4-byte frame cannot hold it. */
	can_id_tracker_set_focus(0x200, false, 32, 8, 1);

	uint8_t shortf[4] = {1, 2, 3, 4};
	can_id_tracker_record(0x200, false, shortf, 4);
	TEST_ASSERT_EQUAL_UINT(0, can_id_tracker_focus_count());

	uint8_t full[8] = {0, 0, 0, 0, 7, 0, 0, 0};
	can_id_tracker_record(0x200, false, full, 8);
	TEST_ASSERT_EQUAL_UINT(1, can_id_tracker_focus_count());
	TEST_ASSERT_NOT_NULL(slot_for(7));
}

/* mux_bit_length 0 focuses an id WITHOUT splitting it: one bucket, value 0.
 * This is the "watch this plain id closely" case. */
static void test_unmultiplexed_focus(void) {
	reset_all();
	can_id_tracker_set_focus(0x5F0, false, 0, 0, 1);

	uint8_t a[8] = {1, 1, 1, 1, 1, 1, 1, 1};
	uint8_t b[8] = {2, 2, 2, 2, 2, 2, 2, 2};
	can_id_tracker_record(0x5F0, false, a, 8);
	can_id_tracker_record(0x5F0, false, b, 8);

	TEST_ASSERT_EQUAL_UINT(1, can_id_tracker_focus_count());
	const can_mux_slot_t *s = slot_for(0);
	TEST_ASSERT_NOT_NULL(s);
	TEST_ASSERT_EQUAL_UINT(2, s->data[0]);      /* newest wins */
	TEST_ASSERT_EQUAL_UINT(2, s->rx_count);
}

/* More distinct indices than slots: keep what we have rather than thrashing.
 * A rotating buffer would make the preview flicker for no gain. */
static void test_slot_overflow_keeps_first(void) {
	reset_all();
	can_id_tracker_set_focus(0x3E8, false, 0, 8, 1);

	for (int i = 0; i < CAN_ID_TRACKER_MAX_MUX_SLOTS + 8; i++)
		send_link((uint8_t)i, (uint16_t)(100 + i), 0, 0);

	TEST_ASSERT_EQUAL_UINT(CAN_ID_TRACKER_MAX_MUX_SLOTS,
	                        can_id_tracker_focus_count());
	/* The early ones are still there and still correct. */
	const can_mux_slot_t *s0 = slot_for(0);
	TEST_ASSERT_NOT_NULL(s0);
	TEST_ASSERT_EQUAL_UINT(100, (uint16_t)(s0->data[2] | (s0->data[3] << 8)));
	TEST_ASSERT_NULL(slot_for(CAN_ID_TRACKER_MAX_MUX_SLOTS + 2));
}

/* Geometry the decoder would refuse is refused here too, and refusing must
 * not silently retarget the buffer to something else. */
static void test_bad_geometry_rejected(void) {
	reset_all();
	can_id_tracker_set_focus(0x3E8, false, 0, 8, 1);
	send_link(5, 1, 2, 3);
	TEST_ASSERT_EQUAL_UINT(1, can_id_tracker_focus_count());

	can_id_tracker_set_focus(0x3E8, false, 60, 8, 1);   /* 60+8 > 64 */
	can_id_tracker_set_focus(0x3E8, false, 0, 17, 1);   /* > 16 bits */
	send_link(5, 4, 5, 6);

	/* Still the original, valid focus — the rejects changed nothing. */
	TEST_ASSERT_EQUAL_UINT(0x3E8, can_id_tracker_focus_id());
	TEST_ASSERT_EQUAL_UINT(1, can_id_tracker_focus_count());
	const can_mux_slot_t *s = slot_for(5);
	TEST_ASSERT_NOT_NULL(s);
	TEST_ASSERT_EQUAL_UINT(2, s->rx_count);
}

/* The mux field is read with the SIGNAL's endianness. A big-endian stream
 * whose index spans a byte boundary must bucket the same way the decoder
 * gates, or preview and dash disagree. */
static void test_big_endian_mux(void) {
	reset_all();
	/* 4-bit index at bits 4-7 of byte 0, Motorola. */
	can_id_tracker_set_focus(0x300, false, 4, 4, 0);

	uint8_t d[8] = {0x0A, 0, 0, 0, 0, 0, 0, 0};   /* low nibble = 0xA */
	can_id_tracker_record(0x300, false, d, 8);

	TEST_ASSERT_EQUAL_UINT(1, can_id_tracker_focus_count());
	/* can_extract_bits big-endian, offset 4 len 4 → the LOW nibble. */
	TEST_ASSERT_NOT_NULL(slot_for(0x0A));
}

/* clear_focus stops bucketing and reports no focused id. */
static void test_clear_focus(void) {
	reset_all();
	can_id_tracker_set_focus(0x3E8, false, 0, 8, 1);
	send_link(1, 1, 2, 3);
	TEST_ASSERT_EQUAL_UINT(0x3E8, can_id_tracker_focus_id());

	can_id_tracker_clear_focus();
	send_link(2, 1, 2, 3);        /* adopts the cleared request */
	TEST_ASSERT_EQUAL_UINT(0, can_id_tracker_focus_id());
	TEST_ASSERT_EQUAL_UINT(0, can_id_tracker_focus_count());
	TEST_ASSERT_NULL(can_id_tracker_focus_get(0));
}

/* The main table must keep behaving exactly as before — the focus buffer is
 * additive, and mux_seen still tracks byte-0 values for auto-detect. */
static void test_main_table_untouched(void) {
	reset_all();
	can_id_tracker_set_focus(0x3E8, false, 0, 8, 1);
	send_link(0, 1, 2, 3);
	send_link(2, 4, 5, 6);
	send_link(8, 7, 8, 9);

	TEST_ASSERT_EQUAL_UINT(1, can_id_tracker_count());
	const can_id_entry_t *e = can_id_tracker_get(0);
	TEST_ASSERT_NOT_NULL(e);
	TEST_ASSERT_EQUAL_UINT(0x3E8, e->can_id);
	TEST_ASSERT_EQUAL_UINT(3, e->rx_count);
	/* Last frame wins in the main table — the behaviour the focus buffer
	 * exists to work around. */
	TEST_ASSERT_EQUAL_UINT(8, e->data[0]);
	/* Indices 0, 2 and 8 seen. */
	TEST_ASSERT_EQUAL_UINT((1 << 0) | (1 << 2) | (1 << 8), e->mux_seen);
}

int main(void) {
	UNITY_BEGIN();
	RUN_TEST(test_buckets_by_frame_index);
	RUN_TEST(test_other_ids_ignored);
	RUN_TEST(test_retarget_discards);
	RUN_TEST(test_same_request_is_idempotent);
	RUN_TEST(test_short_frame_dropped);
	RUN_TEST(test_unmultiplexed_focus);
	RUN_TEST(test_slot_overflow_keeps_first);
	RUN_TEST(test_bad_geometry_rejected);
	RUN_TEST(test_big_endian_mux);
	RUN_TEST(test_clear_focus);
	RUN_TEST(test_main_table_untouched);
	return UNITY_END();
}
