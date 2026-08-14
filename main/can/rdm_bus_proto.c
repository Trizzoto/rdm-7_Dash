/*
 * rdm_bus_proto.c — pack/unpack and reassembly for the RDM bus contract.
 *
 * See rdm_bus_proto.h for why discovery is at a fixed id and why the transfer
 * is sequenced rather than streamed. Vendored byte-for-byte into
 * rdm-gps-node; keep the two identical.
 *
 * Everything here is pure arithmetic over byte buffers so tests/native can
 * exercise the wire format on the host. Bugs in this file are exactly the kind
 * that do not announce themselves on a car — a swapped endianness or an
 * off-by-one in the sequence bitmap looks like "the track sometimes does not
 * arrive", which is unfalsifiable in a pit lane.
 */
#include "rdm_bus_proto.h"

#include <string.h>

/* ── little-endian helpers ────────────────────────────────────────────────── */

static void _put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static uint16_t _get_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void _put_u24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
}

static uint32_t _get_u24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/* ── Discovery ────────────────────────────────────────────────────────────── */

void rdm_bus_announce_pack(const rdm_bus_announce_t *a, uint8_t out[8]) {
    if (!a || !out) return;
    memset(out, 0, 8);
    out[0] = a->devtype;
    out[1] = a->proto_ver;
    _put_u16(&out[2], a->base_id);
    out[4] = a->caps;
    _put_u24(&out[5], a->track_rev & RDM_BUS_TRACK_REV_MAX);
}

bool rdm_bus_announce_unpack(const uint8_t in[8], rdm_bus_announce_t *out) {
    if (!in || !out) return false;
    out->devtype   = in[0];
    out->proto_ver = in[1];
    out->base_id   = _get_u16(&in[2]);
    out->caps      = in[4];
    out->track_rev = _get_u24(&in[5]);

    /* A base has to be a real 11-bit id on a 16-aligned block, or the peer is
     * misconfigured (or this is not our frame at all). Reject rather than
     * transmit into whatever that address turns out to be — the one thing
     * worse than not finding the puck is talking over an ECU. */
    if (out->base_id > 0x7F0u || (out->base_id & 0x0Fu) != 0u) return false;
    if (out->devtype == RDM_BUS_DEV_UNKNOWN) return false;
    return true;
}

/* ── Control frames ───────────────────────────────────────────────────────── */

void rdm_bus_ctrl_pack(const rdm_bus_ctrl_t *c, uint8_t out[8]) {
    if (!c || !out) return;
    memset(out, 0, 8);
    out[0] = c->msg;
    _put_u24(&out[1], c->track_rev & RDM_BUS_TRACK_REV_MAX);

    switch (c->msg) {
    case RDM_BUS_MSG_BEGIN:
        /* total_bytes then the low 3 bytes of the crc do not fit together, so
         * BEGIN spends its remaining 4 bytes on the crc and carries the size
         * in the two before it. */
        _put_u16(&out[4], c->total_bytes);
        out[6] = (uint8_t)(c->crc32 & 0xFF);
        out[7] = (uint8_t)((c->crc32 >> 8) & 0xFF);
        break;
    case RDM_BUS_MSG_NAK:
        _put_u16(&out[4], c->seq);
        break;
    case RDM_BUS_MSG_ABORT:
        out[4] = c->reason;
        break;
    default:
        break;
    }
}

bool rdm_bus_ctrl_unpack(const uint8_t in[8], rdm_bus_ctrl_t *out) {
    if (!in || !out) return false;
    memset(out, 0, sizeof(*out));
    out->msg       = in[0];
    out->track_rev = _get_u24(&in[1]);

    switch (out->msg) {
    case RDM_BUS_MSG_BEGIN:
        out->total_bytes = _get_u16(&in[4]);
        out->crc32       = (uint32_t)in[6] | ((uint32_t)in[7] << 8);
        break;
    case RDM_BUS_MSG_NAK:
        out->seq = _get_u16(&in[4]);
        break;
    case RDM_BUS_MSG_ABORT:
        out->reason = in[4];
        break;
    case RDM_BUS_MSG_REQUEST:
    case RDM_BUS_MSG_DONE:
        break;
    default:
        return false;   /* unknown message: ignore, do not guess */
    }
    return true;
}

/* ── Data frames ──────────────────────────────────────────────────────────── */

void rdm_bus_data_pack(uint16_t seq, const uint8_t *src, uint8_t len, uint8_t out[8]) {
    if (!out) return;
    memset(out, 0, 8);
    if (len > RDM_BUS_DATA_PAYLOAD) len = RDM_BUS_DATA_PAYLOAD;
    _put_u16(&out[0], seq);
    if (src && len) memcpy(&out[2], src, len);
}

bool rdm_bus_data_unpack(const uint8_t in[8], uint8_t dlc, uint16_t *seq,
                         const uint8_t **payload, uint8_t *len) {
    if (!in || !seq || !payload || !len) return false;
    if (dlc < 3) return false;          /* seq + at least one byte */
    *seq     = _get_u16(&in[0]);
    *payload = &in[2];
    *len     = (uint8_t)(dlc - 2);
    if (*len > RDM_BUS_DATA_PAYLOAD) *len = RDM_BUS_DATA_PAYLOAD;
    return true;
}

/* ── CRC32 (IEEE, reflected — same as zlib) ───────────────────────────────── */

uint32_t rdm_bus_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    if (!data) return 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            /* Bitwise rather than a 1 KB table: this runs a few times per
             * transfer, not per frame, and the node has less RAM to spare
             * than it has microseconds. */
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

/* ── Receiver reassembly ──────────────────────────────────────────────────── */

static bool _bit_get(const uint8_t *bm, uint16_t i) {
    return (bm[i >> 3] & (uint8_t)(1u << (i & 7u))) != 0u;
}

static void _bit_set(uint8_t *bm, uint16_t i) {
    bm[i >> 3] |= (uint8_t)(1u << (i & 7u));
}

bool rdm_bus_rx_begin(rdm_bus_rx_t *rx, uint8_t *buf, size_t cap,
                      const rdm_bus_ctrl_t *begin) {
    if (!rx || !buf || !begin) return false;
    if (begin->total_bytes == 0) return false;
    if (begin->total_bytes > cap) return false;
    if (begin->total_bytes > RDM_BUS_ASSET_MAX_BYTES) return false;

    memset(rx, 0, sizeof(*rx));
    rx->buf         = buf;
    rx->buf_cap     = cap;
    rx->total_bytes = begin->total_bytes;
    rx->total_seq   = (uint16_t)((begin->total_bytes + RDM_BUS_DATA_PAYLOAD - 1)
                                 / RDM_BUS_DATA_PAYLOAD);
    rx->expect_crc  = begin->crc32;
    rx->track_rev   = begin->track_rev;
    rx->active      = true;
    return true;
}

void rdm_bus_rx_data(rdm_bus_rx_t *rx, uint16_t seq, const uint8_t *payload, uint8_t len) {
    if (!rx || !rx->active || !payload) return;
    if (seq >= rx->total_seq) return;               /* out of range: ignore */
    if (_bit_get(rx->got, seq)) return;             /* duplicate: ignore, so a
                                                     * resend after NAK costs
                                                     * nothing */
    size_t off = (size_t)seq * RDM_BUS_DATA_PAYLOAD;
    if (off >= rx->total_bytes) return;

    size_t remain = rx->total_bytes - off;
    size_t n = (len < remain) ? len : remain;       /* the last frame is short */
    memcpy(rx->buf + off, payload, n);
    _bit_set(rx->got, seq);
}

int32_t rdm_bus_rx_first_missing(const rdm_bus_rx_t *rx) {
    if (!rx || !rx->active) return 0;
    for (uint16_t i = 0; i < rx->total_seq; i++) {
        if (!_bit_get(rx->got, i)) return (int32_t)i;
    }
    return -1;
}

bool rdm_bus_rx_complete(const rdm_bus_rx_t *rx) {
    if (!rx || !rx->active) return false;
    if (rdm_bus_rx_first_missing(rx) >= 0) return false;
    /* Only the low 16 bits of the crc fit in BEGIN, so that is what is
     * compared. Combined with the length check and the fact that the widget
     * parser still has to accept the result, a corrupt outline getting all the
     * way to the screen is not a realistic outcome. */
    uint32_t have = rdm_bus_crc32(rx->buf, rx->total_bytes) & 0xFFFFu;
    return have == (rx->expect_crc & 0xFFFFu);
}
