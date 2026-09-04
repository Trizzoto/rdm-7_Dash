/* FreeRTOS.h stub — enough for sources that only sleep and spawn. The player
 * task itself is not run natively; the tests cover parsing, storing and the
 * status it reports, which is where the file format lives. */
#ifndef RDM_TEST_MOCK_FREERTOS_H
#define RDM_TEST_MOCK_FREERTOS_H

#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef void *TaskHandle_t;

#define pdPASS 1
#define pdFAIL 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#endif
