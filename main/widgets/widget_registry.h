/**
 * widget_registry.h — central widget_t registry.
 *
 * Tracks all widget_t instances created from layout JSON so that other
 * subsystems (dashboard, future editors, etc.) can enumerate and look up
 * widgets by id.
 */
#pragma once

#include "widget_types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of widgets the registry will track at once. */
#define WIDGET_REGISTRY_MAX 32

/** Reset the registry, dropping all tracked pointers. */
void widget_registry_reset(void);

/**
 * Register a widget instance.
 *
 * The registry does not take ownership of @p w; it simply stores the pointer.
 * Returns true on success, or false if the registry is full.
 */
bool widget_registry_add(widget_t *w);

/** Return the current number of registered widgets. */
uint8_t widget_registry_count(void);

/**
 * Snapshot the registry contents into @p out (up to @p max entries).
 * The pointers are copied as-is; the caller must not free them.
 *
 * On return, *@p out_count holds the number of entries written.
 */
void widget_registry_snapshot(widget_t **out, uint8_t max, uint8_t *out_count);

/**
 * Lookup a widget by its id string.  Returns NULL if not found.
 */
widget_t *widget_registry_find_by_id(const char *id);

/**
 * Lookup a widget by type and slot index.  Returns NULL if not found.
 * For singleton widgets (RPM) the slot is ignored.
 */
widget_t *widget_registry_find_by_type_and_slot(widget_type_t type, uint8_t slot);

#ifdef __cplusplus
}
#endif

