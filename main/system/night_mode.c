/* night_mode.c — see header for rationale.
 *
 * Implementation notes:
 *  - Subscribers live in a fixed-size table (no malloc on the LVGL task).
 *  - Setter posts to LVGL task via lv_async_call when called from non-LVGL
 *    contexts; subscribers always run on the LVGL task so they can touch
 *    widget LVGL objects without locking.
 *  - The internal `__NIGHT_MODE` signal is registered lazily on first
 *    night_mode_init() call so signal-bound layout configs can subscribe to it.
 */

#include "night_mode.h"

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "night_mode";

typedef struct {
    night_mode_change_cb_t cb;
    void                   *user_data;
} subscriber_t;

static subscriber_t s_subscribers[NIGHT_MODE_MAX_SUBSCRIBERS];
static uint8_t      s_subscriber_count = 0;
static bool         s_active           = false;
static bool         s_initialised      = false;

void night_mode_init(void)
{
    if (s_initialised) return;
    s_initialised = true;
    ESP_LOGI(TAG, "Initialised (active=%d, %u subscriber slots)",
             (int)s_active, (unsigned)NIGHT_MODE_MAX_SUBSCRIBERS);
}

bool night_mode_is_active(void)
{
    return s_active;
}

bool night_mode_subscribe(night_mode_change_cb_t cb, void *user_data)
{
    if (!cb) return false;
    if (s_subscriber_count >= NIGHT_MODE_MAX_SUBSCRIBERS) {
        ESP_LOGW(TAG, "Subscriber table full (%u) — refusing subscribe",
                 (unsigned)NIGHT_MODE_MAX_SUBSCRIBERS);
        return false;
    }
    s_subscribers[s_subscriber_count].cb        = cb;
    s_subscribers[s_subscriber_count].user_data = user_data;
    s_subscriber_count++;
    return true;
}

void night_mode_unsubscribe(night_mode_change_cb_t cb, void *user_data)
{
    if (!cb) return;
    for (uint8_t i = 0; i < s_subscriber_count; i++) {
        if (s_subscribers[i].cb == cb && s_subscribers[i].user_data == user_data) {
            /* Compact the table by shifting subsequent entries down */
            for (uint8_t j = i; j < s_subscriber_count - 1; j++) {
                s_subscribers[j] = s_subscribers[j + 1];
            }
            s_subscriber_count--;
            return;
        }
    }
}

void night_mode_clear_subscribers(void)
{
    s_subscriber_count = 0;
}

/* Helper: actual notification work — runs on LVGL task. */
static void _do_notify(void *param)
{
    bool active = (param != NULL);
    for (uint8_t i = 0; i < s_subscriber_count; i++) {
        if (s_subscribers[i].cb) {
            s_subscribers[i].cb(active, s_subscribers[i].user_data);
        }
    }
}

void night_mode_set_active(bool active)
{
    if (!s_initialised) {
        ESP_LOGW(TAG, "set_active before init — ignored");
        return;
    }
    if (s_active == active) return;
    s_active = active;
    ESP_LOGI(TAG, "Night mode → %s", active ? "ACTIVE" : "OFF");

    /* Defer notification to LVGL task so subscribers can safely touch widgets
     * regardless of who set the state (CAN task, settings UI, etc.). */
    lv_async_call(_do_notify, active ? (void *)1 : NULL);
}
