#include "widget_registry.h"
#include <string.h>

static widget_t *s_widgets[WIDGET_REGISTRY_MAX];
static uint8_t s_count = 0;

void widget_registry_reset(void) {
	for (uint8_t i = 0; i < WIDGET_REGISTRY_MAX; i++) {
		if (s_widgets[i]) {
			/* Destroy frees type_data and the widget itself */
			if (s_widgets[i]->destroy)
				s_widgets[i]->destroy(s_widgets[i]);
		}
		s_widgets[i] = NULL;
	}
	s_count = 0;
}

bool widget_registry_add(widget_t *w) {
	if (!w)
		return false;
	if (s_count >= WIDGET_REGISTRY_MAX)
		return false;
	s_widgets[s_count++] = w;
	return true;
}

bool widget_registry_remove(widget_t *w) {
	if (!w)
		return false;
	for (uint8_t i = 0; i < s_count; i++) {
		if (s_widgets[i] != w)
			continue;
		/* Shift everyone after this slot down by one. */
		for (uint8_t j = i; j < s_count - 1; j++)
			s_widgets[j] = s_widgets[j + 1];
		s_widgets[--s_count] = NULL;
		return true;
	}
	return false;
}

uint8_t widget_registry_count(void) { return s_count; }

void widget_registry_snapshot(widget_t **out, uint8_t max, uint8_t *out_count) {
	if (!out || !out_count) {
		return;
	}

	uint8_t n = s_count;
	if (n > max)
		n = max;

	for (uint8_t i = 0; i < n; i++) {
		out[i] = s_widgets[i];
	}
	*out_count = n;
}

widget_t *widget_registry_find_by_id(const char *id) {
	if (!id)
		return NULL;
	for (uint8_t i = 0; i < s_count; i++) {
		if (!s_widgets[i])
			continue;
		if (strcmp(s_widgets[i]->id, id) == 0)
			return s_widgets[i];
	}
	return NULL;
}

widget_t *widget_registry_find_by_type_and_slot(widget_type_t type, uint8_t slot) {
	for (uint8_t i = 0; i < s_count; i++) {
		if (!s_widgets[i] || s_widgets[i]->type != type)
			continue;
		if (s_widgets[i]->slot == slot)
			return s_widgets[i];
	}
	return NULL;
}
