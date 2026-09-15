/* test_can_emu.c — the dash as a keypad or IO box (can_emu_core.c), on the host.
 *
 * What the ECU sees is bytes, so bytes are what this asserts. The Haltech
 * IO 12 Expander frames are checked against PT Motorsport's open-source
 * emulator (github.com/ptmotorsport, the CAN-IO Nano sketch), which runs on
 * Haltech ECUs today:
 *
 *   analog inputs  0x2C0, four 12-bit counts (0-4095 = 0-5 V), each in a
 *                  big-endian 16-bit slot, every 20 ms
 *   switch inputs  0x2C2 bytes 0 and 4, 0x2C4 bytes 0 and 4: 250 on, 0 off
 *   keep-alive     0x2C6, 5 bytes 10 09 0D 01 00, every 100 ms
 *   outputs        0x2D0 / 0x2D2 from the ECU, bytes 0 and 4
 *   Box B          every ID + 1
 *
 * The templates are read from main/can/can_emu_templates.json itself — the
 * same file the firmware embeds and the web editor is served.
 */
#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "can/can_emu_core.h"

static char *slurp(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)sz + 1);
	size_t rd = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[rd] = '\0';
	return buf;
}

static char *s_templates;
static cemu_model_t s_m;
static char s_err[160];

static void assert_bytes(const uint8_t *want, const uint8_t *got, int n) {
	for (int i = 0; i < n; i++) {
		if (want[i] != got[i]) {
			printf("  byte %d: want %02X got %02X  (", i, want[i], got[i]);
			for (int k = 0; k < n; k++) printf("%02X ", got[k]);
			printf(")\n");
		}
		TEST_ASSERT_EQUAL_HEX(want[i], got[i]);
	}
}

/* One device file holding a template instance. */
static void load_template(const char *tpl, const char *variant) {
	if (!s_templates) s_templates = slurp("../../main/can/can_emu_templates.json");
	TEST_ASSERT_NOT_NULL(s_templates);
	char *dev = cemu_template_device(s_templates, tpl, variant, NULL, s_err, sizeof(s_err));
	if (!dev) printf("  %s\n", s_err);
	TEST_ASSERT_NOT_NULL(dev);
	size_t cap = strlen(dev) + 32;
	char *file = malloc(cap);
	snprintf(file, cap, "{\"v\":1,\"devices\":[%s]}", dev);
	free(dev);
	bool ok = cemu_parse(file, &s_m, s_err, sizeof(s_err));
	if (!ok) printf("  %s\n", s_err);
	TEST_ASSERT_TRUE(ok);
	free(file);
}

static cemu_send_t *frame(uint32_t id) {
	for (int i = 0; i < s_m.devices[0].n_send; i++)
		if (s_m.devices[0].send[i].can_id == id) return &s_m.devices[0].send[i];
	return NULL;
}

static cemu_control_t *control(const char *ref, int *state) {
	int d, c;
	if (!cemu_resolve(&s_m, ref, &d, &c, state)) { printf("  can't resolve %s\n", ref); exit(1); }
	return &s_m.devices[d].controls[c];
}

static uint8_t compose(uint32_t id, uint8_t out[8]) {
	cemu_send_t *s = frame(id);
	if (!s) { printf("  no frame 0x%X\n", (unsigned)id); exit(1); }
	return cemu_compose(&s_m.devices[0], s, NULL, NULL, out);
}

/* ── Haltech IO Box ─────────────────────────────────────────────────────── */

static void test_cruise_ladder_is_twelve_bit_big_endian_volts(void) {
	load_template("haltech_cruise", "A");
	uint8_t f[8];
	int st;

	TEST_ASSERT_EQUAL_INT(8, compose(0x2C0, f));
	assert_bytes((uint8_t[]){0, 0, 0, 0, 0, 0, 0, 0}, f, 8);          /* idle = 0 V */

	cemu_control_t *c = control("cruise_a:cruise:set", &st);
	cemu_press(&s_m, c, st);
	compose(0x2C0, f);
	assert_bytes((uint8_t[]){0x0F, 0xFF, 0, 0, 0, 0, 0, 0}, f, 8);    /* 5.00 V = 4095 */

	int res;
	control("cruise_a:cruise:resume", &res);
	cemu_press(&s_m, c, res);                                          /* second finger */
	compose(0x2C0, f);
	assert_bytes((uint8_t[]){0x08, 0x00, 0, 0, 0, 0, 0, 0}, f, 8);    /* 2.50 V = 2048 */

	cemu_release(c, res);                                              /* back to the one still held */
	compose(0x2C0, f);
	assert_bytes((uint8_t[]){0x0F, 0xFF, 0, 0, 0, 0, 0, 0}, f, 8);

	cemu_release(c, st);
	int can;
	control("cruise_a:cruise:cancel", &can);
	cemu_press(&s_m, c, can);
	compose(0x2C0, f);
	assert_bytes((uint8_t[]){0x0D, 0xBA, 0, 0, 0, 0, 0, 0}, f, 8);    /* 4.29 V x 819 = 3513.5 -> 3514 */

	cemu_release_all(&s_m);
	compose(0x2C0, f);
	assert_bytes((uint8_t[]){0, 0, 0, 0, 0, 0, 0, 0}, f, 8);
}

static void test_switch_input_is_250_and_latches(void) {
	load_template("haltech_cruise", "A");
	uint8_t f[8];
	int st;
	cemu_control_t *on = control("cruise_a:cruise_on", &st);
	TEST_ASSERT_EQUAL_INT(-1, st);

	compose(0x2C2, f);
	TEST_ASSERT_EQUAL_HEX(0x00, f[0]);
	cemu_press(&s_m, on, st);
	cemu_release(on, st);                     /* a latch ignores the lift */
	compose(0x2C2, f);
	assert_bytes((uint8_t[]){0xFA, 0, 0, 0, 0, 0, 0, 0}, f, 8);
	cemu_release_all(&s_m);                   /* and survives "release everything" */
	compose(0x2C2, f);
	TEST_ASSERT_EQUAL_HEX(0xFA, f[0]);
	cemu_press(&s_m, on, st);
	compose(0x2C2, f);
	TEST_ASSERT_EQUAL_HEX(0x00, f[0]);
}

static void test_keep_alive_is_five_bytes(void) {
	load_template("haltech_cruise", "A");
	uint8_t f[8];
	TEST_ASSERT_EQUAL_INT(5, compose(0x2C6, f));
	assert_bytes((uint8_t[]){0x10, 0x09, 0x0D, 0x01, 0x00}, f, 5);
	TEST_ASSERT_EQUAL_INT(100, frame(0x2C6)->every_ms);
	TEST_ASSERT_EQUAL_INT(20, frame(0x2C0)->every_ms);
	TEST_ASSERT_EQUAL_INT(1000, s_m.devices[0].bitrate_k);
}

static void test_box_b_is_every_id_plus_one(void) {
	load_template("haltech_io12", "B");
	const cemu_device_t *d = &s_m.devices[0];
	TEST_ASSERT_EQUAL_STRING("iobox_b", d->id);
	TEST_ASSERT_EQUAL_STRING("IO Box B", d->name);
	TEST_ASSERT_NOT_NULL(frame(0x2C1));
	TEST_ASSERT_NOT_NULL(frame(0x2C3));
	TEST_ASSERT_NOT_NULL(frame(0x2C5));
	TEST_ASSERT_NOT_NULL(frame(0x2C7));
	TEST_ASSERT_NULL(frame(0x2C0));
	TEST_ASSERT_EQUAL_HEX(0x2D1, d->listen[0].can_id);
	TEST_ASSERT_EQUAL_HEX(0x2D3, d->listen[1].can_id);
	TEST_ASSERT_EQUAL_STRING("IOB_DPO1", d->listen[0].channels[0].name);
	TEST_ASSERT_EQUAL_INT(32, d->listen[0].channels[1].bit_start);
}

static void test_four_analog_inputs_share_one_frame(void) {
	load_template("haltech_io12", "A");
	uint8_t f[8];
	int st;
	cemu_control_t *a2 = control("iobox_a:avi2:pressed", &st);
	cemu_control_t *a4 = control("iobox_a:avi4:pressed", &st);
	cemu_press(&s_m, a2, st);
	cemu_press(&s_m, a4, st);
	compose(0x2C0, f);
	assert_bytes((uint8_t[]){0, 0, 0x0F, 0xFF, 0, 0, 0x0F, 0xFF}, f, 8);
	/* switch 4 lives in byte 4 of the second switch frame */
	cemu_control_t *d4 = control("iobox_a:dpi4", &st);
	cemu_press(&s_m, d4, st);
	compose(0x2C4, f);
	assert_bytes((uint8_t[]){0, 0, 0, 0, 0xFA, 0, 0, 0}, f, 8);
}

/* ── the format ─────────────────────────────────────────────────────────── */

static bool parse(const char *json) {
	return cemu_parse(json, &s_m, s_err, sizeof(s_err));
}

static void test_refuses_what_would_put_wrong_bytes_on_a_bus(void) {
	/* runs off the end of a 5-byte frame */
	TEST_ASSERT_FALSE(parse("{\"devices\":[{\"id\":\"d\",\"send\":[{\"can_id\":1,\"dlc\":5,"
	                        "\"fields\":[{\"bit_start\":32,\"bit_length\":16}]}]}]}"));
	TEST_ASSERT_NOT_NULL(strstr(s_err, "runs past the end"));
	/* two fields on the same bits */
	TEST_ASSERT_FALSE(parse("{\"devices\":[{\"id\":\"d\",\"send\":[{\"can_id\":1,"
	                        "\"fields\":[{\"bit_start\":0,\"bit_length\":8},"
	                        "{\"bit_start\":4,\"bit_length\":8}]}]}]}"));
	TEST_ASSERT_NOT_NULL(strstr(s_err, "overlaps"));
	/* a 29-bit number on an 11-bit frame */
	TEST_ASSERT_FALSE(parse("{\"devices\":[{\"id\":\"d\",\"send\":[{\"can_id\":\"0x18EF0021\"}]}]}"));
	TEST_ASSERT_NOT_NULL(strstr(s_err, "extd"));
	TEST_ASSERT_TRUE(parse("{\"devices\":[{\"id\":\"d\",\"send\":[{\"can_id\":\"0x18EF0021\",\"extd\":true}]}]}"));
	/* a value table that doesn't match the control's states */
	TEST_ASSERT_FALSE(parse("{\"devices\":[{\"id\":\"d\",\"controls\":[{\"id\":\"c\",\"kind\":\"select\","
	                        "\"states\":[\"idle\",\"a\",\"b\"]}],\"send\":[{\"can_id\":1,"
	                        "\"fields\":[{\"bit_start\":0,\"bit_length\":8,\"control\":\"c\","
	                        "\"values\":[0,1]}]}]}]}"));
	TEST_ASSERT_NOT_NULL(strstr(s_err, "3 states"));
	/* a field pointing at a control that doesn't exist */
	TEST_ASSERT_FALSE(parse("{\"devices\":[{\"id\":\"d\",\"send\":[{\"can_id\":1,"
	                        "\"fields\":[{\"bit_start\":0,\"bit_length\":8,\"control\":\"nope\"}]}]}]}"));
	/* two devices sending one ID */
	TEST_ASSERT_FALSE(parse("{\"devices\":[{\"id\":\"a\",\"send\":[{\"can_id\":704}]},"
	                        "{\"id\":\"b\",\"send\":[{\"can_id\":704}]}]}"));
	TEST_ASSERT_NOT_NULL(strstr(s_err, "both send"));
	/* a frame repeated faster than 10 ms */
	TEST_ASSERT_FALSE(parse("{\"devices\":[{\"id\":\"d\",\"send\":[{\"can_id\":1,\"every_ms\":2}]}]}"));
	/* a refused file leaves nothing half-loaded */
	TEST_ASSERT_EQUAL_INT(0, s_m.n_devices);
	/* no devices at all is fine */
	TEST_ASSERT_TRUE(parse("{\"v\":1,\"devices\":[]}"));
	TEST_ASSERT_FALSE(parse("not json"));
}

static void test_a_field_with_no_table_is_off_or_full(void) {
	TEST_ASSERT_TRUE(parse("{\"devices\":[{\"id\":\"k\",\"controls\":[{\"id\":\"b1\"},{\"id\":\"b2\"}],"
	                       "\"send\":[{\"can_id\":405,\"dlc\":2,\"fields\":["
	                       "{\"bit_start\":0,\"bit_length\":1,\"control\":\"b1\"},"
	                       "{\"bit_start\":1,\"bit_length\":1,\"control\":\"b2\"},"
	                       "{\"bit_start\":8,\"bit_length\":8,\"counter\":true}]}]}]}"));
	uint8_t f[8];
	int st;
	cemu_control_t *b2 = control("k:b2", &st);
	cemu_press(&s_m, b2, st);
	TEST_ASSERT_EQUAL_INT(2, compose(405, f));
	assert_bytes((uint8_t[]){0x02, 0x00}, f, 2);
	compose(405, f);
	assert_bytes((uint8_t[]){0x02, 0x01}, f, 2);   /* counter ticks per frame */
	cemu_release(b2, st);
	/* a status readout peeks without taking a count from the real sequence */
	cemu_peek(&s_m.devices[0], frame(405), NULL, NULL, f);
	cemu_peek(&s_m.devices[0], frame(405), NULL, NULL, f);
	assert_bytes((uint8_t[]){0x00, 0x02}, f, 2);
	compose(405, f);
	assert_bytes((uint8_t[]){0x00, 0x02}, f, 2);
}

static bool fake_channel(const char *name, float *v, void *user) {
	(void)user;
	if (!strcmp(name, "FUEL_LEVEL")) { *v = 42.4f; return true; }
	return false;
}

static void test_a_channel_field_sends_the_live_value(void) {
	TEST_ASSERT_TRUE(parse("{\"devices\":[{\"id\":\"f\",\"send\":[{\"can_id\":1776,\"fields\":["
	                       "{\"bit_start\":0,\"bit_length\":16,\"endian\":1,\"scale\":0.1,\"channel\":\"FUEL_LEVEL\"},"
	                       "{\"bit_start\":16,\"bit_length\":8,\"channel\":\"MISSING\",\"value\":7}]}]}]}"));
	uint8_t f[8];
	cemu_compose(&s_m.devices[0], &s_m.devices[0].send[0], fake_channel, NULL, f);
	assert_bytes((uint8_t[]){0xA8, 0x01, 0x07}, f, 3);   /* 424 LE, then the no-data value */
}

static void test_values_clamp_rather_than_wrap(void) {
	TEST_ASSERT_EQUAL_UINT(4095, cemu_to_raw(9.0f, 0.001221001f, 0, 12));
	TEST_ASSERT_EQUAL_UINT(0, cemu_to_raw(-1.0f, 1, 0, 8));
	TEST_ASSERT_EQUAL_UINT(255, cemu_to_raw(300, 1, 0, 8));
	TEST_ASSERT_EQUAL_UINT(819, cemu_to_raw(1.0f, 0.001221001f, 0, 16));   /* Tom's 1 V = 03 33 */
}

static void test_a_range_keeps_a_value_inside_what_the_input_carries(void) {
	load_template("haltech_io12", "A");
	uint8_t f[8];
	int st;
	cemu_control_t *a1 = control("iobox_a:avi1:pressed", &st);
	frame(0x2C0)->fields[0].values[1] = 12.0f;         /* 12 V on a 0-5 V input */
	cemu_press(&s_m, a1, st);
	compose(0x2C0, f);
	assert_bytes((uint8_t[]){0x0F, 0xFF}, f, 2);        /* 5 V, not 16-bit full scale */
	TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, frame(0x2C0)->fields[0].max);
}

static void test_put_bits_replaces_the_base(void) {
	uint8_t d[8] = {0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0};
	cemu_put_bits(d, 4, 8, 0x00, 1);
	assert_bytes((uint8_t[]){0x0F, 0xF0, 0xFF}, d, 3);
	cemu_put_bits(d, 0, 16, 0x1234, 0);
	assert_bytes((uint8_t[]){0x12, 0x34, 0xFF}, d, 3);
}

/* ── the old per-widget outputs ─────────────────────────────────────────── */

static void test_two_buttons_on_one_id_keep_each_others_bits(void) {
	int a = 1, b = 2;
	cemu_legacy_t slots[2] = {
		{ .owner = &a, .can_id = 0x500, .bit_start = 0, .bit_length = 1, .endian = 1, .on = true },
		{ .owner = &b, .can_id = 0x500, .bit_start = 1, .bit_length = 1, .endian = 1, .on = true },
	};
	uint8_t f[8];
	cemu_legacy_compose(slots, 2, 0x500, f);
	TEST_ASSERT_EQUAL_HEX(0x03, f[0]);   /* was 0x01 or 0x02, whichever sent last */
	slots[0].on = false;
	cemu_legacy_compose(slots, 2, 0x500, f);
	TEST_ASSERT_EQUAL_HEX(0x02, f[0]);
	cemu_legacy_compose(slots, 2, 0x501, f);
	TEST_ASSERT_EQUAL_HEX(0x00, f[0]);
}

static void test_resolve_names_states_exactly(void) {
	load_template("haltech_cruise", "B");
	int d, c, s;
	TEST_ASSERT_TRUE(cemu_resolve(&s_m, "cruise_b:cruise:resume", &d, &c, &s));
	TEST_ASSERT_EQUAL_INT(2, s);
	TEST_ASSERT_FALSE(cemu_resolve(&s_m, "cruise_b:cruise:accel", &d, &c, &s));
	TEST_ASSERT_FALSE(cemu_resolve(&s_m, "cruise_a:cruise:set", &d, &c, &s));
	TEST_ASSERT_FALSE(cemu_resolve(&s_m, "cruise_b", &d, &c, &s));
	TEST_ASSERT_NULL(cemu_template_device(s_templates, "haltech_io12", "C", NULL, s_err, sizeof(s_err)));
}

static void test_every_template_parses(void) {
	if (!s_templates) s_templates = slurp("../../main/can/can_emu_templates.json");
	TEST_ASSERT_NOT_NULL(s_templates);
	load_template("haltech_cruise", "A");
	load_template("haltech_cruise", "B");
	load_template("haltech_io12", "A");
	load_template("haltech_io12", "B");
	load_template("blank", NULL);
	/* Box A and Box B together are legal — different IDs */
	char *a = cemu_template_device(s_templates, "haltech_io12", "A", NULL, s_err, sizeof(s_err));
	char *b = cemu_template_device(s_templates, "haltech_cruise", "B", NULL, s_err, sizeof(s_err));
	char file[16000];
	snprintf(file, sizeof(file), "{\"devices\":[%s,%s]}", a, b);
	TEST_ASSERT_TRUE(parse(file));
	/* ...but two Box A's are not */
	char *a2 = cemu_template_device(s_templates, "haltech_cruise", "A", NULL, s_err, sizeof(s_err));
	snprintf(file, sizeof(file), "{\"devices\":[%s,%s]}", a, a2);
	TEST_ASSERT_FALSE(parse(file));
	free(a); free(b); free(a2);
}

int main(void) {
	UNITY_BEGIN();
	RUN_TEST(test_cruise_ladder_is_twelve_bit_big_endian_volts);
	RUN_TEST(test_switch_input_is_250_and_latches);
	RUN_TEST(test_keep_alive_is_five_bytes);
	RUN_TEST(test_box_b_is_every_id_plus_one);
	RUN_TEST(test_four_analog_inputs_share_one_frame);
	RUN_TEST(test_refuses_what_would_put_wrong_bytes_on_a_bus);
	RUN_TEST(test_a_field_with_no_table_is_off_or_full);
	RUN_TEST(test_a_channel_field_sends_the_live_value);
	RUN_TEST(test_values_clamp_rather_than_wrap);
	RUN_TEST(test_a_range_keeps_a_value_inside_what_the_input_carries);
	RUN_TEST(test_put_bits_replaces_the_base);
	RUN_TEST(test_two_buttons_on_one_id_keep_each_others_bits);
	RUN_TEST(test_resolve_names_states_exactly);
	RUN_TEST(test_every_template_parses);
	free(s_templates);
	return UNITY_END();
}
