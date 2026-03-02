/*
 * can_dispatch.h — CAN ID → widget-index dispatch table.
 *
 * Owns the O(1) lookup structures that map a received CAN ID to the set of
 * warning lights, indicator lights, and value widgets that depend on it.
 */
#pragma once
#include "driver/twai.h"
#include <stdint.h>

/* Maximum distinct CAN IDs tracked simultaneously */
#define CAN_DISPATCH_MAX_ENTRIES  32
/* Standard 11-bit CAN ID range */
#define CAN_DISPATCH_ID_TABLE_SZ  2048

/**
 * One entry in the dispatch table: all widget indices subscribed to a
 * single CAN ID.
 */
typedef struct {
    uint32_t can_id;
    uint8_t  num_warning;
    uint8_t  warning_indices[8];
    uint8_t  num_indicator;
    uint8_t  indicator_indices[2];
    uint8_t  num_values;
    uint8_t  value_indices[13];
} can_dispatch_entry_t;

/* Dispatch table storage — accessed directly by process_can_message()
 * in ui_Screen3.c via this header.                                    */
extern can_dispatch_entry_t can_dispatch_entries[CAN_DISPATCH_MAX_ENTRIES];
extern int                  can_dispatch_count;
extern int16_t              can_id_to_dispatch_index[CAN_DISPATCH_ID_TABLE_SZ];

/**
 * Rebuild the dispatch table from the current warning / indicator / value
 * configs.  Call after any config change and before the next CAN frame.
 */
void rebuild_can_dispatch(void);

/**
 * Process one received CAN frame: decode bit fields, route to widgets,
 * and update LVGL objects.  Must be called with the LVGL mutex held.
 */
void can_dispatch_process_frame(const twai_message_t *msg);
