/* task.h stub — the test never starts the player, so these do nothing. */
#ifndef RDM_TEST_MOCK_TASK_H
#define RDM_TEST_MOCK_TASK_H

#include "freertos/FreeRTOS.h"

typedef void (*TaskFunction_t)(void *);

static inline void vTaskDelay(TickType_t t) { (void)t; }
static inline void vTaskDelete(TaskHandle_t h) { (void)h; }
static inline BaseType_t xTaskCreate(TaskFunction_t fn, const char *name,
                                     uint32_t stack, void *arg,
                                     unsigned prio, TaskHandle_t *out) {
	(void)fn; (void)name; (void)stack; (void)arg; (void)prio;
	if (out) *out = (TaskHandle_t)1;
	return pdPASS;
}

#endif
