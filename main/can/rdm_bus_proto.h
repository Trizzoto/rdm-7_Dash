/*
 * rdm_bus_proto.h — the RDM device-to-device bus contract: who is on the bus,
 *                   where they answer, and how a track outline gets between
 *                   them when nothing else is connected.
 *
 * Vendored byte-for-byte into rdm-gps-node. Both ends MUST build the same
 * file, the same way lap_core.c is shared — a protocol described twice is a
 * protocol that disagrees with itself the first time someone edits one copy.
 * Pure C: no ESP-IDF, no FreeRTOS, no LVGL, so tests/native compiles it on the
 * host and the wire format is testable without a car.
 *
 *
 * ── Why discovery needs a fixed address ────────────────────────────────────
 *
 * The puck's block base is configurable (16-aligned, 0x100..0x7F0) so it can
 * dodge an ECU that already owns 0x400. That creates a chicken-and-egg: the
 * dash cannot hear "my base is 0x420" if the announcement itself rides the
 * base it is trying to announce.
 *
 * So exactly ONE id never moves: RDM_BUS_DISCOVERY_ID. Every RDM device
 * broadcasts its identity and its base there at 1 Hz, and listens there
 * always. Everything else rides the discovered base. Change a base in Studio
 * and the other device knows inside a second, with no re-pairing — that is
 * the whole point.
 *
 * 0x4FF is chosen deliberately:
 *   - it sits in 0x4C0-0x4FF, which the RDM id map already reserves and no
 *     RDM device has claimed (see rdm-gps-node docs/PROTOCOL.md);
 *   - it is clear of the aftermarket ECUs a dash actually meets — Haltech
 *     0x360-0x373, Link 0x3E8-0x3F1, MoTeC 0x500+, MaxxECU 0x520+,
 *     ECUMaster/CANopen SDO 0x580+ — and nowhere near OBD2 (0x7DF, 0x7E8-EF);
 *   - it is a HIGH id, so it loses arbitration to engine data. A 1 Hz
 *     housekeeping frame should always yield to the ECU, and this one does by
 *     construction rather than by politeness.
 *
 * It is deliberately NOT configurable. One fixed rendezvous point is what
 * makes every other address negotiable.
 *
 *
 * ── Why the transfer is not ISO-TP ─────────────────────────────────────────
 *
 * A whole outline fits in one ISO-TP transfer (TRACK_MAP_MAX_FILE is 3252 B,
 * under the 4095 B ceiling), so the tempting answer is to reuse the OBD2 code.
 * It is the wrong answer here. ISO-TP is a stream: lose a consecutive frame
 * and the whole transfer aborts, and this transfer happens in a car that may
 * be cranking — brownout, ECU reset and a burst of bus errors are the normal
 * case, not the exception.
 *
 * So the payload is sequenced, not streamed. Every data frame carries its own
 * index, the receiver tracks which ones arrived, and the sender resends only
 * the gaps. A crank mid-transfer costs the frames that were in flight, not the
 * transfer. Nothing is published until the CRC over the whole payload matches.
 */
#ifndef RDM_BUS_PROTO_H
#define RDM_BUS_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The one address that never moves. See the header comment. */
#define RDM_BUS_DISCOVERY_ID      0x4FFu

/* Bumped only for an incompatible change. A device that meets a higher
 * version than it knows must ignore the peer for transfer purposes rather
 * than guess — a half-understood outline is worse than no outline. */
#define RDM_BUS_PROTO_VERSION     1u

/* Offsets inside a device's own (configurable) block. The GPS block documents
 * +0xA..+0xD as reserved; asset transfer claims two of them, which keeps
 * COMMAND/RESPONSE at +0xE/+0xF where they already are. */
#define RDM_BUS_OFF_ASSET_TX      0x0Au   /* holder -> requester: data      */
#define RDM_BUS_OFF_ASSET_RX      0x0Bu   /* requester -> holder: control   */

/* Device classes on the bus. */
typedef enum {
    RDM_BUS_DEV_UNKNOWN = 0,
    RDM_BUS_DEV_DASH    = 1,
    RDM_BUS_DEV_GPS     = 2,
    RDM_BUS_DEV_IO      = 3,
    RDM_BUS_DEV_KEYPAD  = 4,
} rdm_bus_devtype_t;

/* Capability bits in the discovery frame. Both directions matter: the dash
 * seeds the puck as readily as the puck seeds the dash, so "who sends" is
 * decided by which side holds the newer revision, not by device class. */
#define RDM_BUS_CAP_HAS_OUTLINE   (1u << 0)  /* holds a track outline        */
#define RDM_BUS_CAP_HAS_GATES     (1u << 1)  /* holds lap gates              */
#define RDM_BUS_CAP_WANTS_OUTLINE (1u << 2)  /* would display one            */
#define RDM_BUS_CAP_CAN_SEND      (1u << 3)  /* can serve an asset transfer  */

/* ── Discovery frame (8 bytes, id RDM_BUS_DISCOVERY_ID, 1 Hz) ──────────────
 *
 *   0    u8   device type (rdm_bus_devtype_t)
 *   1    u8   protocol version
 *   2..3 u16  this device's block base id, little-endian
 *   4    u8   capability bits
 *   5..7 u24  revision of the track this device holds, little-endian
 *             (0 = none). Studio stamps it; highest wins, so there is never
 *             an ambiguous merge and the two cannot ping-pong.
 */
typedef struct {
    uint8_t  devtype;
    uint8_t  proto_ver;
    uint16_t base_id;
    uint8_t  caps;
    uint32_t track_rev;   /* 24-bit on the wire */
} rdm_bus_announce_t;

#define RDM_BUS_TRACK_REV_MAX     0xFFFFFFu

void rdm_bus_announce_pack(const rdm_bus_announce_t *a, uint8_t out[8]);
bool rdm_bus_announce_unpack(const uint8_t in[8], rdm_bus_announce_t *out);

/* ── Asset transfer ────────────────────────────────────────────────────────
 *
 * Control frames ride the REQUESTER's block at +RDM_BUS_OFF_ASSET_RX; data
 * frames ride the HOLDER's block at +RDM_BUS_OFF_ASSET_TX. Keeping each
 * device's traffic inside its own block means neither has to be told an id to
 * transmit on — it already knows its own base, and discovery told it the
 * other's.
 *
 *   BEGIN   holder announces size + crc, then streams
 *   DATA    u16 sequence + 6 payload bytes
 *   NAK     receiver names the first missing sequence; sender resumes there
 *   DONE    receiver confirms crc matched and the file is published
 *   ABORT   either side gives up (moving, busy, bad payload)
 */
typedef enum {
    RDM_BUS_MSG_REQUEST = 0x01,  /* requester: send me revision N            */
    RDM_BUS_MSG_BEGIN   = 0x02,  /* holder:    total bytes, chunk count, crc */
    RDM_BUS_MSG_NAK     = 0x03,  /* requester: I am missing from seq N       */
    RDM_BUS_MSG_DONE    = 0x04,  /* requester: crc matched, stored           */
    RDM_BUS_MSG_ABORT   = 0x05,  /* either:    stop, with a reason           */
} rdm_bus_msg_t;

typedef enum {
    RDM_BUS_ABORT_NONE      = 0,
    RDM_BUS_ABORT_MOVING    = 1,  /* car is moving; try again when stopped   */
    RDM_BUS_ABORT_BUSY      = 2,
    RDM_BUS_ABORT_TOO_BIG   = 3,  /* exceeds the receiver's point cap        */
    RDM_BUS_ABORT_BAD_CRC   = 4,
    RDM_BUS_ABORT_BAD_DATA  = 5,  /* parsed, and it is not a track           */
    RDM_BUS_ABORT_TIMEOUT   = 6,
} rdm_bus_abort_t;

#define RDM_BUS_DATA_PAYLOAD      6u    /* bytes of file per data frame */

/* Ceiling mirrors TRACK_MAP_MAX_FILE (400 points + 52 B header). Declared
 * here too so the node can bound its buffers without vendoring the widget. */
#define RDM_BUS_ASSET_MAX_BYTES   3252u
#define RDM_BUS_ASSET_MAX_SEQ     ((RDM_BUS_ASSET_MAX_BYTES + RDM_BUS_DATA_PAYLOAD - 1) / RDM_BUS_DATA_PAYLOAD)

typedef struct {
    uint8_t  msg;          /* rdm_bus_msg_t */
    uint32_t track_rev;    /* 24-bit */
    uint16_t total_bytes;  /* BEGIN only */
    uint32_t crc32;        /* BEGIN only */
    uint16_t seq;          /* NAK only: first missing sequence */
    uint8_t  reason;       /* ABORT only */
} rdm_bus_ctrl_t;

void rdm_bus_ctrl_pack(const rdm_bus_ctrl_t *c, uint8_t out[8]);
bool rdm_bus_ctrl_unpack(const uint8_t in[8], rdm_bus_ctrl_t *out);

/* Data frame: u16 seq little-endian, then up to 6 bytes. `len` is how many of
 * those 6 are real — only the final frame is short. */
void rdm_bus_data_pack(uint16_t seq, const uint8_t *src, uint8_t len, uint8_t out[8]);
bool rdm_bus_data_unpack(const uint8_t in[8], uint8_t dlc, uint16_t *seq,
                         const uint8_t **payload, uint8_t *len);

/* CRC32 (IEEE, reflected) over the whole file. Same polynomial as zlib so a
 * desktop can compute the expected value without embedding this code. */
uint32_t rdm_bus_crc32(const uint8_t *data, size_t len);

/* ── Receiver reassembly ───────────────────────────────────────────────────
 *
 * Owns the "which frames arrived" bitmap so both firmwares reassemble
 * identically and the logic is testable on the host. The caller supplies the
 * destination buffer; nothing here allocates.
 */
typedef struct {
    uint8_t  *buf;
    size_t    buf_cap;
    uint16_t  total_bytes;
    uint16_t  total_seq;
    uint32_t  expect_crc;
    uint32_t  track_rev;
    bool      active;
    uint8_t   got[(RDM_BUS_ASSET_MAX_SEQ + 7) / 8];
} rdm_bus_rx_t;

/* Start reassembly from a BEGIN. False if it cannot fit — the caller should
 * ABORT with TOO_BIG rather than half-receive. */
bool rdm_bus_rx_begin(rdm_bus_rx_t *rx, uint8_t *buf, size_t cap,
                      const rdm_bus_ctrl_t *begin);

/* Feed one data frame. Out-of-range or duplicate frames are ignored, which is
 * what makes a resend after NAK harmless. */
void rdm_bus_rx_data(rdm_bus_rx_t *rx, uint16_t seq, const uint8_t *payload, uint8_t len);

/* First sequence still missing, or -1 when the payload is complete. Drives
 * both the NAK and the "are we done" test, so they can never disagree. */
int32_t rdm_bus_rx_first_missing(const rdm_bus_rx_t *rx);

/* Complete AND crc matches. Only then is it safe to publish. */
bool rdm_bus_rx_complete(const rdm_bus_rx_t *rx);

#ifdef __cplusplus
}
#endif

#endif /* RDM_BUS_PROTO_H */
