#pragma once

/* Lap engine — the ESP-IDF half.
 *
 * Owns everything lap_core.c deliberately does not: CAN frames, channels,
 * signals, persistence, logging. lap_core.c holds the arithmetic and is
 * host-tested (tests/native/test_lap_core.c); this file is the plumbing that
 * connects it to an RDM GPS on the bus.
 *
 * Threading: every function here runs on the LVGL task. lap_engine_on_can_frame
 * is called from can_process_queued_frames(), which already holds the LVGL
 * mutex, and the channel/signal APIs require it. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "lap/lap_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default RDM GPS block base. The puck's frames are base+0 status,
 * base+1 position, base+2 motion, base+3 quality — see the RDM GPS
 * PROTOCOL.md, which allocates 0x400-0x4FF to RDM bus devices. */
#define LAP_GPS_BASE_ID_DEFAULT 0x400u

/* Activate the lap channels and register their feeding signals. Idempotent.
 * Call once during boot, before dashboard_init(). */
esp_err_t lap_engine_init(void);

/* Begin/refresh operation. Idempotent and safe to call on every layout load,
 * matching channel_math_start(). */
void lap_engine_start(void);

/* Feed one CAN frame. Cheap and non-matching frames return immediately — this
 * sits in the hot path for every frame on the bus. `extd` frames never match:
 * a 29-bit id numerically inside the GPS block is somebody else's traffic.
 *
 * Takes RAW BYTES rather than reading the gps_latitude/gps_longitude channels
 * on purpose: channel values are floats, which quantise latitude to about
 * 1.7 m, and lap timing at 200 km/h resolves 1.1 m. Decoding here keeps the
 * position in a double all the way to lap_core. See ADR-0008. */
void lap_engine_on_can_frame(uint32_t can_id, bool extd, const uint8_t *data, uint8_t dlc);

/* Point the engine at a different RDM GPS block base (four pucks can share a
 * bus at 0x400/0x410/0x420/0x430). Persisted. */
void lap_engine_set_base_id(uint16_t base_id);
uint16_t lap_engine_get_base_id(void);

/* Activate the Position & GPS canonical channels and give each one the CAN
 * decode for the current base ID, so lat/lon/speed/heading/altitude/accuracy/
 * fix/satellites light up on the bus with no DBC import and no manual bit
 * fiddling. Returns the number of channels bound. */
int lap_engine_bind_gps_channels(void);

/* True if RDM GPS status frames have arrived recently. This is what a device
 * tree in the desktop suite, or a "GPS" indicator on screen, should ask. */
bool lap_engine_gps_present(void);

/* The most recent raw position and course from the puck, in full double
 * precision — the values the line-capture flow must use. Reading the
 * gps_latitude/gps_longitude CHANNELS here would quantise the captured line
 * by ~1.7 m (float); a line offset by that much biases every lap's crossing
 * the same way, but the capture should still be as good as the data allows.
 * Returns false until a usable fix has been decoded. */
bool lap_engine_last_position(double *lat_deg, double *lon_deg, float *heading_deg,
                              float *speed_kph);

/* Track configuration. Setting a track clears session timing (a best lap from
 * a different circuit is worse than no best lap). */
esp_err_t lap_engine_set_track(const lap_track_t *track);
const lap_track_t *lap_engine_get_track(void);
bool lap_engine_has_track(void);

/* Clear lap/sector/best times and the predictive reference, keeping the track.
 * This is "new session" — a dry reference is the wrong target in the wet. */
void lap_engine_reset_session(void);

/* Read-only view of engine state, for web endpoints and diagnostics. */
const lap_engine_t *lap_engine_state(void);

#ifdef __cplusplus
}
#endif
