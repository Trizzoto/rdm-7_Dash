/*
 * rdm_lv_async.h — crash-safe deferred LVGL object deletion.
 *
 * lv_obj_del_async() schedules lv_obj_del() on the next LVGL timer tick. That
 * is the correct way to delete an object from WITHIN one of its own (or its
 * child's) event handlers — you can't free it synchronously mid-event. But it
 * has a fatal flaw: if the object is freed by ANY OTHER path before the queued
 * delete fires (e.g. a layout reload wiping the screen / lv_layer_top, or a
 * second close), the async delete then runs lv_obj_del() on freed memory →
 * use-after-free (it walks the parent chain / sends LV_EVENT_DELETE through a
 * dangling callback list → LoadProhibited / InstrFetchProhibited panic).
 *
 * LVGL's internal lv_obj_del_async_cb is static, so the queued call can't be
 * cancelled. rdm_obj_del_async() uses a cancellable callback PLUS an
 * LV_EVENT_DELETE handler on the target: whoever frees the object first cancels
 * the pending delete, so it can never touch freed memory. Drop-in replacement
 * for lv_obj_del_async().
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Crash-safe lv_obj_del_async() — see file header. No-op on NULL/invalid. */
void rdm_obj_del_async(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
