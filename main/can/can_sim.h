/*
 * can_sim.h — a bench ECU that broadcasts an ECU preset's CAN stream.
 *
 * The CAN twin of the OBD2 virtual ECU (obd2_sim_set_enabled): with no car
 * on the bench, it builds frames for every channel of one preset (e.g.
 * Haltech Nexus) from the preset table itself and feeds them into the same
 * receive queue a real bus uses, so ECU detection, channel decoding, gauges
 * and the channels editor all run for real. The setup wizard's bitrate scan
 * listens to the CAN driver directly, so can_bus_test also asks
 * can_sim_scan_frames() what the bus "would" carry at each rate.
 *
 * Values are what an idling, warm engine reads (RPM ~850 with a little
 * movement, coolant 86 C, 14.1 V…), encoded through the dash's own decoder as
 * an oracle, so every channel reads back exactly. Not persisted: a restart
 * turns it off. For demos, guides and bench tests only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Start broadcasting the preset @p ecu / @p version. False if the preset has
 *  no CAN rows. Replaces any running simulation. */
bool can_sim_start(const char *ecu, const char *version);
void can_sim_stop(void);
bool can_sim_active(void);
const char *can_sim_ecu(void);
const char *can_sim_version(void);

/** For the bitrate scan: at @p bitrate_idx (0 = 125 k … 3 = 1 M), how many
 *  frames a listen of @p listen_ms would have heard, and which IDs (up to
 *  @p max written to @p ids, count in @p n_ids). 0 frames at other rates. */
uint32_t can_sim_scan_frames(uint8_t bitrate_idx, uint32_t listen_ms,
                             uint32_t *ids, uint8_t max, uint8_t *n_ids);

#ifdef __cplusplus
}
#endif
