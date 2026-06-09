/*
 * rdm_lv_async.c — see rdm_lv_async.h.
 *
 * Threading: all functions run on the LVGL task (they touch LVGL objects +
 * the async queue), same as lv_obj_del_async().
 */
#include "system/rdm_lv_async.h"

static void _cancel_on_delete_cb(lv_event_t *e);   /* fwd decl */

/* The deferred delete itself. lv_obj_is_valid() guards the (benign) case where
 * the object was already deleted AND its slot not yet reused; the real
 * protection against reuse is the cancel-on-delete handler below, which removes
 * this call from the async queue the instant the object is freed by any path. */
static void _del_async_cb(void *arg)
{
    lv_obj_t *o = (lv_obj_t *)arg;
    if (o && lv_obj_is_valid(o)) {
        /* CRITICAL: detach the cancel-on-delete handler BEFORE deleting.
         *
         * lv_obj_del() below fires LV_EVENT_DELETE, which would run
         * _cancel_on_delete_cb → lv_async_call_cancel(_del_async_cb, o).
         * But WE are _del_async_cb, executing right now from lv_async_timer_cb.
         * lv_async_call_cancel() matches the still-listed current timer,
         * deletes it, and frees its lv_async_info — then lv_async_timer_cb
         * returns here and frees the SAME timer/info again → double-free
         * ("CORRUPT HEAP: Bad head" at the next lv_mem_free).
         *
         * Removing our own handler first means this self-triggered delete
         * won't cancel the in-flight call. The handler is only needed to
         * protect against OTHER deletion paths firing while our call is still
         * QUEUED (not executing), and by the time we're here that race is over. */
        lv_obj_remove_event_cb(o, _cancel_on_delete_cb);
        lv_obj_del(o);
    }
}

/* Fires from lv_obj_del() (LV_EVENT_DELETE) no matter who triggers the delete —
 * our own _del_async_cb, a layout reload cleaning the screen / lv_layer_top, a
 * parent deletion, etc. Cancelling here guarantees the queued _del_async_cb can
 * never run on this (about-to-be-freed) pointer. */
static void _cancel_on_delete_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_async_call_cancel(_del_async_cb, o);
}

void rdm_obj_del_async(lv_obj_t *obj)
{
    if (!obj || !lv_obj_is_valid(obj)) return;

    /* Attach the cancel handler exactly once (remove any prior copy first so
     * repeated calls don't stack duplicates). */
    lv_obj_remove_event_cb(obj, _cancel_on_delete_cb);
    lv_obj_add_event_cb(obj, _cancel_on_delete_cb, LV_EVENT_DELETE, NULL);

    /* De-dupe any already-queued delete for this object, then queue one. */
    lv_async_call_cancel(_del_async_cb, obj);
    lv_async_call(_del_async_cb, obj);
}
