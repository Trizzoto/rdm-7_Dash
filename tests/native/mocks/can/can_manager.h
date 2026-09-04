/* can_manager.h stub — records what was transmitted instead of transmitting,
 * and lets a test say what bitrate the dash is on. */
#ifndef RDM_TEST_MOCK_CAN_MANAGER_H
#define RDM_TEST_MOCK_CAN_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t id; uint8_t data[8]; uint8_t dlc; } mock_can_tx_t;

extern mock_can_tx_t mock_can_tx[256];
extern int mock_can_tx_count;
extern uint8_t mock_can_bitrate_index;
extern bool mock_can_suspended;

static inline esp_err_t can_transmit_frame(uint32_t id, const uint8_t *d, uint8_t dlc) {
	if (mock_can_tx_count < 256) {
		mock_can_tx[mock_can_tx_count].id = id;
		mock_can_tx[mock_can_tx_count].dlc = dlc;
		for (int i = 0; i < 8; i++) mock_can_tx[mock_can_tx_count].data[i] = d ? d[i] : 0;
		mock_can_tx_count++;
	}
	return ESP_OK;
}
static inline uint8_t can_get_bitrate_index(void) { return mock_can_bitrate_index; }
static inline bool can_is_suspended(void) { return mock_can_suspended; }

#endif
