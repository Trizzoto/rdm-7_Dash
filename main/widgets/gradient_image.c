/**
 * gradient_image.c — see header.
 *
 * Bake strategy: for each column x in [0, w), compute t = x / (w-1)
 * and sample the gradient stops. Replicate the column down all h rows.
 * RGB565 is the firmware's native colour format so no conversion is
 * needed when LVGL renders the image — straight memcpy at draw time.
 *
 * No alpha channel: the bg_img sits on PART_INDICATOR which is opaque
 * anyway, and dropping alpha halves the buffer size.
 */
#include "gradient_image.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "grad_img";

lv_img_dsc_t *gradient_image_create(uint16_t w, uint16_t h,
                                    const gradient_stops_t *stops)
{
    if (!stops || stops->count < 2 || w == 0 || h == 0) return NULL;

    size_t px_bytes = (size_t)w * (size_t)h * 2;
    uint8_t *buf = heap_caps_malloc(px_bytes, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "PSRAM alloc failed for %ux%u grad (%u bytes)",
                 w, h, (unsigned)px_bytes);
        return NULL;
    }
    lv_img_dsc_t *dsc = heap_caps_calloc(1, sizeof(lv_img_dsc_t),
                                          MALLOC_CAP_SPIRAM);
    if (!dsc) {
        heap_caps_free(buf);
        return NULL;
    }

    /* Bake the first row, then memcpy down. The first row is the
     * expensive part (per-column lerp); copying it h-1 times is
     * memcpy speed which is far cheaper than re-sampling. */
    uint16_t *row0 = (uint16_t *)buf;
    if (w == 1) {
        row0[0] = gradient_stops_sample(stops, 0.0f);
    } else {
        float inv = 1.0f / (float)(w - 1);
        for (uint16_t x = 0; x < w; x++) {
            row0[x] = gradient_stops_sample(stops, (float)x * inv);
        }
    }
    for (uint16_t y = 1; y < h; y++) {
        memcpy(buf + y * (size_t)w * 2, row0, (size_t)w * 2);
    }

    dsc->header.always_zero = 0;
    dsc->header.w           = w;
    dsc->header.h           = h;
    dsc->header.cf          = LV_IMG_CF_TRUE_COLOR;
    dsc->data_size          = px_bytes;
    dsc->data               = buf;
    return dsc;
}

void gradient_image_free(lv_img_dsc_t *dsc)
{
    if (!dsc) return;
    if (dsc->data) heap_caps_free((void *)dsc->data);
    heap_caps_free(dsc);
}
