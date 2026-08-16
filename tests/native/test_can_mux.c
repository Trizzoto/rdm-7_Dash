/* test_can_mux.c — multiplexed CAN frames, and the Link catalogue shape.
 *
 * ── Why this exists ─────────────────────────────────────────────────────
 *
 * Link's Generic Dash runs its ENTIRE stream through one CAN id (0x3E8) and
 * puts a frame index in byte 0; the three 16-bit words at bytes 2-3 / 4-5 /
 * 6-7 mean something different for each index. The catalogue used to model
 * that as fourteen consecutive ids (0x3E8 + frame), which meant:
 *
 *   - auto-detect could never match more than one of the preset's ids, so a
 *     Link was undetectable no matter how much of its data was on the bus;
 *   - applying the preset by hand left 11 channels bound to ids the ECU
 *     never sends, while RPM/MAP/MGP decoded from EVERY frame index and
 *     produced confident garbage rather than an obvious failure.
 *
 * Three halves here. The first exercises the decode gate itself against the
 * real can_extract_bits(). The second links the REAL catalogue table and
 * asserts the shape, including a guard against the whole bug class: a run of
 * consecutive ids where no row ever reads byte 0-1 is the fingerprint of a
 * multiplexed stream that has been mis-modelled as separate ids.
 *
 * The third covers the drain coalescer, which gets a multiplexed id wrong in
 * its own way: gating the decode correctly is worthless if the batching step
 * upstream threw 13 of every 14 frames away before the gate ever saw them.
 * That one starved every Link channel to ~2 Hz and read as a slow ECU.
 */
#include "unity.h"
#include "../../main/can/can_decode.c"
#include "preset_picker.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ── Half 1: the decode gate ─────────────────────────────────────────────
 *
 * Mirror of the gate in signal_dispatch_frame() (main/widgets/signal.c).
 * The firmware version can't be linked here — it drags in the PSRAM-backed
 * registry, LVGL and esp_timer — so the gate is reproduced verbatim over the
 * real can_extract_bits(). Keep the two in lockstep.
 */
typedef struct {
	uint8_t  bit_start, bit_length, endian;
	bool     is_signed;
	float    scale, offset;
	uint8_t  mux_bit_start, mux_bit_length;
	uint16_t mux_value;
} sig_t;

/* Returns true and writes *out when the frame feeds this signal. */
static bool decode_if_matches(const sig_t *s, const uint8_t *data, uint8_t dlc,
                              float *out) {
	if (s->mux_bit_length) {
		uint8_t mux_end = (uint8_t)((s->mux_bit_start + s->mux_bit_length - 1) / 8);
		if (dlc <= mux_end) return false;
		int64_t mux = can_extract_bits(data, s->mux_bit_start,
		                               s->mux_bit_length, s->endian, false);
		if ((uint16_t)mux != s->mux_value) return false;
	}
	uint8_t end_byte = (uint8_t)((s->bit_start + s->bit_length - 1) / 8);
	if (dlc <= end_byte) return false;
	int64_t raw = can_extract_bits(data, s->bit_start, s->bit_length,
	                               s->endian, s->is_signed);
	*out = (float)raw * s->scale + s->offset;
	return true;
}

/* The real Link rows: RPM is frame 0 bits 16-31, coolant is frame 2 bits
 * 48-63 with a -50 offset. Both on id 0x3E8, both Intel. */
static const sig_t LINK_RPM = { 16, 16, 1, false, 1.0f,   0.0f, 0, 8, 0 };
static const sig_t LINK_ECT = { 48, 16, 1, false, 1.0f, -50.0f, 0, 8, 2 };

/* Frame 0: index 0, RPM = 4200 (0x1068 little-endian at bytes 2-3). */
static const uint8_t FRAME0[8] = { 0x00, 0x00, 0x68, 0x10, 0x00, 0x00, 0x00, 0x00 };
/* Frame 2: index 2, coolant raw 140 at bytes 6-7 → 140 - 50 = 90 degC. */
static const uint8_t FRAME2[8] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8C, 0x00 };

static void test_gate_accepts_its_own_frame(void) {
	float v = -1.0f;
	TEST_ASSERT_TRUE(decode_if_matches(&LINK_RPM, FRAME0, 8, &v));
	TEST_ASSERT_EQUAL_FLOAT(4200.0f, v);
}

static void test_gate_rejects_other_frames(void) {
	/* The whole point: frame 2's bytes 2-3 are NOT rpm. Before the gate
	 * existed this returned true and RPM read whatever frame 2 carried. */
	float v = -1.0f;
	TEST_ASSERT_FALSE(decode_if_matches(&LINK_RPM, FRAME2, 8, &v));
	TEST_ASSERT_EQUAL_FLOAT(-1.0f, v);   /* untouched */
}

static void test_gate_decodes_correct_frame_with_offset(void) {
	float v = 0.0f;
	TEST_ASSERT_TRUE(decode_if_matches(&LINK_ECT, FRAME2, 8, &v));
	TEST_ASSERT_EQUAL_FLOAT(90.0f, v);
	TEST_ASSERT_FALSE(decode_if_matches(&LINK_ECT, FRAME0, 8, &v));
}

static void test_ungated_signal_decodes_every_frame(void) {
	/* mux_bit_length 0 = not multiplexed, the default every other preset
	 * uses. Both frames must decode, byte-for-byte as before this field
	 * existed — this is the back-compat assertion. */
	sig_t plain = LINK_RPM;
	plain.mux_bit_length = 0;
	float a = 0.0f, b = 0.0f;
	TEST_ASSERT_TRUE(decode_if_matches(&plain, FRAME0, 8, &a));
	TEST_ASSERT_TRUE(decode_if_matches(&plain, FRAME2, 8, &b));
	TEST_ASSERT_EQUAL_FLOAT(4200.0f, a);
	TEST_ASSERT_EQUAL_FLOAT(0.0f, b);
}

static void test_gate_rejects_frame_too_short_for_mux(void) {
	/* A 0-byte frame can't carry a frame index. Must be dropped, not read
	 * out of uninitialised memory. */
	float v = 0.0f;
	TEST_ASSERT_FALSE(decode_if_matches(&LINK_RPM, FRAME0, 0, &v));
}

static void test_gate_distinguishes_all_fourteen_frames(void) {
	/* Sweep every frame index against a signal gated on each one: exactly
	 * one index may pass. Catches an off-by-one or a truncating compare. */
	for (uint16_t want = 0; want < 14; want++) {
		sig_t s = LINK_RPM;
		s.mux_value = want;
		int passes = 0;
		for (uint16_t got = 0; got < 14; got++) {
			uint8_t f[8] = { (uint8_t)got, 0, 0x68, 0x10, 0, 0, 0, 0 };
			float v;
			if (decode_if_matches(&s, f, 8, &v)) passes++;
		}
		TEST_ASSERT_EQUAL_INT(1, passes);
	}
}

/* ── Half 2: the real catalogue table ────────────────────────────────────
 *
 * preset_picker_data.c is host-compilable by design (ADR-0033 — the baked
 * web catalogue is generated from it), so these assert the shipped data
 * rather than a copy of it.
 */
#define IS_LINK(it) ((it)->ecu && strcmp((it)->ecu, "Link ECU") == 0)

static void test_link_rows_all_share_one_can_id(void) {
	int seen = 0;
	for (int i = 0; i < preconfig_items_count; i++) {
		const preconfig_item_t *it = &preconfig_items[i];
		if (!IS_LINK(it)) continue;
		seen++;
		TEST_ASSERT_EQUAL_STRING("3E8", it->can_id);
	}
	TEST_ASSERT_TRUE(seen >= 40);   /* 41 rows today */
}

static void test_link_rows_are_all_gated_on_byte_zero(void) {
	for (int i = 0; i < preconfig_items_count; i++) {
		const preconfig_item_t *it = &preconfig_items[i];
		if (!IS_LINK(it)) continue;
		TEST_ASSERT_EQUAL_INT(8, it->mux_bit_length);
		TEST_ASSERT_EQUAL_INT(0, it->mux_bit_start);
		/* Payload never overlaps the frame index or the reserved byte 1. */
		TEST_ASSERT_TRUE(it->bit_start >= 16);
	}
}

static void test_link_covers_many_distinct_frames(void) {
	/* If a future edit collapsed the mux values, detection would silently
	 * fall back to scoring one frame — the original bug. */
	unsigned mask = 0;
	for (int i = 0; i < preconfig_items_count; i++) {
		const preconfig_item_t *it = &preconfig_items[i];
		if (!IS_LINK(it)) continue;
		TEST_ASSERT_TRUE(it->mux_value < 16);   /* tracker only records 0-15 */
		mask |= 1u << it->mux_value;
	}
	int distinct = 0;
	for (int b = 0; b < 16; b++) if (mask & (1u << b)) distinct++;
	TEST_ASSERT_TRUE(distinct >= 10);
}

static void test_no_preset_looks_like_a_mismodelled_mux(void) {
	/* The bug class, as a guard. For each (ecu, version): if its ids form a
	 * consecutive run of 3+ AND not one row in the whole preset ever reads
	 * bits 0-15, that is the fingerprint of a multiplexed stream flattened
	 * into separate ids — bytes 0-1 go unused precisely because they were
	 * never data. A genuinely multi-id preset uses byte 0 somewhere.
	 *
	 * Multiplexed presets are exempt: they have already been modelled
	 * correctly and legitimately reserve byte 0. */
	for (int i = 0; i < preconfig_items_count; i++) {
		const preconfig_item_t *a = &preconfig_items[i];
		if (!a->ecu || !a->version || a->obd2_pid) continue;
		/* Only evaluate each (ecu, version) once — at its first row. */
		bool first = true;
		for (int k = 0; k < i; k++) {
			const preconfig_item_t *p = &preconfig_items[k];
			if (p->ecu && p->version && !p->obd2_pid &&
			    !strcmp(p->ecu, a->ecu) && !strcmp(p->version, a->version)) {
				first = false; break;
			}
		}
		if (!first) continue;

		unsigned long lo = ~0UL, hi = 0;
		int ids = 0, uses_low_bytes = 0, muxed = 0;
		unsigned long seen_ids[64];
		for (int k = 0; k < preconfig_items_count; k++) {
			const preconfig_item_t *r = &preconfig_items[k];
			if (!r->ecu || !r->version || r->obd2_pid) continue;
			if (strcmp(r->ecu, a->ecu) || strcmp(r->version, a->version)) continue;
			if (r->mux_bit_length) muxed = 1;
			if (r->bit_start < 16) uses_low_bytes = 1;
			unsigned long id = strtoul(r->can_id, NULL, 16);
			bool dup = false;
			for (int q = 0; q < ids; q++) if (seen_ids[q] == id) { dup = true; break; }
			if (!dup && ids < 64) {
				seen_ids[ids++] = id;
				if (id < lo) lo = id;
				if (id > hi) hi = id;
			}
		}
		if (muxed || ids < 3) continue;
		bool consecutive = ((hi - lo + 1) == (unsigned long)ids);
		if (consecutive && !uses_low_bytes) {
			char msg[128];
			snprintf(msg, sizeof(msg),
			         "%s %s: %d consecutive ids and no row reads bytes 0-1 "
			         "— looks like a multiplexed stream split across ids",
			         a->ecu, a->version, ids);
			TEST_FAIL(msg);
		}
	}
}

/* ── Half 3: the drain coalescer ─────────────────────────────────────────
 *
 * Gating the decode was only half the job. can_process_queued_frames()
 * batches a drain and collapses it to the freshest frame per key before
 * calling signal_dispatch_frame(), so widgets repaint once per render cycle
 * instead of once per intermediate value. That is lossless when an id always
 * carries the same quantity — and lossy when it doesn't. Keyed on the id
 * alone, all 14 Link payloads in a batch collapsed to whichever arrived last
 * and the other 13 never reached the gate above at all, so every Link channel
 * updated at render_rate/14 (~2 Hz) while the raw trace showed full rate.
 *
 * Like the gate, the real coalescer can't be linked here (TWAI, FreeRTOS,
 * the PSRAM registry), so the keying is reproduced verbatim over the real
 * can_extract_bits(). Keep the two in lockstep.
 */
typedef struct { uint32_t id; uint8_t data[8]; uint8_t dlc; bool extd; uint32_t mux; } coal_t;

/* mux_aware=false reproduces the pre-fix key, so the regression this guards
 * is asserted rather than described. */
static int coalesce(const coal_t *in, int n, bool mux_aware, coal_t *out) {
	int n_out = 0;
	for (int i = 0; i < n; i++) {
		uint32_t mux = 0;
		if (mux_aware && in[i].id == 0x3E8 && in[i].dlc > 0)
			mux = (uint32_t)can_extract_bits(in[i].data, 0, 8, 1, false);
		int k;
		for (k = 0; k < n_out; k++)
			if (out[k].id == in[i].id && out[k].extd == in[i].extd &&
			    out[k].mux == mux) break;
		if (k < 32) {
			out[k] = in[i];
			out[k].mux = mux;
			if (k == n_out) n_out++;
		}
	}
	return n_out;
}

/* One full Link cycle, indices 0..13, each tagged in byte 2 so we can tell
 * which payload survived. Plus two frames on an unrelated id. */
static int build_link_batch(coal_t *b) {
	int n = 0;
	for (int f = 0; f < 14; f++) {
		b[n].id = 0x3E8; b[n].extd = false; b[n].dlc = 8; b[n].mux = 0;
		memset(b[n].data, 0, 8);
		b[n].data[0] = (uint8_t)f;
		b[n].data[2] = (uint8_t)(0xA0 + f);
		n++;
	}
	for (int r = 0; r < 2; r++) {
		b[n].id = 0x360; b[n].extd = false; b[n].dlc = 8; b[n].mux = 0;
		memset(b[n].data, 0, 8);
		b[n].data[2] = (uint8_t)(0x10 + r);
		n++;
	}
	return n;
}

static void test_old_key_collapsed_the_whole_link_cycle(void) {
	coal_t in[32], out[32];
	int n = build_link_batch(in);
	int got = coalesce(in, n, false, out);

	/* 0x3E8 and 0x360 — two entries for sixteen frames. */
	TEST_ASSERT_EQUAL_INT(2, got);
	TEST_ASSERT_EQUAL_HEX(0x3E8, out[0].id);
	/* Only the LAST index survived; payloads 0..12 were discarded. */
	TEST_ASSERT_EQUAL_INT(13,   out[0].data[0]);
	TEST_ASSERT_EQUAL_INT(0xA0 + 13, out[0].data[2]);
}

static void test_mux_key_preserves_every_frame_index(void) {
	coal_t in[32], out[32];
	int n = build_link_batch(in);
	int got = coalesce(in, n, true, out);

	/* 14 Link payloads + 1 collapsed entry for the ordinary id. */
	TEST_ASSERT_EQUAL_INT(15, got);

	for (int f = 0; f < 14; f++) {
		TEST_ASSERT_EQUAL_HEX(0x3E8, out[f].id);
		TEST_ASSERT_EQUAL_HEX((uint32_t)f, out[f].mux);
		TEST_ASSERT_EQUAL_INT((uint8_t)f, out[f].data[0]);
		TEST_ASSERT_EQUAL_INT((uint8_t)(0xA0 + f), out[f].data[2]);
	}
	/* The non-multiplexed id must still coalesce — last-value-wins is what
	 * keeps the dirty-rect flood away for every normal ECU. */
	TEST_ASSERT_EQUAL_HEX(0x360, out[14].id);
	TEST_ASSERT_EQUAL_INT(0x11,   out[14].data[2]);
}

static void test_mux_key_survives_multiple_cycles_in_one_batch(void) {
	/* Two cycles of the first eight indices: each index must appear once,
	 * holding the FRESHER of its two payloads. */
	coal_t in[32], out[32];
	int n = 0;
	for (int pass = 0; pass < 2; pass++) {
		for (int f = 0; f < 8; f++) {
			in[n].id = 0x3E8; in[n].extd = false; in[n].dlc = 8; in[n].mux = 0;
			memset(in[n].data, 0, 8);
			in[n].data[0] = (uint8_t)f;
			in[n].data[2] = (uint8_t)(pass ? 0xB0 + f : 0xA0 + f);
			n++;
		}
	}
	int got = coalesce(in, n, true, out);
	TEST_ASSERT_EQUAL_INT(8, got);
	for (int f = 0; f < 8; f++)
		TEST_ASSERT_EQUAL_INT((uint8_t)(0xB0 + f), out[f].data[2]);
}

/* ── Runner ─────────────────────────────────────────────────────────────── */

int main(void) {
	UNITY_BEGIN();

	RUN_TEST(test_gate_accepts_its_own_frame);
	RUN_TEST(test_gate_rejects_other_frames);
	RUN_TEST(test_gate_decodes_correct_frame_with_offset);
	RUN_TEST(test_ungated_signal_decodes_every_frame);
	RUN_TEST(test_gate_rejects_frame_too_short_for_mux);
	RUN_TEST(test_gate_distinguishes_all_fourteen_frames);

	RUN_TEST(test_link_rows_all_share_one_can_id);
	RUN_TEST(test_link_rows_are_all_gated_on_byte_zero);
	RUN_TEST(test_link_covers_many_distinct_frames);
	RUN_TEST(test_no_preset_looks_like_a_mismodelled_mux);

	RUN_TEST(test_old_key_collapsed_the_whole_link_cycle);
	RUN_TEST(test_mux_key_preserves_every_frame_index);
	RUN_TEST(test_mux_key_survives_multiple_cycles_in_one_batch);

	return UNITY_END();
}
