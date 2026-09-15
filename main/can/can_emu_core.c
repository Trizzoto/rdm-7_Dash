/*
 * can_emu_core.c — parse, control state, frame bytes. See can_emu_core.h.
 *
 * Everything that arrives here came over HTTP or out of a file a person
 * edited, and the result is traffic on a car's bus. So the parser refuses
 * rather than guesses: a field that runs off the end of its frame, two fields
 * writing the same bits, a value table shorter than the control's states, an
 * 11-bit frame with a 29-bit ID — each is an error with a sentence that says
 * where, never a silently clamped frame.
 */
#include "can_emu_core.h"
#include "can_decode.h"
#include "cJSON.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── small helpers ─────────────────────────────────────────────────────── */

static bool _fail(char *err, size_t n, const char *fmt, ...) {
	if (err && n) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(err, n, fmt, ap);
		va_end(ap);
	}
	return false;
}

static void _copy(char *dst, size_t cap, const char *src) {
	if (!cap) return;
	if (!src) { dst[0] = '\0'; return; }
	strncpy(dst, src, cap - 1);
	dst[cap - 1] = '\0';
}

static int _nibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return (c | 0x20) - 'a' + 10;
	return -1;
}

/* A number, or a string "0x2C0" / "2C0h" / "704". */
static bool _json_u32(const cJSON *j, uint32_t *out) {
	if (cJSON_IsNumber(j)) {
		if (j->valuedouble < 0 || j->valuedouble > 4294967295.0) return false;
		*out = (uint32_t)j->valuedouble;
		return true;
	}
	if (cJSON_IsString(j) && j->valuestring) {
		const char *s = j->valuestring;
		char *end = NULL;
		unsigned long v;
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) v = strtoul(s + 2, &end, 16);
		else v = strtoul(s, &end, 10);
		if (!end || end == s || *end) return false;
		*out = (uint32_t)v;
		return true;
	}
	return false;
}

static bool _id_ok(const char *s) {
	if (!s || !s[0] || strlen(s) >= CEMU_ID_LEN) return false;
	for (; *s; s++) {
		char c = *s;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '-'))
			return false;
	}
	return true;
}

static const char *_str(const cJSON *obj, const char *key) {
	const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
	return (cJSON_IsString(j) && j->valuestring) ? j->valuestring : NULL;
}

static bool _num(const cJSON *obj, const char *key, double *out) {
	const cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
	if (!cJSON_IsNumber(j)) return false;
	*out = j->valuedouble;
	return true;
}

static uint32_t _max_raw(uint8_t bit_length) {
	return bit_length >= 32 ? 0xFFFFFFFFu : ((1u << bit_length) - 1u);
}

/* The last byte a field touches. Same for both bit orders: can_pack_bits
 * counts a big-endian field's bits forward from bit_start's byte too. */
static uint8_t _last_byte(uint8_t bit_start, uint8_t bit_length) {
	return (uint8_t)((bit_start + bit_length - 1) / 8);
}

/* ── bits ──────────────────────────────────────────────────────────────── */

uint32_t cemu_to_raw(float v, float scale, float offset, uint8_t bit_length) {
	if (scale == 0.0f || !isfinite(v)) return 0;
	double r = ((double)v - (double)offset) / (double)scale;
	if (!isfinite(r) || r <= 0.0) return 0;
	double max = (double)_max_raw(bit_length);
	r = floor(r + 0.5);
	return r >= max ? _max_raw(bit_length) : (uint32_t)r;
}

void cemu_put_bits(uint8_t *data, uint8_t bit_start, uint8_t bit_length,
                   uint32_t value, uint8_t endian) {
	if (!data || bit_length == 0 || bit_length > 32) return;
	if (_last_byte(bit_start, bit_length) > 7) return;
	uint8_t mask[8] = {0};
	can_pack_bits(mask, bit_start, bit_length, 0xFFFFFFFFu, endian);
	for (int i = 0; i < 8; i++) data[i] &= (uint8_t)~mask[i];
	can_pack_bits(data, bit_start, bit_length, value, endian);
}

/* ── parse ─────────────────────────────────────────────────────────────── */

static bool _parse_bits(const cJSON *j, uint8_t dlc, uint8_t *start, uint8_t *len,
                        uint8_t *endian, const char *where, char *err, size_t n) {
	double bs, bl, en = 1;
	if (!_num(j, "bit_start", &bs) || !_num(j, "bit_length", &bl))
		return _fail(err, n, "%s: needs bit_start and bit_length.", where);
	_num(j, "endian", &en);
	if (bs < 0 || bs > 63)
		return _fail(err, n, "%s: bit_start %g is outside 0-63.", where, bs);
	if (bl < 1 || bl > 32)
		return _fail(err, n, "%s: bit_length %g is outside 1-32.", where, bl);
	*start = (uint8_t)bs;
	*len = (uint8_t)bl;
	*endian = en ? 1 : 0;
	if (_last_byte(*start, *len) >= dlc)
		return _fail(err, n, "%s: bit %u + %u bits runs past the end of a %u-byte frame.",
		             where, (unsigned)*start, (unsigned)*len, (unsigned)dlc);
	return true;
}

static bool _parse_control(const cJSON *j, cemu_control_t *c, const char *dev,
                           char *err, size_t n) {
	const char *id = _str(j, "id");
	if (!_id_ok(id))
		return _fail(err, n, "%s: every control needs an id of letters, digits, _ or - "
		             "(up to %d).", dev, CEMU_ID_LEN - 1);
	_copy(c->id, sizeof(c->id), id);
	const char *name = _str(j, "name");
	_copy(c->name, sizeof(c->name), name ? name : id);

	const char *kind = _str(j, "kind");
	if (!kind || !strcmp(kind, "momentary")) c->kind = CEMU_KIND_MOMENTARY;
	else if (!strcmp(kind, "latch"))         c->kind = CEMU_KIND_LATCH;
	else if (!strcmp(kind, "select"))        c->kind = CEMU_KIND_SELECT;
	else return _fail(err, n, "%s, control %s: kind \"%s\" isn't momentary, latch or select.",
	                  dev, id, kind);

	const cJSON *states = cJSON_GetObjectItemCaseSensitive(j, "states");
	if (c->kind == CEMU_KIND_SELECT) {
		int ns = cJSON_GetArraySize(states);
		if (!cJSON_IsArray(states) || ns < 2 || ns > CEMU_MAX_STATES)
			return _fail(err, n, "%s, control %s: a select needs 2-%d states, the first "
			             "being what it rests at.", dev, id, CEMU_MAX_STATES);
		int i = 0;
		const cJSON *s;
		cJSON_ArrayForEach(s, states) {
			if (!cJSON_IsString(s) || !_id_ok(s->valuestring) ||
			    strlen(s->valuestring) >= CEMU_STATE_LEN)
				return _fail(err, n, "%s, control %s: state %d needs a short name of letters, "
				             "digits, _ or -.", dev, id, i + 1);
			for (int k = 0; k < i; k++)
				if (!strcmp(c->states[k], s->valuestring))
					return _fail(err, n, "%s, control %s: state \"%s\" is listed twice.",
					             dev, id, s->valuestring);
			_copy(c->states[i], CEMU_STATE_LEN, s->valuestring);
			i++;
		}
		c->n_states = (uint8_t)ns;
	} else {
		c->n_states = 2;
		_copy(c->states[0], CEMU_STATE_LEN, "off");
		_copy(c->states[1], CEMU_STATE_LEN, "on");
	}
	c->remember = c->kind == CEMU_KIND_LATCH &&
	              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "remember"));
	return true;
}

static int _find_control(const cemu_device_t *d, const char *id) {
	for (int i = 0; i < d->n_controls; i++)
		if (!strcmp(d->controls[i].id, id)) return i;
	return -1;
}

static bool _parse_field(const cJSON *j, cemu_device_t *d, cemu_field_t *f,
                         uint8_t dlc, const char *where, char *err, size_t n) {
	if (!_parse_bits(j, dlc, &f->bit_start, &f->bit_length, &f->endian, where, err, n))
		return false;
	double v;
	f->scale = _num(j, "scale", &v) ? (float)v : 1.0f;
	f->offset = _num(j, "offset", &v) ? (float)v : 0.0f;
	if (f->scale == 0.0f || !isfinite(f->scale))
		return _fail(err, n, "%s: scale can't be 0.", where);
	f->value = _num(j, "value", &v) ? (float)v : 0.0f;
	/* What the input really carries: a Haltech analog input is 0-5 V in a
	 * 16-bit slot that could otherwise say 80 V. Values are clamped to it. */
	f->min = _num(j, "min", &v) ? (float)v : f->offset;
	f->max = _num(j, "max", &v) ? (float)v : -1.0f;
	if (f->max >= 0.0f && f->max < f->min)
		return _fail(err, n, "%s: max is below min.", where);
	if (f->max < 0.0f) f->max = f->min - 1.0f;          /* none given */

	const char *ctl = _str(j, "control");
	const char *chan = _str(j, "channel");
	if (ctl) {
		int ci = _find_control(d, ctl);
		if (ci < 0)
			return _fail(err, n, "%s: there is no control \"%s\" on this device.", where, ctl);
		const cemu_control_t *c = &d->controls[ci];
		f->src = CEMU_SRC_CONTROL;
		f->control = (uint8_t)ci;
		const cJSON *vals = cJSON_GetObjectItemCaseSensitive(j, "values");
		if (!vals) {
			/* No table: rest is 0, every other state is the field full. The old
			 * button behaviour, and right for a 1-bit switch. */
			float full = (float)_max_raw(f->bit_length) * f->scale + f->offset;
			for (int s = 0; s < c->n_states; s++)
				f->values[s] = s ? full : f->offset;
		} else {
			if (!cJSON_IsArray(vals) || cJSON_GetArraySize(vals) != c->n_states)
				return _fail(err, n, "%s: values needs one number for each of %s's %u states.",
				             where, c->id, (unsigned)c->n_states);
			int s = 0;
			const cJSON *e;
			cJSON_ArrayForEach(e, vals) {
				if (!cJSON_IsNumber(e) || !isfinite(e->valuedouble))
					return _fail(err, n, "%s: value %d for %s isn't a number.",
					             where, s + 1, c->id);
				f->values[s++] = (float)e->valuedouble;
			}
		}
	} else if (chan) {
		if (!chan[0] || strlen(chan) >= CEMU_SIG_LEN)
			return _fail(err, n, "%s: channel name is empty or too long.", where);
		f->src = CEMU_SRC_CHANNEL;
		_copy(f->channel, sizeof(f->channel), chan);
	} else if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "counter"))) {
		f->src = CEMU_SRC_COUNTER;
	} else {
		f->src = CEMU_SRC_CONST;
	}
	return true;
}

static bool _parse_send(const cJSON *j, cemu_device_t *d, cemu_send_t *s,
                        const char *dev, char *err, size_t n) {
	char where[64];
	if (!_json_u32(cJSON_GetObjectItemCaseSensitive(j, "can_id"), &s->can_id))
		return _fail(err, n, "%s: every frame it sends needs a can_id.", dev);
	s->extd = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "extd"));
	snprintf(where, sizeof(where), "%s, frame 0x%lX", dev, (unsigned long)s->can_id);
	if (s->extd ? s->can_id > 0x1FFFFFFFu : s->can_id > 0x7FFu)
		return _fail(err, n, s->extd ? "%s: a 29-bit ID can't be above 0x1FFFFFFF."
		                            : "%s: that's above 0x7FF, so it needs \"extd\": true.",
		             where);
	double v;
	s->dlc = _num(j, "dlc", &v) ? (uint8_t)v : 8;
	if (s->dlc < 1 || s->dlc > 8)
		return _fail(err, n, "%s: dlc must be 1-8.", where);
	s->every_ms = _num(j, "every_ms", &v) ? (uint16_t)(v < 0 ? 0 : (v > 65535 ? 65535 : v)) : 100;
	if (s->every_ms < CEMU_MIN_EVERY_MS || s->every_ms > CEMU_MAX_EVERY_MS)
		return _fail(err, n, "%s: every_ms must be %d-%d.", where,
		             CEMU_MIN_EVERY_MS, CEMU_MAX_EVERY_MS);

	const char *hex = _str(j, "data");
	if (hex) {
		int b = 0;
		for (const char *p = hex; *p;) {
			if (*p == ' ') { p++; continue; }
			int hi = _nibble(p[0]), lo = p[1] ? _nibble(p[1]) : -1;
			if (hi < 0 || lo < 0 || b >= s->dlc)
				return _fail(err, n, "%s: data should be up to %u bytes of hex, like "
				             "\"10090D0100\".", where, (unsigned)s->dlc);
			s->base[b++] = (uint8_t)((hi << 4) | lo);
			p += 2;
		}
	}

	const cJSON *fields = cJSON_GetObjectItemCaseSensitive(j, "fields");
	if (fields && !cJSON_IsArray(fields))
		return _fail(err, n, "%s: fields must be a list.", where);
	if (cJSON_GetArraySize(fields) > CEMU_MAX_FIELDS)
		return _fail(err, n, "%s: at most %d fields in one frame.", where, CEMU_MAX_FIELDS);
	uint8_t used[8] = {0};
	const cJSON *fj;
	cJSON_ArrayForEach(fj, fields) {
		cemu_field_t *f = &s->fields[s->n_fields];
		char fwhere[80];
		snprintf(fwhere, sizeof(fwhere), "%s, field %u", where, (unsigned)s->n_fields + 1);
		if (!_parse_field(fj, d, f, s->dlc, fwhere, err, n)) return false;
		uint8_t mask[8] = {0};
		can_pack_bits(mask, f->bit_start, f->bit_length, 0xFFFFFFFFu, f->endian);
		for (int i = 0; i < 8; i++) {
			if (used[i] & mask[i])
				return _fail(err, n, "%s: overlaps bits another field already writes "
				             "(byte %d).", fwhere, i);
			used[i] |= mask[i];
		}
		s->n_fields++;
	}
	return true;
}

static bool _parse_listen(const cJSON *j, cemu_listen_t *l, const char *dev,
                          char *err, size_t n) {
	char where[64];
	if (!_json_u32(cJSON_GetObjectItemCaseSensitive(j, "can_id"), &l->can_id))
		return _fail(err, n, "%s: every frame it listens to needs a can_id.", dev);
	l->extd = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "extd"));
	snprintf(where, sizeof(where), "%s, listening on 0x%lX", dev, (unsigned long)l->can_id);
	if (l->extd ? l->can_id > 0x1FFFFFFFu : l->can_id > 0x7FFu)
		return _fail(err, n, "%s: that ID doesn't fit %s.", where,
		             l->extd ? "29 bits" : "11 bits (set \"extd\": true)");
	const cJSON *chans = cJSON_GetObjectItemCaseSensitive(j, "channels");
	if (!cJSON_IsArray(chans) || cJSON_GetArraySize(chans) < 1 ||
	    cJSON_GetArraySize(chans) > CEMU_MAX_CHANNELS)
		return _fail(err, n, "%s: needs 1-%d channels.", where, CEMU_MAX_CHANNELS);
	const cJSON *cj;
	cJSON_ArrayForEach(cj, chans) {
		cemu_channel_t *c = &l->channels[l->n_channels];
		const char *name = _str(cj, "name");
		if (!name || !name[0] || strlen(name) >= CEMU_SIG_LEN)
			return _fail(err, n, "%s: every channel needs a name (up to %d characters).",
			             where, CEMU_SIG_LEN - 1);
		_copy(c->name, sizeof(c->name), name);
		char cwhere[96];
		snprintf(cwhere, sizeof(cwhere), "%s, channel %s", where, name);
		if (!_parse_bits(cj, 8, &c->bit_start, &c->bit_length, &c->endian, cwhere, err, n))
			return false;
		double v;
		c->scale = _num(cj, "scale", &v) ? (float)v : 1.0f;
		c->offset = _num(cj, "offset", &v) ? (float)v : 0.0f;
		c->is_signed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cj, "is_signed"));
		_copy(c->unit, sizeof(c->unit), _str(cj, "unit"));
		l->n_channels++;
	}
	return true;
}

static bool _parse_device(const cJSON *j, cemu_device_t *d, int index,
                          char *err, size_t n) {
	const char *id = _str(j, "id");
	char dev[48];
	if (!_id_ok(id))
		return _fail(err, n, "Device %d needs an id of letters, digits, _ or - (up to %d).",
		             index + 1, CEMU_ID_LEN - 1);
	_copy(d->id, sizeof(d->id), id);
	const char *name = _str(j, "name");
	_copy(d->name, sizeof(d->name), name ? name : id);
	snprintf(dev, sizeof(dev), "%s", d->name);
	_copy(d->template_id, sizeof(d->template_id), _str(j, "template"));
	const cJSON *en = cJSON_GetObjectItemCaseSensitive(j, "enabled");
	d->enabled = !cJSON_IsBool(en) || cJSON_IsTrue(en);
	double v;
	d->bitrate_k = _num(j, "bitrate", &v) ? (uint16_t)v : 0;
	if (d->bitrate_k && d->bitrate_k != 125 && d->bitrate_k != 250 &&
	    d->bitrate_k != 500 && d->bitrate_k != 1000)
		return _fail(err, n, "%s: bitrate must be 125, 250, 500 or 1000.", dev);

	const cJSON *controls = cJSON_GetObjectItemCaseSensitive(j, "controls");
	if (cJSON_GetArraySize(controls) > CEMU_MAX_CONTROLS)
		return _fail(err, n, "%s: at most %d controls.", dev, CEMU_MAX_CONTROLS);
	const cJSON *e;
	cJSON_ArrayForEach(e, controls) {
		cemu_control_t *c = &d->controls[d->n_controls];
		if (!_parse_control(e, c, dev, err, n)) return false;
		if (_find_control(d, c->id) >= 0)
			return _fail(err, n, "%s: two controls are called \"%s\".", dev, c->id);
		d->n_controls++;
	}

	const cJSON *send = cJSON_GetObjectItemCaseSensitive(j, "send");
	if (cJSON_GetArraySize(send) > CEMU_MAX_SEND)
		return _fail(err, n, "%s: at most %d frames to send.", dev, CEMU_MAX_SEND);
	cJSON_ArrayForEach(e, send) {
		cemu_send_t *s = &d->send[d->n_send];
		if (!_parse_send(e, d, s, dev, err, n)) return false;
		for (int k = 0; k < d->n_send; k++)
			if (d->send[k].can_id == s->can_id && d->send[k].extd == s->extd)
				return _fail(err, n, "%s: sends 0x%lX twice. Put both fields in one frame.",
				             dev, (unsigned long)s->can_id);
		d->n_send++;
	}

	const cJSON *listen = cJSON_GetObjectItemCaseSensitive(j, "listen");
	if (cJSON_GetArraySize(listen) > CEMU_MAX_LISTEN)
		return _fail(err, n, "%s: at most %d frames to listen to.", dev, CEMU_MAX_LISTEN);
	cJSON_ArrayForEach(e, listen) {
		cemu_listen_t *l = &d->listen[d->n_listen];
		if (!_parse_listen(e, l, dev, err, n)) return false;
		for (int k = 0; k < d->n_send; k++)
			if (d->send[k].can_id == l->can_id && d->send[k].extd == l->extd)
				return _fail(err, n, "%s: listens on 0x%lX, which it also sends.",
				             dev, (unsigned long)l->can_id);
		d->n_listen++;
	}
	return true;
}

bool cemu_parse(const char *json, cemu_model_t *out, char *err, size_t n) {
	if (!out) return false;
	memset(out, 0, sizeof(*out));
	if (err && n) err[0] = '\0';
	cJSON *root = json ? cJSON_Parse(json) : NULL;
	if (!cJSON_IsObject(root)) {
		cJSON_Delete(root);
		return _fail(err, n, "That isn't valid JSON.");
	}
	bool ok = true;
	const cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
	if (devs && !cJSON_IsArray(devs)) {
		ok = _fail(err, n, "\"devices\" must be a list.");
	} else if (cJSON_GetArraySize(devs) > CEMU_MAX_DEVICES) {
		ok = _fail(err, n, "At most %d devices.", CEMU_MAX_DEVICES);
	} else {
		const cJSON *e;
		cJSON_ArrayForEach(e, devs) {
			cemu_device_t *d = &out->devices[out->n_devices];
			if (!_parse_device(e, d, out->n_devices, err, n)) { ok = false; break; }
			for (int k = 0; k < out->n_devices; k++)
				if (!strcmp(out->devices[k].id, d->id)) {
					ok = _fail(err, n, "Two devices are called \"%s\".", d->id);
					break;
				}
			if (!ok) break;
			out->n_devices++;
		}
	}
	/* Two devices sending one ID would be the shared-frame bug all over again,
	 * one level up. */
	for (int a = 0; ok && a < out->n_devices; a++)
		for (int b = a + 1; ok && b < out->n_devices; b++)
			for (int i = 0; ok && i < out->devices[a].n_send; i++)
				for (int k = 0; ok && k < out->devices[b].n_send; k++) {
					const cemu_send_t *x = &out->devices[a].send[i];
					const cemu_send_t *y = &out->devices[b].send[k];
					if (x->can_id == y->can_id && x->extd == y->extd)
						ok = _fail(err, n, "%s and %s both send 0x%lX.",
						           out->devices[a].name, out->devices[b].name,
						           (unsigned long)x->can_id);
				}
	cJSON_Delete(root);
	if (!ok) memset(out, 0, sizeof(*out));
	return ok;
}

/* ── controls ──────────────────────────────────────────────────────────── */

bool cemu_resolve(const cemu_model_t *m, const char *ref,
                  int *dev_idx, int *ctl_idx, int *state_idx) {
	if (!m || !ref) return false;
	char buf[CEMU_ID_LEN * 2 + CEMU_STATE_LEN + 4];
	_copy(buf, sizeof(buf), ref);
	char *ctl = strchr(buf, ':');
	if (!ctl) return false;
	*ctl++ = '\0';
	char *st = strchr(ctl, ':');
	if (st) *st++ = '\0';
	for (int d = 0; d < m->n_devices; d++) {
		const cemu_device_t *dv = &m->devices[d];
		if (strcmp(dv->id, buf)) continue;
		int c = _find_control(dv, ctl);
		if (c < 0) return false;
		int s = -1;
		if (st && *st) {
			for (int k = 0; k < dv->controls[c].n_states; k++)
				if (!strcmp(dv->controls[c].states[k], st)) { s = k; break; }
			if (s < 0) return false;
		}
		if (dev_idx) *dev_idx = d;
		if (ctl_idx) *ctl_idx = c;
		if (state_idx) *state_idx = s;
		return true;
	}
	return false;
}

void cemu_press(cemu_model_t *m, cemu_control_t *c, int state) {
	if (!c) return;
	if (c->kind == CEMU_KIND_LATCH) { c->latched = !c->latched; return; }
	if (state < 0) state = 1;
	if (state >= c->n_states) return;
	if (c->holds[state] < 255) c->holds[state]++;
	c->seq[state] = m ? ++m->seq : 1;
}

void cemu_release(cemu_control_t *c, int state) {
	if (!c || c->kind == CEMU_KIND_LATCH) return;
	if (state < 0) state = 1;
	if (state >= c->n_states) return;
	if (c->holds[state]) c->holds[state]--;
}

void cemu_set_latch(cemu_control_t *c, bool on) {
	if (c && c->kind == CEMU_KIND_LATCH) c->latched = on ? 1 : 0;
}

void cemu_release_all(cemu_model_t *m) {
	if (!m) return;
	for (int d = 0; d < m->n_devices; d++)
		for (int c = 0; c < m->devices[d].n_controls; c++) {
			cemu_control_t *ct = &m->devices[d].controls[c];
			memset(ct->holds, 0, sizeof(ct->holds));
		}
}

uint8_t cemu_control_state(const cemu_control_t *c) {
	if (!c) return 0;
	if (c->kind == CEMU_KIND_LATCH) return c->latched ? 1 : 0;
	if (c->kind == CEMU_KIND_MOMENTARY) return c->holds[1] ? 1 : 0;
	uint8_t best = 0;
	uint32_t best_seq = 0;
	for (uint8_t s = 0; s < c->n_states; s++)
		if (c->holds[s] && c->seq[s] >= best_seq) { best = s; best_seq = c->seq[s]; }
	return best;
}

/* ── frames ────────────────────────────────────────────────────────────── */

static float _range(const cemu_field_t *f, float v) {
	if (f->max < f->min) return v;                     /* no range given */
	return v < f->min ? f->min : (v > f->max ? f->max : v);
}

static uint8_t _compose(const cemu_device_t *d, cemu_send_t *s,
                       cemu_channel_fn fn, void *user, uint8_t out[8], bool advance) {
	memset(out, 0, 8);
	if (!d || !s) return 0;
	memcpy(out, s->base, s->dlc);
	for (int i = 0; i < s->n_fields; i++) {
		cemu_field_t *f = &s->fields[i];
		uint32_t raw;
		switch (f->src) {
		case CEMU_SRC_CONTROL: {
			uint8_t st = cemu_control_state(&d->controls[f->control]);
			raw = cemu_to_raw(_range(f, f->values[st]), f->scale, f->offset, f->bit_length);
			break;
		}
		case CEMU_SRC_COUNTER:
			raw = f->counter & _max_raw(f->bit_length);
			if (advance) f->counter++;
			break;
		case CEMU_SRC_CHANNEL: {
			float v = f->value;
			if (!fn || !fn(f->channel, &v, user)) v = f->value;
			raw = cemu_to_raw(_range(f, v), f->scale, f->offset, f->bit_length);
			break;
		}
		default:
			raw = cemu_to_raw(_range(f, f->value), f->scale, f->offset, f->bit_length);
			break;
		}
		cemu_put_bits(out, f->bit_start, f->bit_length, raw, f->endian);
	}
	return s->dlc;
}

uint8_t cemu_compose(const cemu_device_t *d, cemu_send_t *s,
                     cemu_channel_fn fn, void *user, uint8_t out[8]) {
	return _compose(d, s, fn, user, out, true);
}

uint8_t cemu_peek(const cemu_device_t *d, const cemu_send_t *s,
                  cemu_channel_fn fn, void *user, uint8_t out[8]) {
	/* _compose only writes through s for counters, and not when !advance. */
	return _compose(d, (cemu_send_t *)s, fn, user, out, false);
}

/* ── templates ─────────────────────────────────────────────────────────── */

static void _subst_strings(cJSON *node, const char *upper, const char *lower) {
	for (cJSON *c = node ? node->child : NULL; c; c = c->next) {
		if (cJSON_IsString(c) && c->valuestring &&
		    (strstr(c->valuestring, "{V}") || strstr(c->valuestring, "{v}"))) {
			char buf[128];
			size_t o = 0;
			for (const char *p = c->valuestring; *p && o < sizeof(buf) - 8;) {
				if (p[0] == '{' && (p[1] == 'V' || p[1] == 'v') && p[2] == '}') {
					const char *r = p[1] == 'V' ? upper : lower;
					for (; *r && o < sizeof(buf) - 8; r++) buf[o++] = *r;
					p += 3;
				} else {
					buf[o++] = *p++;
				}
			}
			buf[o] = '\0';
			cJSON_SetValuestring(c, buf);
		} else if (cJSON_IsObject(c) || cJSON_IsArray(c)) {
			_subst_strings(c, upper, lower);
		}
	}
}

static void _offset_ids(cJSON *list, uint32_t off) {
	cJSON *e;
	cJSON_ArrayForEach(e, list) {
		cJSON *id = cJSON_GetObjectItemCaseSensitive(e, "can_id");
		uint32_t v;
		if (_json_u32(id, &v))
			cJSON_ReplaceItemInObjectCaseSensitive(e, "can_id", cJSON_CreateNumber(v + off));
	}
}

char *cemu_template_device(const char *templates_json, const char *template_id,
                           const char *variant_key, const char *id,
                           char *err, size_t n) {
	if (err && n) err[0] = '\0';
	cJSON *root = templates_json ? cJSON_Parse(templates_json) : NULL;
	cJSON *tpl = NULL, *e;
	cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(root, "templates")) {
		const char *tid = _str(e, "id");
		if (tid && template_id && !strcmp(tid, template_id)) { tpl = e; break; }
	}
	if (!tpl) {
		cJSON_Delete(root);
		_fail(err, n, "There's no template called \"%s\".", template_id ? template_id : "");
		return NULL;
	}
	uint32_t off = 0;
	char upper[8] = "", lower[8] = "";
	cJSON *variants = cJSON_GetObjectItemCaseSensitive(tpl, "variants");
	if (cJSON_GetArraySize(variants) > 0) {
		cJSON *pick = NULL;
		cJSON_ArrayForEach(e, variants) {
			const char *k = _str(e, "key");
			if (!variant_key || !variant_key[0]) { pick = e; break; }
			if (k && !strcmp(k, variant_key)) { pick = e; break; }
		}
		if (!pick) {
			cJSON_Delete(root);
			_fail(err, n, "\"%s\" has no variant \"%s\".", template_id, variant_key);
			return NULL;
		}
		const char *k = _str(pick, "key");
		_copy(upper, sizeof(upper), k ? k : "");
		for (int i = 0; upper[i]; i++) {
			char c = upper[i];
			lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
			lower[i + 1] = '\0';
		}
		double v;
		if (_num(pick, "id_offset", &v) && v > 0) off = (uint32_t)v;
	}
	cJSON *dev = cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(tpl, "device"), 1);
	cJSON_Delete(root);
	if (!dev) { _fail(err, n, "That template has no device."); return NULL; }
	_subst_strings(dev, upper, lower);
	if (off) {
		_offset_ids(cJSON_GetObjectItemCaseSensitive(dev, "send"), off);
		_offset_ids(cJSON_GetObjectItemCaseSensitive(dev, "listen"), off);
	}
	if (id && id[0])
		cJSON_ReplaceItemInObjectCaseSensitive(dev, "id", cJSON_CreateString(id));
	char *out = cJSON_PrintUnformatted(dev);
	cJSON_Delete(dev);
	return out;
}

void cemu_legacy_compose(const cemu_legacy_t *slots, int n, uint32_t can_id,
                         uint8_t out[8]) {
	memset(out, 0, 8);
	for (int i = 0; i < n; i++) {
		const cemu_legacy_t *l = &slots[i];
		if (!l->owner || l->can_id != can_id || !l->on) continue;
		cemu_put_bits(out, l->bit_start, l->bit_length, _max_raw(l->bit_length), l->endian);
	}
}
