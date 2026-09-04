/* esp_timer.h stub — a monotonic microsecond clock off the host's own. */
#ifndef RDM_TEST_MOCK_ESP_TIMER_H
#define RDM_TEST_MOCK_ESP_TIMER_H

#include <stdint.h>
#include <time.h>

static inline int64_t esp_timer_get_time(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

#endif
