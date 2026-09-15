/*
 * can_emu_settings.c — the Keypads & IO boxes popup. See can_emu_settings.h.
 *
 * Two views in one kit popup: the list of devices (with an on/off switch each
 * and the built-in templates to add), and one device (hold its buttons to
 * test them, see the bytes it is sending and what the ECU sends back).
 *
 * The model can be swapped under us at any time — a save from the web editor
 * applies on this same task between two refreshes — so nothing here keeps a
 * pointer into it across a refresh: every tick looks the device up again, and
 * a new can_emu_generation() rebuilds the view.
 *
 * Body text is Montserrat, which has no "·" or "—": plain hyphens only.
 */
#include "can_emu_settings.h"

#include "can/can_emu.h"
#include "can/can_emu_core.h"
#include "widgets/signal.h"

#include "cJSON.h"
#include "esp_attr.h"
#include "lvgl.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define POP_W       720
#define POP_H       440
#define MAX_REFS    24
#define MAX_ADDS    8
#define REF_LEN     (CEMU_ID_LEN * 2 + CEMU_STATE_LEN + 4)

typedef struct {
	lv_obj_t *popup, *body;
	lv_timer_t *timer;
	int view;                                   /* 0 = list, 1 = one device */
	char dev_id[CEMU_ID_LEN];
	uint32_t gen;

	lv_obj_t *list_status[CEMU_MAX_DEVICES];
	lv_obj_t *list_sub[CEMU_MAX_DEVICES];
	lv_obj_t *list_sw[CEMU_MAX_DEVICES];

	lv_obj_t *d_status, *d_reason, *d_sw, *d_remove;
	lv_obj_t *frame_val[CEMU_MAX_SEND];
	lv_obj_t *chan_val[CEMU_MAX_LISTEN * CEMU_MAX_CHANNELS];
	bool remove_armed;

	char     refs[MAX_REFS][REF_LEN];
	lv_obj_t *ref_btn[MAX_REFS];
	int8_t   ref_ctl[MAX_REFS];
	bool     held[MAX_REFS];
	int      n_refs;

	char add_tpl[MAX_ADDS][24];
	char add_var[MAX_ADDS][8];
	int  n_adds;
} ui_t;

static EXT_RAM_BSS_ATTR ui_t S;

static void _build(void);

/* ── model lookups ─────────────────────────────────────────────────────── */

static const cemu_device_t *_dev(int *index) {
	const cemu_model_t *m = (const cemu_model_t *)can_emu_model();
	for (int d = 0; m && d < m->n_devices; d++)
		if (!strcmp(m->devices[d].id, S.dev_id)) {
			if (index) *index = d;
			return &m->devices[d];
		}
	return NULL;
}

static const char *_word(uint8_t st, uk_tone_t *tone) {
	switch (st) {
	case CEMU_ST_SENDING: *tone = UK_TONE_OK;     return "ON THE BUS";
	case CEMU_ST_WAITING: *tone = UK_TONE_MUTED;  return "STARTING";
	case CEMU_ST_PAUSED:  *tone = UK_TONE_WARN;   return "PAUSED";
	case CEMU_ST_REFUSED: *tone = UK_TONE_DANGER; return "STOPPED";
	default:              *tone = UK_TONE_MUTED;  return "OFF";
	}
}

static void _set_status(lv_obj_t *l, uint8_t st) {
	if (!l || !lv_obj_is_valid(l)) return;
	uk_tone_t tone;
	const char *w = _word(st, &tone);
	if (strcmp(lv_label_get_text(l), w)) lv_label_set_text(l, w);
	lv_obj_set_style_text_color(l, uk_tone(tone), 0);
}

static void _set_text(lv_obj_t *l, const char *t) {
	if (l && lv_obj_is_valid(l) && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

/* "resume" -> "Resume", "cruise_on" -> "Cruise on" */
static void _pretty(char *dst, size_t cap, const char *src) {
	snprintf(dst, cap, "%s", src);
	for (char *p = dst; *p; p++) if (*p == '_') *p = ' ';
	if (dst[0]) dst[0] = (char)toupper((unsigned char)dst[0]);
}

static void _sub_text(const cemu_device_t *d, char *buf, size_t cap) {
	if (d->reason[0] && d->status != CEMU_ST_WAITING) {
		snprintf(buf, cap, "%s", d->reason);
		return;
	}
	uint32_t sent = 0;
	for (int i = 0; i < d->n_send; i++) sent += d->send[i].sent;
	snprintf(buf, cap, "%u buttons - %u frames - %lu sent", (unsigned)d->n_controls,
	         (unsigned)d->n_send, (unsigned long)sent);
}

/* ── events ────────────────────────────────────────────────────────────── */

static void _close_cb(lv_event_t *e) { (void)e; can_emu_settings_close(); }

static void _open_dev_cb(lv_event_t *e) {
	const cemu_model_t *m = (const cemu_model_t *)can_emu_model();
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (!m || i >= m->n_devices) return;
	snprintf(S.dev_id, sizeof(S.dev_id), "%s", m->devices[i].id);
	S.view = 1;
	_build();
}

static void _back_cb(lv_event_t *e) { (void)e; S.view = 0; _build(); }

static void _switch_cb(lv_event_t *e) {
	lv_obj_t *sw = lv_event_get_target(e);
	const cemu_model_t *m = (const cemu_model_t *)can_emu_model();
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (!m || i >= m->n_devices) return;
	bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
	if (can_emu_set_enabled(m->devices[i].id, on) != ESP_OK)
		uk_toast("Couldn't save that", UK_TONE_DANGER);
}

static void _add_cb(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= S.n_adds) return;
	char err[160];
	if (can_emu_add_template(S.add_tpl[i], S.add_var[i][0] ? S.add_var[i] : NULL,
	                         err, sizeof(err)) != ESP_OK)
		uk_toast(err[0] ? err : "Couldn't add that", UK_TONE_DANGER);
	else
		uk_toast("Added", UK_TONE_OK);
}

static void _hold_cb(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= S.n_refs) return;
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSED && !S.held[i]) {
		S.held[i] = can_emu_press(S.refs[i]);
	} else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && S.held[i]) {
		can_emu_release(S.refs[i]);
		S.held[i] = false;
	}
}

static void _flip_cb(lv_event_t *e) {
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if (i >= 0 && i < S.n_refs) can_emu_press(S.refs[i]);
}

static void _remove_cb(lv_event_t *e) {
	(void)e;
	if (!S.remove_armed) {
		S.remove_armed = true;
		uk_btn_set_text(S.d_remove, "Tap again to remove it");
		return;
	}
	if (can_emu_remove(S.dev_id) == ESP_OK) {
		S.view = 0;
		S.dev_id[0] = '\0';
		_build();
	}
}

static void _release_held(void) {
	for (int i = 0; i < S.n_refs; i++)
		if (S.held[i]) { can_emu_release(S.refs[i]); S.held[i] = false; }
}

/* ── views ─────────────────────────────────────────────────────────────── */

static lv_obj_t *_row(lv_obj_t *parent) {
	lv_obj_t *r = lv_obj_create(parent);
	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_gap(r, 8, 0);
	lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
	return r;
}

static lv_obj_t *_note(lv_obj_t *parent, const char *text) {
	lv_obj_t *n = uk_label(parent, text, UK_FONT_SMALL, UK_TONE_MUTED);
	lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(n, lv_pct(100));
	lv_obj_set_style_text_line_space(n, 3, 0);
	return n;
}

static lv_obj_t *_switch(lv_obj_t *parent, bool on, int index) {
	lv_obj_t *sw = lv_switch_create(parent);
	uk_style_switch(sw);
	if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
	lv_obj_add_event_cb(sw, _switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)index);
	lv_obj_set_ext_click_area(sw, 12);
	return sw;
}

static void _build_list(void) {
	const cemu_model_t *m = (const cemu_model_t *)can_emu_model();
	int n = m ? m->n_devices : 0;
	if (!n)
		_note(S.body, "The dash can pretend to be a CAN keypad or IO box, so your ECU reads "
		              "buttons on this screen as its own inputs. Cruise control buttons on a "
		              "Haltech, for example.");
	for (int d = 0; d < n; d++) {
		const cemu_device_t *dv = &m->devices[d];
		lv_obj_t *card = uk_card(S.body);
		lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_gap(card, 12, 0);
		lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(card, _open_dev_cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);

		lv_obj_t *col = lv_obj_create(card);
		lv_obj_remove_style_all(col);
		lv_obj_set_flex_grow(col, 1);
		lv_obj_set_height(col, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_gap(col, 2, 0);
		lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
		uk_label(col, dv->name, UK_FONT_HEAD, UK_TONE_TEXT);
		char sub[160];
		_sub_text(dv, sub, sizeof(sub));
		S.list_sub[d] = _note(col, sub);

		S.list_status[d] = uk_label(card, "", UK_FONT_LABEL, UK_TONE_MUTED);
		_set_status(S.list_status[d], dv->status);
		S.list_sw[d] = _switch(card, dv->enabled, d);
		uk_icon(card, UK_ICON_RIGHT, UK_ICON_MD, UK_TONE_MUTED);
	}

	uk_section(S.body, "Add a device");
	lv_obj_t *adds = _row(S.body);
	S.n_adds = 0;
	cJSON *root = cJSON_Parse(can_emu_templates());
	cJSON *t;
	cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(root, "templates")) {
		const cJSON *id = cJSON_GetObjectItemCaseSensitive(t, "id");
		const cJSON *name = cJSON_GetObjectItemCaseSensitive(t, "name");
		cJSON *vars = cJSON_GetObjectItemCaseSensitive(t, "variants");
		if (!cJSON_IsString(id) || !cJSON_IsString(name) || cJSON_GetArraySize(vars) == 0)
			continue;                              /* "My own spec" is the editor's */
		cJSON *v;
		cJSON_ArrayForEach(v, vars) {
			if (S.n_adds >= MAX_ADDS) break;
			const cJSON *key = cJSON_GetObjectItemCaseSensitive(v, "key");
			const cJSON *vname = cJSON_GetObjectItemCaseSensitive(v, "name");
			snprintf(S.add_tpl[S.n_adds], sizeof(S.add_tpl[0]), "%s", id->valuestring);
			snprintf(S.add_var[S.n_adds], sizeof(S.add_var[0]), "%s",
			         cJSON_IsString(key) ? key->valuestring : "");
			char label[64];
			snprintf(label, sizeof(label), "%s - %s", name->valuestring,
			         cJSON_IsString(vname) ? vname->valuestring : "");
			uk_btn(adds, UK_ICON_PLUS, label, UK_BTN_NEUTRAL, _add_cb, (void *)(intptr_t)S.n_adds);
			S.n_adds++;
		}
	}
	cJSON_Delete(root);
	_note(S.body, "Voltages, frames and your own specs are set up in the web editor: "
	              "Setup - Keypads & IO boxes.");
}

static void _build_device(void) {
	int idx = 0;
	const cemu_device_t *dv = _dev(&idx);
	if (!dv) { S.view = 0; _build_list(); return; }

	lv_obj_t *top = _row(S.body);
	lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_gap(top, 12, 0);
	uk_btn(top, UK_ICON_BACK, "All devices", UK_BTN_GHOST, _back_cb, NULL);
	lv_obj_t *name = uk_label(top, dv->name, UK_FONT_HEAD, UK_TONE_TEXT);
	lv_obj_set_flex_grow(name, 1);
	S.d_status = uk_label(top, "", UK_FONT_LABEL, UK_TONE_MUTED);
	_set_status(S.d_status, dv->status);
	S.d_sw = _switch(top, dv->enabled, idx);
	S.d_reason = _note(S.body, dv->reason);

	/* Buttons, held for as long as a finger is on them — like the real one. */
	uk_section(S.body, "Hold to test");
	lv_obj_t *btns = _row(S.body);
	S.n_refs = 0;
	int selects = 0;
	for (int c = 0; c < dv->n_controls; c++) selects += dv->controls[c].kind == CEMU_KIND_SELECT;
	for (int c = 0; c < dv->n_controls && S.n_refs < MAX_REFS; c++) {
		const cemu_control_t *ct = &dv->controls[c];
		int first = ct->kind == CEMU_KIND_SELECT ? 1 : 0;
		int last = ct->kind == CEMU_KIND_SELECT ? ct->n_states - 1 : 0;
		for (int s = first; s <= last && S.n_refs < MAX_REFS; s++) {
			int i = S.n_refs++;
			char label[48], a[CEMU_NAME_LEN], b[CEMU_STATE_LEN];
			if (ct->kind == CEMU_KIND_SELECT) {
				snprintf(S.refs[i], REF_LEN, "%s:%s:%s", dv->id, ct->id, ct->states[s]);
				_pretty(b, sizeof(b), ct->states[s]);
				if (selects > 1) snprintf(label, sizeof(label), "%s %s", ct->name, b);
				else snprintf(label, sizeof(label), "%s", b);
			} else {
				snprintf(S.refs[i], REF_LEN, "%s:%s", dv->id, ct->id);
				_pretty(a, sizeof(a), ct->name);
				snprintf(label, sizeof(label), "%s", a);
			}
			S.ref_ctl[i] = (int8_t)c;
			S.held[i] = false;
			bool latch = ct->kind == CEMU_KIND_LATCH;
			lv_obj_t *btn = uk_btn(btns, UK_ICON_NONE, label,
			                       latch && ct->latched ? UK_BTN_ON : UK_BTN_NEUTRAL, NULL, NULL);
			lv_obj_set_height(btn, 52);
			lv_obj_set_style_min_width(btn, 120, 0);
			if (latch) {
				lv_obj_add_event_cb(btn, _flip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
			} else {
				lv_obj_add_event_cb(btn, _hold_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
				lv_obj_add_event_cb(btn, _hold_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);
				lv_obj_add_event_cb(btn, _hold_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)i);
			}
			S.ref_btn[i] = btn;
		}
	}
	if (!S.n_refs) _note(S.body, "This device has no buttons yet.");

	uk_section(S.body, "What it sends");
	for (int i = 0; i < dv->n_send; i++) {
		char key[40];
		snprintf(key, sizeof(key), "0x%lX  every %u ms", (unsigned long)dv->send[i].can_id,
		         (unsigned)dv->send[i].every_ms);
		S.frame_val[i] = uk_row(S.body, key, "");
	}

	int k = 0;
	if (dv->n_listen) uk_section(S.body, "What the ECU sends back");
	for (int l = 0; l < dv->n_listen; l++)
		for (int c = 0; c < dv->listen[l].n_channels; c++, k++)
			S.chan_val[k] = uk_row(S.body, dv->listen[l].channels[c].name, "no data");

	S.remove_armed = false;
	S.d_remove = uk_btn(S.body, UK_ICON_TRASH, "Remove this device", UK_BTN_DANGER, _remove_cb, NULL);
}

static void _refresh(lv_timer_t *t);

static void _build(void) {
	if (!S.body) return;
	_release_held();
	lv_obj_clean(S.body);
	memset(S.list_status, 0, sizeof(S.list_status));
	memset(S.list_sub, 0, sizeof(S.list_sub));
	memset(S.list_sw, 0, sizeof(S.list_sw));
	memset(S.frame_val, 0, sizeof(S.frame_val));
	memset(S.chan_val, 0, sizeof(S.chan_val));
	memset(S.ref_btn, 0, sizeof(S.ref_btn));
	S.d_status = S.d_reason = S.d_sw = S.d_remove = NULL;
	S.n_refs = 0;
	S.gen = can_emu_generation();
	if (S.view == 1) _build_device();
	else _build_list();
	_refresh(NULL);
	lv_obj_scroll_to_y(S.body, 0, LV_ANIM_OFF);
}

static void _sync_switch(lv_obj_t *sw, bool on) {
	if (!sw || !lv_obj_is_valid(sw) || lv_obj_has_state(sw, LV_STATE_PRESSED)) return;
	if (lv_obj_has_state(sw, LV_STATE_CHECKED) != on) {
		if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
		else lv_obj_clear_state(sw, LV_STATE_CHECKED);
	}
}

static void _refresh(lv_timer_t *t) {
	(void)t;
	if (!S.popup) return;
	if (S.gen != can_emu_generation()) { _build(); return; }
	const cemu_model_t *m = (const cemu_model_t *)can_emu_model();
	if (!m) return;
	if (S.view == 0) {
		for (int d = 0; d < m->n_devices; d++) {
			const cemu_device_t *dv = &m->devices[d];
			_set_status(S.list_status[d], dv->status);
			char sub[160];
			_sub_text(dv, sub, sizeof(sub));
			_set_text(S.list_sub[d], sub);
			_sync_switch(S.list_sw[d], dv->enabled);
		}
		return;
	}
	const cemu_device_t *dv = _dev(NULL);
	if (!dv) return;
	_set_status(S.d_status, dv->status);
	_set_text(S.d_reason, dv->status == CEMU_ST_SENDING ? "" : dv->reason);
	_sync_switch(S.d_sw, dv->enabled);
	for (int i = 0; i < S.n_refs; i++) {
		const cemu_control_t *ct = &dv->controls[S.ref_ctl[i]];
		if (ct->kind == CEMU_KIND_LATCH && S.ref_btn[i])
			uk_btn_set_kind(S.ref_btn[i], ct->latched ? UK_BTN_ON : UK_BTN_NEUTRAL);
	}
	for (int i = 0; i < dv->n_send; i++) {
		const cemu_send_t *s = &dv->send[i];
		char v[64];
		int o = 0;
		uint8_t now[8];
		uint8_t dlc = cemu_peek(dv, s, NULL, NULL, now);
		for (int b = 0; b < dlc && o < 40; b++)
			o += snprintf(v + o, sizeof(v) - (size_t)o, "%02X ", now[b]);
		snprintf(v + o, sizeof(v) - (size_t)o, "  %lu sent", (unsigned long)s->sent);
		_set_text(S.frame_val[i], v);
	}
	int k = 0;
	for (int l = 0; l < dv->n_listen; l++)
		for (int c = 0; c < dv->listen[l].n_channels; c++, k++) {
			const cemu_channel_t *ch = &dv->listen[l].channels[c];
			char v[40] = "no data";
			/* The ECU's reply is an ordinary channel; read it the same way. */
			int16_t si = signal_find_by_name(ch->name);
			const signal_t *sg = si >= 0 ? signal_get_by_index((uint16_t)si) : NULL;
			if (sg && !sg->is_stale && sg->last_update_ms)
				snprintf(v, sizeof(v), "%.0f %s", (double)sg->current_value, ch->unit);
			_set_text(S.chan_val[k], v);
		}
}

/* ── open / close / tile ───────────────────────────────────────────────── */

void can_emu_settings_open(void) {
	if (S.popup && lv_obj_is_valid(S.popup)) return;
	memset(&S, 0, sizeof(S));
	S.popup = uk_popup(POP_W, POP_H, "Keypads & IO boxes", _close_cb);
	S.body = lv_obj_create(S.popup);
	lv_obj_remove_style_all(S.body);
	lv_obj_set_size(S.body, POP_W - 40, POP_H - UK_POPUP_BODY_Y - 24);
	lv_obj_align(S.body, LV_ALIGN_TOP_LEFT, 0, UK_POPUP_BODY_Y);
	lv_obj_set_flex_flow(S.body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_gap(S.body, 8, 0);
	lv_obj_set_style_pad_right(S.body, 8, 0);
	lv_obj_add_flag(S.body, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(S.body, LV_DIR_VER);
	uk_style_scrollbar(S.body);
	_build();
	S.timer = lv_timer_create(_refresh, 250, NULL);
}

void can_emu_settings_close(void) {
	_release_held();
	if (S.timer) lv_timer_del(S.timer);
	if (S.popup && lv_obj_is_valid(S.popup)) lv_obj_del(S.popup);
	memset(&S, 0, sizeof(S));
}

void can_emu_settings_stat(char *buf, size_t cap, uk_tone_t *tone) {
	can_emu_summary_t s;
	can_emu_summary(&s);
	*tone = UK_TONE_TEXT;
	if (!s.devices) snprintf(buf, cap, "Add one");
	else if (s.problems) { snprintf(buf, cap, "Check"); *tone = UK_TONE_DANGER; }
	else if (s.sending) { snprintf(buf, cap, "%u on the bus", (unsigned)s.sending); *tone = UK_TONE_OK; }
	else if (s.enabled) { snprintf(buf, cap, "Not sending"); *tone = UK_TONE_WARN; }
	else snprintf(buf, cap, "Off");
}
