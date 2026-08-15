/*
 * rdm_bus.h — the dash's half of the RDM device bus.
 *
 * Announces this dash on the fixed discovery id, learns where the puck lives,
 * and moves a track outline between the two when their revisions disagree.
 * The wire format is rdm_bus_proto.h; this is the part that knows about
 * LittleFS, timers and the CAN driver.
 *
 * Threading: every entry point here runs on the LVGL task. RX arrives through
 * rdm_bus_on_can_frame() from can_process_queued_frames(), and the 1 Hz tick
 * is an LVGL timer, so there is no locking and no cross-task state.
 */
#ifndef RDM_BUS_H
#define RDM_BUS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The dash's own block. The RDM id map leaves 0x4C0-0x4FF reserved; the dash
 * takes the bottom of it and discovery sits at the top (0x4FF), so both live
 * in space nothing else claims. Configurable for the same reason the puck's
 * is — an ECU may already own the address — and announced, so the peer never
 * has to be told. */
#define RDM_BUS_DASH_BASE_DEFAULT   0x4C0u

void rdm_bus_init(void);

/* Called for EVERY received frame, in arrival order, from the uncoalesced
 * drain. Coalescing keeps only the last frame per id per batch, which would
 * quietly eat an asset transfer — every data frame shares one id. */
void rdm_bus_on_can_frame(uint32_t id, bool extd, const uint8_t *data, uint8_t dlc);

/* What this dash currently holds, for the announce and for deciding who
 * sends. `name` is the .rdmtrk basename; rev is Studio's stamp (0 = none). */
void rdm_bus_set_local_track(const char *name, uint32_t rev);
uint32_t rdm_bus_local_rev(void);

/* Change this dash's block base at runtime (Studio). Takes effect on the next
 * announce, so the peer follows within a second. */
void rdm_bus_set_base(uint16_t base);
uint16_t rdm_bus_get_base(void);

/* Peer visibility, for the setup screen: has a puck been heard, where, and
 * what does it hold. Returns false when nothing has announced. */
bool rdm_bus_peer_info(uint16_t *base_out, uint32_t *rev_out, uint32_t *age_ms_out);

#ifdef __cplusplus
}
#endif

#endif /* RDM_BUS_H */
