#pragma once

/* Lap engine — CAN decode only.
 *
 * The dash does not time laps. It reads the numbers the RDM GPS puck already
 * computed and broadcasts, and it decodes the puck's raw position/motion for
 * the Position & GPS telemetry channels. Track configuration (the start/finish
 * line, sectors) is authored on the puck itself — today over USB from RDM
 * Studio's RDM GPS workspace, later from a phone app — never on the dash.
 * See ADR-0008's addendum, 2026-07-30.
 *
 * Threading: every function here runs on the LVGL task. lap_engine_on_can_frame
 * is called from can_process_queued_frames(), which already holds the LVGL
 * mutex, and the channel/signal APIs require it. */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default RDM GPS block base. The puck's frames are base+0 status, base+1
 * position, base+2 motion, base+3 quality, base+5/6 IMU, base+7/8/9
 * lap/sector/delta — see the RDM GPS PROTOCOL.md, which allocates
 * 0x400-0x4FF to RDM bus devices. */
#define LAP_GPS_BASE_ID_DEFAULT 0x400u

/* Activate the lap + GPS channels and register their feeding signals.
 * Idempotent. Call once during boot, before dashboard_init(). */
esp_err_t lap_engine_init(void);

/* Begin/refresh operation. Idempotent and safe to call on every layout load,
 * matching channel_math_start(). */
void lap_engine_start(void);

/* Feed one CAN frame. Cheap and non-matching frames return immediately — this
 * sits in the hot path for every frame on the bus. `extd` frames never match:
 * a 29-bit id numerically inside the GPS block is somebody else's traffic. */
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

#ifdef __cplusplus
}
#endif
