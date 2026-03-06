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
