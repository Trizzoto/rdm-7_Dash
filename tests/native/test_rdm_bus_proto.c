/*
 * test_rdm_bus_proto.c — the RDM bus wire format, exercised on the host.
 *
 * This is the file that has to be right before any of it goes near a car. A
 * swapped endianness or an off-by-one in the sequence bitmap does not crash
 * anything; it shows up as "the track sometimes doesn't arrive", which nobody
 * can debug in a pit lane. So the transfer is simulated end to end here,
 * including the failure the design exists for: frames lost mid-transfer
 * because the engine cranked.
 */
#include "../../main/can/rdm_bus_proto.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

static void ok(const char *name, int cond) {
    if (cond) { printf("  [PASS] %s\n", name); g_pass++; }
    else      { printf("  [FAIL] %s\n", name); g_fail++; }
}

/* ── discovery ────────────────────────────────────────────────────────────── */

static void test_announce_roundtrip(void) {
    rdm_bus_announce_t a = {
        .devtype   = RDM_BUS_DEV_GPS,
        .proto_ver = RDM_BUS_PROTO_VERSION,
        .base_id   = 0x420,
        .caps      = RDM_BUS_CAP_HAS_OUTLINE | RDM_BUS_CAP_CAN_SEND,
        .track_rev = 0x0ABCDE,
    };
    uint8_t f[8];
    rdm_bus_announce_pack(&a, f);

    rdm_bus_announce_t b;
    ok("announce unpacks", rdm_bus_announce_unpack(f, &b));
    ok("announce devtype survives",   b.devtype == RDM_BUS_DEV_GPS);
    ok("announce base survives",      b.base_id == 0x420);
    ok("announce caps survive",       b.caps == a.caps);
    ok("announce rev survives (24b)", b.track_rev == 0x0ABCDE);
}

static void test_announce_rejects_nonsense(void) {
    /* A base that is not 16-aligned, or beyond 11 bits, means the peer is
     * misconfigured or this is not our frame. Either way transmitting at that
     * address could talk over an ECU, so it must be refused. */
    rdm_bus_announce_t a = { .devtype = RDM_BUS_DEV_GPS, .proto_ver = 1,
                             .base_id = 0x425, .caps = 0, .track_rev = 1 };
    uint8_t f[8];
    rdm_bus_announce_pack(&a, f);
    rdm_bus_announce_t b;
    ok("unaligned base rejected", !rdm_bus_announce_unpack(f, &b));

    a.base_id = 0x800;                     /* past 11 bits */
    rdm_bus_announce_pack(&a, f);
    ok("out-of-range base rejected", !rdm_bus_announce_unpack(f, &b));

    a.base_id = 0x400;
    a.devtype = RDM_BUS_DEV_UNKNOWN;
    rdm_bus_announce_pack(&a, f);
    ok("unknown devtype rejected", !rdm_bus_announce_unpack(f, &b));
}

static void test_discovery_id_is_fixed(void) {
    /* The whole scheme rests on this address never moving. If someone ever
     * "tidies" it into the configurable block, every pairing breaks silently
     * and this test is the only thing that says so. */
    ok("discovery id is 0x4FF", RDM_BUS_DISCOVERY_ID == 0x4FFu);
    ok("discovery clear of Haltech 0x360-0x373",
       RDM_BUS_DISCOVERY_ID < 0x360u || RDM_BUS_DISCOVERY_ID > 0x373u);
    ok("discovery clear of MoTeC/MaxxECU 0x500+", RDM_BUS_DISCOVERY_ID < 0x500u);
    ok("discovery clear of OBD2 0x7DF/0x7E8-EF", RDM_BUS_DISCOVERY_ID < 0x7DFu);
}

/* ── control frames ───────────────────────────────────────────────────────── */

static void test_ctrl_roundtrip(void) {
    rdm_bus_ctrl_t c = { .msg = RDM_BUS_MSG_BEGIN, .track_rev = 0x123456,
                         .total_bytes = 3252, .crc32 = 0xDEADBEEF };
    uint8_t f[8];
    rdm_bus_ctrl_pack(&c, f);
    rdm_bus_ctrl_t d;
    ok("BEGIN unpacks", rdm_bus_ctrl_unpack(f, &d));
    ok("BEGIN rev",     d.track_rev == 0x123456);
    ok("BEGIN size",    d.total_bytes == 3252);
    ok("BEGIN crc low16 survives", (d.crc32 & 0xFFFFu) == (0xDEADBEEFu & 0xFFFFu));

    rdm_bus_ctrl_t n = { .msg = RDM_BUS_MSG_NAK, .track_rev = 7, .seq = 511 };
    rdm_bus_ctrl_pack(&n, f);
    ok("NAK unpacks", rdm_bus_ctrl_unpack(f, &d));
    ok("NAK seq survives", d.seq == 511);

    rdm_bus_ctrl_t ab = { .msg = RDM_BUS_MSG_ABORT, .reason = RDM_BUS_ABORT_MOVING };
    rdm_bus_ctrl_pack(&ab, f);
    ok("ABORT unpacks", rdm_bus_ctrl_unpack(f, &d));
    ok("ABORT reason survives", d.reason == RDM_BUS_ABORT_MOVING);

    uint8_t junk[8] = { 0x7F, 0, 0, 0, 0, 0, 0, 0 };
    ok("unknown message refused", !rdm_bus_ctrl_unpack(junk, &d));
}

/* ── full transfer, including a crank ─────────────────────────────────────── */

/* A stand-in outline. Content does not matter to the transport, only that it
 * is byte-exact at the far end. */
static void fill_pattern(uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) b[i] = (uint8_t)((i * 31u + (i >> 3)) & 0xFF);
}

static void run_transfer(const char *label, size_t size, int drop_every) {
    static uint8_t src[RDM_BUS_ASSET_MAX_BYTES];
    static uint8_t dst[RDM_BUS_ASSET_MAX_BYTES];
    fill_pattern(src, size);
    memset(dst, 0, sizeof(dst));

    rdm_bus_ctrl_t begin = {
        .msg = RDM_BUS_MSG_BEGIN,
        .track_rev = 42,
        .total_bytes = (uint16_t)size,
        .crc32 = rdm_bus_crc32(src, size),
    };

    rdm_bus_rx_t rx;
    if (!rdm_bus_rx_begin(&rx, dst, sizeof(dst), &begin)) {
        printf("  [FAIL] %s: BEGIN refused\n", label);
        g_fail++;
        return;
    }

    uint16_t total_seq = (uint16_t)((size + RDM_BUS_DATA_PAYLOAD - 1) / RDM_BUS_DATA_PAYLOAD);

    /* First pass: the sender streams, and every drop_every-th frame is lost —
     * the bus errors you get while the starter is turning. */
    int sent = 0;
    for (uint16_t s = 0; s < total_seq; s++) {
        sent++;
        if (drop_every && (sent % drop_every) == 0) continue;
        size_t off = (size_t)s * RDM_BUS_DATA_PAYLOAD;
        size_t n = size - off;
        if (n > RDM_BUS_DATA_PAYLOAD) n = RDM_BUS_DATA_PAYLOAD;
        uint8_t f[8];
        rdm_bus_data_pack(s, src + off, (uint8_t)n, f);

        uint16_t gs; const uint8_t *pl; uint8_t gl;
        if (rdm_bus_data_unpack(f, (uint8_t)(2 + n), &gs, &pl, &gl))
            rdm_bus_rx_data(&rx, gs, pl, gl);
    }

    if (drop_every) {
        char b[96];
        snprintf(b, sizeof(b), "%s: incomplete after losses", label);
        ok(b, rdm_bus_rx_first_missing(&rx) >= 0);
        ok("  ...and refuses to publish", !rdm_bus_rx_complete(&rx));

        /* Recovery: NAK names the first gap, sender resumes from there. Repeat
         * until it closes — this is the loop the real implementation runs. */
        int rounds = 0;
        int32_t miss;
        while ((miss = rdm_bus_rx_first_missing(&rx)) >= 0 && rounds < 64) {
            rounds++;
            for (uint16_t s = (uint16_t)miss; s < total_seq; s++) {
                size_t off = (size_t)s * RDM_BUS_DATA_PAYLOAD;
                size_t n = size - off;
                if (n > RDM_BUS_DATA_PAYLOAD) n = RDM_BUS_DATA_PAYLOAD;
                uint8_t f[8];
                rdm_bus_data_pack(s, src + off, (uint8_t)n, f);
                uint16_t gs; const uint8_t *pl; uint8_t gl;
                if (rdm_bus_data_unpack(f, (uint8_t)(2 + n), &gs, &pl, &gl))
                    rdm_bus_rx_data(&rx, gs, pl, gl);
            }
        }
        char c[96];
        snprintf(c, sizeof(c), "%s: recovers in %d resume round(s)", label, rounds);
        ok(c, rounds > 0 && rounds <= 2);
    }

    char d[96];
    snprintf(d, sizeof(d), "%s: complete + crc", label);
    ok(d, rdm_bus_rx_complete(&rx));

    snprintf(d, sizeof(d), "%s: bytes identical", label);
    ok(d, memcmp(src, dst, size) == 0);
}

static void test_transfers(void) {
    run_transfer("mallala-sized (492 B)", 492, 0);
    run_transfer("max outline (3252 B)", RDM_BUS_ASSET_MAX_BYTES, 0);
    run_transfer("short/odd tail (500 B)", 500, 0);
    run_transfer("with crank losses", RDM_BUS_ASSET_MAX_BYTES, 17);
}

static void test_rx_guards(void) {
    static uint8_t dst[64];
    rdm_bus_ctrl_t begin = { .msg = RDM_BUS_MSG_BEGIN, .total_bytes = 4000,
                             .crc32 = 0 };
    rdm_bus_rx_t rx;
    ok("BEGIN larger than buffer refused",
       !rdm_bus_rx_begin(&rx, dst, sizeof(dst), &begin));

    begin.total_bytes = RDM_BUS_ASSET_MAX_BYTES + 1;
    static uint8_t big[RDM_BUS_ASSET_MAX_BYTES + 64];
    ok("BEGIN past the point cap refused",
       !rdm_bus_rx_begin(&rx, big, sizeof(big), &begin));

    begin.total_bytes = 0;
    ok("empty BEGIN refused", !rdm_bus_rx_begin(&rx, big, sizeof(big), &begin));

    /* Duplicates and strays must not corrupt a good transfer. */
    begin.total_bytes = 24;
    begin.crc32 = 0;
    ok("small BEGIN accepted", rdm_bus_rx_begin(&rx, big, sizeof(big), &begin));
    uint8_t six[6] = { 1, 2, 3, 4, 5, 6 };
    rdm_bus_rx_data(&rx, 0, six, 6);
    rdm_bus_rx_data(&rx, 0, six, 6);            /* duplicate */
    rdm_bus_rx_data(&rx, 9999, six, 6);         /* out of range */
    ok("dup + stray leave gap intact", rdm_bus_rx_first_missing(&rx) == 1);
}

static void test_crc_is_zlib(void) {
    /* Pinned against the known zlib crc32 of "123456789" so Studio can compute
     * the same value in JS without embedding this code. */
    const uint8_t v[] = "123456789";
    ok("crc32 matches zlib check vector",
       rdm_bus_crc32(v, 9) == 0xCBF43926u);
}

int main(void) {
    printf("=== rdm_bus_proto ===\n");
    test_discovery_id_is_fixed();
    test_announce_roundtrip();
    test_announce_rejects_nonsense();
    test_ctrl_roundtrip();
    test_crc_is_zlib();
    test_rx_guards();
    test_transfers();
    printf("\n=== %d tests, %d passed, %d failed ===\n",
           g_pass + g_fail, g_pass, g_fail);
    return g_fail ? 1 : 0;
}
