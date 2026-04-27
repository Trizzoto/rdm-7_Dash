/* esp_heap_caps.h stub for native-host tests — routes heap_caps_*
 * allocators through libc. The capability flags are no-ops here.
 *
 * widget_rules.c uses plain calloc/free directly (not heap_caps_calloc),
 * but this stub is provided so any firmware module that expects the
 * ESP-IDF heap API can compile against it without modification. */
#ifndef RDM_TEST_MOCK_ESP_HEAP_CAPS_H
#define RDM_TEST_MOCK_ESP_HEAP_CAPS_H

#include <stdlib.h>

#define MALLOC_CAP_8BIT     0
#define MALLOC_CAP_DMA      0
#define MALLOC_CAP_SPIRAM   0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_DEFAULT  0

static inline void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    return calloc(n, size);
}

static inline void *heap_caps_realloc(void *p, size_t size, uint32_t caps) {
    (void)caps;
    return realloc(p, size);
}

static inline void heap_caps_free(void *p) {
    free(p);
}

#endif /* RDM_TEST_MOCK_ESP_HEAP_CAPS_H */
