/**
 * config_bridge.c — Bridge between value_id-based config modal and
 * widget type_data + signal registry.
 */

#include "config_bridge.h"
#include "ui/screens/ui_Screen3.h"
#include "widgets/signal.h"
#include "widgets/widget_types.h"
#include "widgets/widget_panel.h"
#include "widgets/widget_bar.h"
#include "widgets/widget_rpm_bar.h"
#include "widgets/widget_text.h"
#include "widgets/widget_registry.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "cfg_bridge";

/* ── Internal helpers ──────────────────────────────────────────────────── */

widget_t *config_bridge_get_widget(uint8_t value_id) {
	if (value_id < 1 || value_id > 11) return NULL;
	if (value_id <= 8)  return widget_registry_find_by_type_and_slot(WIDGET_PANEL, value_id - 1);
	if (value_id == 9)  return widget_registry_find_by_type_and_slot(WIDGET_RPM_BAR, 0);
	if (value_id == 10) return widget_registry_find_by_type_and_slot(WIDGET_BAR, 0);
	if (value_id == 11) return widget_registry_find_by_type_and_slot(WIDGET_BAR, 1);
	return NULL;
}

/** Get signal_name pointer from widget type_data (all types have it at known offset) */
static const char *_get_signal_name(widget_t *w) {
	if (!w || !w->type_data) return "";
	switch (w->type) {
		case WIDGET_PANEL:   return ((panel_data_t *)w->type_data)->signal_name;
		case WIDGET_BAR:     return ((bar_data_t *)w->type_data)->signal_name;
		case WIDGET_RPM_BAR: return ((rpm_bar_data_t *)w->type_data)->signal_name;
		case WIDGET_TEXT:    return ((text_data_t *)w->type_data)->signal_name;
		default: return "";
	}
}

static int16_t *_get_signal_index_ptr(widget_t *w) {
	if (!w || !w->type_data) return NULL;
	switch (w->type) {
		case WIDGET_PANEL:   return &((panel_data_t *)w->type_data)->signal_index;
		case WIDGET_BAR:     return &((bar_data_t *)w->type_data)->signal_index;
		case WIDGET_RPM_BAR: return &((rpm_bar_data_t *)w->type_data)->signal_index;
		case WIDGET_TEXT:    return &((text_data_t *)w->type_data)->signal_index;
		default: return NULL;
	}
}

static char *_get_signal_name_buf(widget_t *w) {
	if (!w || !w->type_data) return NULL;
	switch (w->type) {
		case WIDGET_PANEL:   return ((panel_data_t *)w->type_data)->signal_name;
		case WIDGET_BAR:     return ((bar_data_t *)w->type_data)->signal_name;
		case WIDGET_RPM_BAR: return ((rpm_bar_data_t *)w->type_data)->signal_name;
		case WIDGET_TEXT:    return ((text_data_t *)w->type_data)->signal_name;
		default: return NULL;
	}
}

/* ── Signal lookup ─────────────────────────────────────────────────────── */

signal_t *config_bridge_get_signal(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w) return NULL;
	const char *name = _get_signal_name(w);
	if (!name || name[0] == '\0') return NULL;
	int16_t idx = signal_find_by_name(name);
	if (idx < 0) return NULL;
	return signal_get_by_index((uint16_t)idx);
}

signal_t *config_bridge_ensure_signal(uint8_t value_id) {
	signal_t *sig = config_bridge_get_signal(value_id);
	if (sig) return sig;

	/* Auto-create a signal for this value_id */
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w) return NULL;

	char name[32];
	snprintf(name, sizeof(name), "%s_sig", w->id);

	int16_t idx = signal_register(name, 0, 0, 16, 1.0f, 0.0f, false, 1);
	if (idx < 0) {
		ESP_LOGE(TAG, "Failed to register signal for value_id %d", value_id);
		return NULL;
	}

	/* Bind signal to widget */
	char *name_buf = _get_signal_name_buf(w);
	int16_t *idx_ptr = _get_signal_index_ptr(w);
	if (name_buf && idx_ptr) {
		strncpy(name_buf, name, 31);
		name_buf[31] = '\0';
		*idx_ptr = idx;
	}

	ESP_LOGI(TAG, "Auto-created signal '%s' (idx %d) for value_id %d", name, idx, value_id);
	return signal_get_by_index((uint16_t)idx);
}

/* ── CAN field readers ─────────────────────────────────────────────────── */

uint32_t config_bridge_get_can_id(uint8_t value_id) {
	signal_t *s = config_bridge_get_signal(value_id);
	return s ? s->can_id : 0;
}

uint8_t config_bridge_get_bit_start(uint8_t value_id) {
	signal_t *s = config_bridge_get_signal(value_id);
	return s ? s->bit_start : 0;
}

uint8_t config_bridge_get_bit_length(uint8_t value_id) {
	signal_t *s = config_bridge_get_signal(value_id);
	return s ? s->bit_length : 16;
}

float config_bridge_get_scale(uint8_t value_id) {
	signal_t *s = config_bridge_get_signal(value_id);
	return s ? s->scale : 1.0f;
}

float config_bridge_get_offset(uint8_t value_id) {
	signal_t *s = config_bridge_get_signal(value_id);
	return s ? s->offset : 0.0f;
}

uint8_t config_bridge_get_endian(uint8_t value_id) {
	signal_t *s = config_bridge_get_signal(value_id);
	return s ? s->endian : 1;
}

bool config_bridge_get_is_signed(uint8_t value_id) {
	signal_t *s = config_bridge_get_signal(value_id);
	return s ? s->is_signed : false;
}

/* ── CAN field writers ─────────────────────────────────────────────────── */

void config_bridge_set_can_id(uint8_t value_id, uint32_t can_id) {
	signal_t *s = config_bridge_ensure_signal(value_id);
	if (s) s->can_id = can_id;
}

void config_bridge_set_bit_start(uint8_t value_id, uint8_t bit_start) {
	signal_t *s = config_bridge_ensure_signal(value_id);
	if (s) s->bit_start = bit_start;
}

void config_bridge_set_bit_length(uint8_t value_id, uint8_t bit_length) {
	signal_t *s = config_bridge_ensure_signal(value_id);
	if (s) s->bit_length = bit_length;
}

void config_bridge_set_scale(uint8_t value_id, float scale) {
	signal_t *s = config_bridge_ensure_signal(value_id);
	if (s) s->scale = scale;
}

void config_bridge_set_offset(uint8_t value_id, float offset) {
	signal_t *s = config_bridge_ensure_signal(value_id);
	if (s) s->offset = offset;
}

void config_bridge_set_endian(uint8_t value_id, uint8_t endian) {
	signal_t *s = config_bridge_ensure_signal(value_id);
	if (s) s->endian = endian;
}

void config_bridge_set_is_signed(uint8_t value_id, bool is_signed) {
	signal_t *s = config_bridge_ensure_signal(value_id);
	if (s) s->is_signed = is_signed;
}

/* ── Display field accessors ───────────────────────────────────────────── */

const char *config_bridge_get_label(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data) return "";
	if (w->type == WIDGET_PANEL)
		return ((panel_data_t *)w->type_data)->label;
	return "";
}

void config_bridge_set_label(uint8_t value_id, const char *label) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || !label) return;
	if (w->type == WIDGET_PANEL) {
		panel_data_t *pd = (panel_data_t *)w->type_data;
		strncpy(pd->label, label, sizeof(pd->label) - 1);
		pd->label[sizeof(pd->label) - 1] = '\0';
	}
}

uint8_t config_bridge_get_decimals(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data) return 0;
	switch (w->type) {
		case WIDGET_PANEL: return ((panel_data_t *)w->type_data)->decimals;
		case WIDGET_BAR:   return ((bar_data_t *)w->type_data)->decimals;
		default: return 0;
	}
}

void config_bridge_set_decimals(uint8_t value_id, uint8_t decimals) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data) return;
	switch (w->type) {
		case WIDGET_PANEL: ((panel_data_t *)w->type_data)->decimals = decimals; break;
		case WIDGET_BAR:   ((bar_data_t *)w->type_data)->decimals = decimals; break;
		default: break;
	}
}

const char *config_bridge_get_custom_text(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data) return "";
	if (w->type == WIDGET_PANEL)
		return ((panel_data_t *)w->type_data)->custom_text;
	return "";
}

void config_bridge_set_custom_text(uint8_t value_id, const char *text) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || !text) return;
	if (w->type == WIDGET_PANEL) {
		panel_data_t *pd = (panel_data_t *)w->type_data;
		strncpy(pd->custom_text, text, sizeof(pd->custom_text) - 1);
		pd->custom_text[sizeof(pd->custom_text) - 1] = '\0';
	}
}

/* ── Bar-specific ──────────────────────────────────────────────────────── */

#define BAR_GETTER(field, defval) \
	widget_t *w = config_bridge_get_widget(value_id); \
	if (!w || !w->type_data || w->type != WIDGET_BAR) return defval; \
	return ((bar_data_t *)w->type_data)->field;

#define BAR_SETTER(field) \
	widget_t *w = config_bridge_get_widget(value_id); \
	if (!w || !w->type_data || w->type != WIDGET_BAR) return; \
	((bar_data_t *)w->type_data)->field = val;

int32_t config_bridge_get_bar_min(uint8_t value_id) { BAR_GETTER(bar_min, 0) }
int32_t config_bridge_get_bar_max(uint8_t value_id) { BAR_GETTER(bar_max, 100) }
bool    config_bridge_get_show_bar_value(uint8_t value_id) { BAR_GETTER(show_bar_value, false) }
bool    config_bridge_get_invert_bar_value(uint8_t value_id) { BAR_GETTER(invert_bar_value, false) }

void config_bridge_set_bar_min(uint8_t value_id, int32_t val) { BAR_SETTER(bar_min) }
void config_bridge_set_bar_max(uint8_t value_id, int32_t val) { BAR_SETTER(bar_max) }
void config_bridge_set_show_bar_value(uint8_t value_id, bool val) { BAR_SETTER(show_bar_value) }
void config_bridge_set_invert_bar_value(uint8_t value_id, bool val) { BAR_SETTER(invert_bar_value) }

#undef BAR_GETTER
#undef BAR_SETTER

/* ── Panel alert-specific ──────────────────────────────────────────────── */

float config_bridge_get_warning_high_threshold(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_PANEL) return 0;
	return ((panel_data_t *)w->type_data)->warning_high_threshold;
}

float config_bridge_get_warning_low_threshold(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_PANEL) return 0;
	return ((panel_data_t *)w->type_data)->warning_low_threshold;
}

lv_color_t config_bridge_get_warning_high_color(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_PANEL) return lv_color_hex(0xFF0000);
	return ((panel_data_t *)w->type_data)->warning_high_color;
}

lv_color_t config_bridge_get_warning_low_color(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_PANEL) return lv_color_hex(0x0000FF);
	return ((panel_data_t *)w->type_data)->warning_low_color;
}

bool config_bridge_get_warning_high_enabled(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_PANEL) return false;
	return ((panel_data_t *)w->type_data)->warning_high_enabled;
}

bool config_bridge_get_warning_low_enabled(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_PANEL) return false;
	return ((panel_data_t *)w->type_data)->warning_low_enabled;
}

void config_bridge_set_warning_high_threshold(uint8_t value_id, float val) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_PANEL) return;
	((panel_data_t *)w->type_data)->warning_high_threshold = val;
	((panel_data_t *)w->type_data)->warning_high_enabled = true;
}

void config_bridge_set_warning_low_threshold(uint8_t value_id, float val) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_PANEL) return;
	((panel_data_t *)w->type_data)->warning_low_threshold = val;
	((panel_data_t *)w->type_data)->warning_low_enabled = true;
}

void config_bridge_set_warning_high_color(uint8_t value_id, lv_color_t color) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_PANEL) return;
	((panel_data_t *)w->type_data)->warning_high_color = color;
}

void config_bridge_set_warning_low_color(uint8_t value_id, lv_color_t color) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_PANEL) return;
	((panel_data_t *)w->type_data)->warning_low_color = color;
}

/* ── Bar alert-specific ────────────────────────────────────────────────── */

int32_t config_bridge_get_bar_low(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_BAR) return 0;
	return ((bar_data_t *)w->type_data)->bar_low;
}

int32_t config_bridge_get_bar_high(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_BAR) return 0;
	return ((bar_data_t *)w->type_data)->bar_high;
}

lv_color_t config_bridge_get_bar_low_color(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_BAR) return lv_color_hex(0x0000FF);
	return ((bar_data_t *)w->type_data)->bar_low_color;
}

lv_color_t config_bridge_get_bar_high_color(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_BAR) return lv_color_hex(0xFF0000);
	return ((bar_data_t *)w->type_data)->bar_high_color;
}

lv_color_t config_bridge_get_bar_in_range_color(uint8_t value_id) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_BAR) return lv_color_hex(0x00FF00);
	return ((bar_data_t *)w->type_data)->bar_in_range_color;
}

void config_bridge_set_bar_low(uint8_t value_id, int32_t val) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_BAR) return;
	((bar_data_t *)w->type_data)->bar_low = val;
}

void config_bridge_set_bar_high(uint8_t value_id, int32_t val) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_BAR) return;
	((bar_data_t *)w->type_data)->bar_high = val;
}

void config_bridge_set_bar_low_color(uint8_t value_id, lv_color_t color) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_BAR) return;
	((bar_data_t *)w->type_data)->bar_low_color = color;
}

void config_bridge_set_bar_high_color(uint8_t value_id, lv_color_t color) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_BAR) return;
	((bar_data_t *)w->type_data)->bar_high_color = color;
}

void config_bridge_set_bar_in_range_color(uint8_t value_id, lv_color_t color) {
	widget_t *w = config_bridge_get_widget(value_id);
	if (!w || !w->type_data || w->type != WIDGET_BAR) return;
	((bar_data_t *)w->type_data)->bar_in_range_color = color;
}

/* ── RPM-specific ──────────────────────────────────────────────────────── */

static rpm_bar_data_t *_get_rpm_data(void) {
	widget_t *w = config_bridge_get_widget(RPM_VALUE_ID);
	if (!w || !w->type_data || w->type != WIDGET_RPM_BAR) return NULL;
	return (rpm_bar_data_t *)w->type_data;
}

lv_color_t config_bridge_get_rpm_bar_color(void) {
	rpm_bar_data_t *rd = _get_rpm_data();
	return rd ? rd->bar_color : lv_color_hex(0x00FF00);
}

uint8_t config_bridge_get_rpm_limiter_effect(void) {
	rpm_bar_data_t *rd = _get_rpm_data();
	return rd ? rd->limiter_effect : 0;
}

int32_t config_bridge_get_rpm_limiter_value(void) {
	rpm_bar_data_t *rd = _get_rpm_data();
	return rd ? rd->limiter_value : 7500;
}

lv_color_t config_bridge_get_rpm_limiter_color(void) {
	rpm_bar_data_t *rd = _get_rpm_data();
	return rd ? rd->limiter_color : lv_color_hex(0xFF0000);
}

bool config_bridge_get_rpm_background_enabled(void) {
	rpm_bar_data_t *rd = _get_rpm_data();
	return rd ? rd->background_enabled : false;
}

int32_t config_bridge_get_rpm_background_value(void) {
	rpm_bar_data_t *rd = _get_rpm_data();
	return rd ? rd->background_value : 0;
}

lv_color_t config_bridge_get_rpm_background_color(void) {
	rpm_bar_data_t *rd = _get_rpm_data();
	return rd ? rd->background_color : lv_color_hex(0x000000);
}

