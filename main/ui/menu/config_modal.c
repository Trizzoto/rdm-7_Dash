/*
 * config_modal.c — single centred tabbed settings modal
 *
 * Replaces the two-panel layout previously used for panels (1-8) and bars
 * (12-13).  Three tabs:
 *   CAN SIGNAL   – CAN ID, Endian, Bit Start/Length, Scale, Offset, Type
 *   DISPLAY      – Label, range, decimals, show/invert, fuel sender
 *   ALERTS       – Enable Warnings master toggle + threshold + colour rows
 *
 * A "LOAD PRESET" button at the top-right opens a full-screen picker
 * (implemented in ui_preconfig.c).
 */

#include "config_modal.h"
#include "../callbacks/ui_callbacks.h"
#include "../screens/ui_Screen3.h"
#include "../settings/settings_panel.h"
#include "../theme.h"
#include "config_bridge.h"
#include "menu_screen.h"
#include "preset_picker.h"
#include <stdio.h>
#include <string.h>


/* ── external widget-array references (defined in ui_Screen3.c) ──────────── */
extern lv_obj_t *g_label_input[];
extern lv_obj_t *g_can_id_input[];
extern lv_obj_t *g_endian_dropdown[];
extern lv_obj_t *g_bit_start_dropdown[];
extern lv_obj_t *g_bit_length_dropdown[];
extern lv_obj_t *g_scale_input[];
extern lv_obj_t *g_offset_input[];
extern lv_obj_t *g_decimals_dropdown[];
extern lv_obj_t *g_type_dropdown[];
extern lv_obj_t *keyboard;
extern lv_obj_t *config_bars[];
extern lv_obj_t *ui_MenuScreen;

/* bar / panel alert-colour callbacks live in ui_Screen3.c */
extern void warning_high_threshold_event_cb(lv_event_t *e);
extern void warning_low_threshold_event_cb(lv_event_t *e);
extern void warning_high_color_event_cb(lv_event_t *e);
extern void warning_low_color_event_cb(lv_event_t *e);
extern void bar_range_input_event_cb(lv_event_t *e);
extern void bar_low_value_event_cb(lv_event_t *e);
extern void bar_high_value_event_cb(lv_event_t *e);
extern void bar_low_color_event_cb(lv_event_t *e);
extern void bar_high_color_event_cb(lv_event_t *e);
extern void bar_in_range_color_event_cb(lv_event_t *e);
extern void keyboard_ready_event_cb(lv_event_t *e);
extern float fuel_sender_get_filtered_v(uint8_t bar_idx);
extern lv_obj_t *ui_Value[];		 /* live value labels on Screen3    */
extern char previous_values[13][64]; /* last known value strings         */

/* ── Widget ID constants ─────────────────────────────────────────────────── */
#define RPM_VALUE_ID 9
#define SPEED_VALUE_ID 10
#define GEAR_VALUE_ID 11
#define BAR1_VALUE_ID 12
#define BAR2_VALUE_ID 13

/* RPM option strings */
#define RPM_OPTS                                                               \
	"3000\n3200\n3400\n3600\n3800\n4000\n4200\n4400\n4600\n4800\n"             \
	"5000\n5200\n5400\n5600\n5800\n6000\n6200\n6400\n6600\n6800\n"             \
	"7000\n7200\n7400\n7600\n7800\n8000\n8200\n8400\n8600\n8800\n"             \
	"9000\n9200\n9400\n9600\n9800\n10000\n10200\n10400\n10600\n10800\n"        \
	"11000\n11200\n11400\n11600\n11800\n12000"

#define RPM_COLOR_OPTS                                                         \
	"Green\nLight Blue\nYellow\nOrange\nRed\nDark "                            \
	"Blue\nPurple\nMagenta\nPink\nCustom"

/* ── Additional externs for RPM / Speed / Gear ───────────────────────────── */
extern lv_obj_t *menu_rpm_value_label;
extern lv_obj_t *menu_speed_value_label;
extern lv_obj_t *menu_speed_units_label;
extern lv_obj_t *menu_gear_value_label;
extern lv_obj_t *ui_RPM_Value, *ui_Speed_Value, *ui_Kmh, *ui_GEAR_Value;
extern int rpm_gauge_max, rpm_redline_value;

/* RPM callbacks */
extern void rpm_ecu_dropdown_event_cb(lv_event_t *e);
extern void rpm_color_dropdown_event_cb(lv_event_t *e);
extern void rpm_gauge_roller_event_cb(lv_event_t *e);
extern void rpm_redline_roller_event_cb(lv_event_t *e);
extern void rpm_limiter_effect_dropdown_event_cb(lv_event_t *e);
extern void rpm_limiter_roller_event_cb(lv_event_t *e);
extern void rpm_limiter_color_dropdown_event_cb(lv_event_t *e);
extern void rpm_background_switch_event_cb(lv_event_t *e);
extern void rpm_background_color_dropdown_event_cb(lv_event_t *e);
extern void rpm_background_threshold_roller_event_cb(lv_event_t *e);

/* Speed callback */
extern void speed_units_dropdown_event_cb(lv_event_t *e);

/* Gear callbacks */
extern void gear_ecu_dropdown_event_cb(lv_event_t *e);
extern void create_custom_gear_values_section(lv_obj_t *parent,
											  uint8_t gear_mode);
extern void hide_custom_gear_values_section(void);

/* ── Live preview panel timer ────────────────────────────────────────────── */
typedef struct {
	uint8_t value_id;
	lv_obj_t *val_lbl;
} prev_ctx_t;

static void panel_preview_timer_cb(lv_timer_t *t) {
	prev_ctx_t *ctx = (prev_ctx_t *)t->user_data;
	if (!ctx || !lv_obj_is_valid(ctx->val_lbl))
		return;
	uint8_t idx = ctx->value_id - 1;
	if (ui_Value[idx] && lv_obj_is_valid(ui_Value[idx])) {
		const char *v = lv_label_get_text(ui_Value[idx]);
		if (v && v[0] != '\0')
			lv_label_set_text(ctx->val_lbl, v);
	}
}
static void prev_timer_del_cb(lv_event_t *e) {
	lv_timer_t *t = (lv_timer_t *)lv_event_get_user_data(e);
	if (t)
		lv_timer_del(t);
}
static void prev_ctx_del_cb(lv_event_t *e) {
	prev_ctx_t *ctx = (prev_ctx_t *)lv_event_get_user_data(e);
	if (ctx)
		lv_mem_free(ctx);
}

/* ── layout constants ────────────────────────────────────────────────────── */
#define MODAL_W 778					  /* screen 800 − 11px margins */
#define MODAL_H 458					  /* screen 480 − 11px margins */
#define PREV_W 192					  /* live-preview pane width   */
#define SETTINGS_W (MODAL_W - PREV_W) /* 586 */
#define HDR_H 52
#define FOOTER_H 50
#define TABBAR_H 42

/* ── bit-field option strings ────────────────────────────────────────────── */
static const char *BIT_START_OPTS =
	"0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n"
	"16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n"
	"32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n"
	"48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60\n61\n62\n63";

static const char *BIT_LEN_OPTS =
	"1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n"
	"17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n"
	"33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n"
	"49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60\n61\n62\n63\n64";

#define BAR_COLOR_OPTS                                                         \
	"Blue\nRed\nGreen\nYellow\nOrange\nPurple\nCyan\nMagenta\nCustom"

static uint8_t bar_color_idx(lv_color_t c) {
	lv_color_t p[] = {THEME_COLOR_BLUE_DARK,	THEME_COLOR_RED,
					  THEME_COLOR_GREEN_BRIGHT, THEME_COLOR_YELLOW,
					  THEME_COLOR_ORANGE,		THEME_COLOR_PURPLE,
					  THEME_COLOR_CYAN,			THEME_COLOR_MAGENTA};
	for (int i = 0; i < 8; i++)
		if (c.full == p[i].full)
			return i;
	return 8;
}

static uint8_t rpm_color_idx(lv_color_t c) {
	lv_color_t p[] = {
		THEME_COLOR_GREEN,	THEME_COLOR_CYAN,	 THEME_COLOR_YELLOW,
		THEME_COLOR_ORANGE, THEME_COLOR_RED,	 THEME_COLOR_BLUE,
		THEME_COLOR_PURPLE, THEME_COLOR_MAGENTA, THEME_COLOR_PINK};
	for (int i = 0; i < 9; i++)
		if (c.full == p[i].full)
			return i;
	return 9;
}

/* ── small helpers ───────────────────────────────────────────────────────── */

static uint8_t *id_alloc(uint8_t v) {
	uint8_t *p = lv_mem_alloc(sizeof(uint8_t));
	*p = v;
	return p;
}

/* Style a tab content page: surface bg + column flex + scrollable. */
static void style_tab(lv_obj_t *tab) {
	lv_obj_set_style_bg_color(tab, THEME_COLOR_SURFACE, 0);
	lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(tab, THEME_PAD_NORMAL, 0);
	lv_obj_set_style_pad_row(tab, 4, 0);
	lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
						  LV_FLEX_ALIGN_START);
}

/* ── Warnings toggle callback (Alerts tab) ──────────────────────────────── */

static void warnings_toggle_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
		return;
	lv_obj_t *container = (lv_obj_t *)lv_event_get_user_data(e);
	if (!container)
		return;
	if (lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED))
		lv_obj_clear_flag(container, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
}

/* ── Fuel sender toggle callback (Display tab) ──────────────────────────── */

static void fuel_toggle_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
		return;
	lv_obj_t *container = (lv_obj_t *)lv_event_get_user_data(e);
	if (!container)
		return;
	if (lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED))
		lv_obj_clear_flag(container, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(container, LV_OBJ_FLAG_HIDDEN);
}

/* ── Preset button opens the full-screen picker ─────────────────────────── */

static void preset_btn_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	uint8_t vid = *(uint8_t *)lv_event_get_user_data(e);
	open_preset_picker(lv_scr_act(), vid);
}

/* =========================================================================
 * Tab builders
 * ========================================================================= */

static void build_can_tab(lv_obj_t *tab, uint8_t value_id) {
	uint8_t idx = value_id - 1;
	settings_section_t *sec =
		settings_add_section(tab, "CAN BUS SIGNAL", THEME_COLOR_ACCENT);

	char can_id_str[16];
	snprintf(can_id_str, sizeof(can_id_str), "%X",
			 config_bridge_get_can_id(value_id));
	g_can_id_input[idx] =
		settings_add_text_input(sec, "CAN ID (0x):", "hex", can_id_str);
	lv_obj_add_event_cb(g_can_id_input[idx], keyboard_event_cb, LV_EVENT_ALL,
						NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(g_can_id_input[idx], can_id_input_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(g_can_id_input[idx], free_value_id_event_cb,
							LV_EVENT_DELETE, p);
	}

	g_endian_dropdown[idx] =
		settings_add_dropdown(sec, "Endian:", "Big Endian\nLittle Endian", 0);
	lv_dropdown_set_selected(
		g_endian_dropdown[idx],
		config_bridge_get_endian(value_id) == BIG_ENDIAN_ORDER ? 0 : 1);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(g_endian_dropdown[idx], endianess_roller_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(g_endian_dropdown[idx], free_value_id_event_cb,
							LV_EVENT_DELETE, p);
	}

	g_bit_start_dropdown[idx] =
		settings_add_dropdown(sec, "Bit Start:", BIT_START_OPTS, 0);
	lv_dropdown_set_selected(g_bit_start_dropdown[idx],
							 config_bridge_get_bit_start(value_id));
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(g_bit_start_dropdown[idx],
							bit_start_roller_event_cb, LV_EVENT_VALUE_CHANGED,
							p);
		lv_obj_add_event_cb(g_bit_start_dropdown[idx], free_value_id_event_cb,
							LV_EVENT_DELETE, p);
	}

	g_bit_length_dropdown[idx] =
		settings_add_dropdown(sec, "Bit Length:", BIT_LEN_OPTS, 0);
	lv_dropdown_set_selected(g_bit_length_dropdown[idx],
							 config_bridge_get_bit_length(value_id) - 1);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(g_bit_length_dropdown[idx],
							bit_length_roller_event_cb, LV_EVENT_VALUE_CHANGED,
							p);
		lv_obj_add_event_cb(g_bit_length_dropdown[idx], free_value_id_event_cb,
							LV_EVENT_DELETE, p);
	}

	char scale_str[16];
	snprintf(scale_str, sizeof(scale_str), "%.6g",
			 config_bridge_get_scale(value_id));
	g_scale_input[idx] =
		settings_add_text_input(sec, "Scale:", "factor", scale_str);
	lv_obj_add_event_cb(g_scale_input[idx], keyboard_event_cb, LV_EVENT_ALL,
						NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(g_scale_input[idx], scale_input_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(g_scale_input[idx], free_value_id_event_cb,
							LV_EVENT_DELETE, p);
	}

	char offset_str[16];
	snprintf(offset_str, sizeof(offset_str), "%.6g",
			 config_bridge_get_offset(value_id));
	g_offset_input[idx] =
		settings_add_text_input(sec, "Offset:", "value offset", offset_str);
	lv_obj_add_event_cb(g_offset_input[idx], keyboard_event_cb, LV_EVENT_ALL,
						NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(g_offset_input[idx], value_offset_input_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(g_offset_input[idx], free_value_id_event_cb,
							LV_EVENT_DELETE, p);
	}

	g_type_dropdown[idx] =
		settings_add_dropdown(sec, "Data Type:", "Unsigned\nSigned", 0);
	lv_dropdown_set_selected(g_type_dropdown[idx],
							 config_bridge_get_is_signed(value_id) ? 1 : 0);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(g_type_dropdown[idx], type_dropdown_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(g_type_dropdown[idx], free_value_id_event_cb,
							LV_EVENT_DELETE, p);
	}
}

/* ─────────────────────────────────────────────────────────────────────────
 * Display tab — panels (value_id 1–8)
 * ─────────────────────────────────────────────────────────────────────────*/

static void build_display_tab_panel(lv_obj_t *tab, uint8_t value_id) {
	uint8_t idx = value_id - 1;
	settings_section_t *sec =
		settings_add_section(tab, "DISPLAY OPTIONS", THEME_COLOR_ACCENT_TEAL);

	g_label_input[idx] = settings_add_text_input(
		sec, "Label:", "panel label", config_bridge_get_label(value_id));
	lv_obj_add_event_cb(g_label_input[idx], keyboard_event_cb, LV_EVENT_ALL,
						NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(g_label_input[idx], label_input_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(g_label_input[idx], free_value_id_event_cb,
							LV_EVENT_DELETE, p);
	}

	lv_obj_t *du = settings_add_text_input(
		sec, "Display Unit:", "suffix",
		config_bridge_get_custom_text(value_id));
	lv_obj_add_event_cb(du, keyboard_event_cb, LV_EVENT_ALL, NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(du, custom_text_input_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(du, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	g_decimals_dropdown[idx] =
		settings_add_dropdown(sec, "Decimals:", "0\n1\n2\n3", 0);
	lv_dropdown_set_selected(g_decimals_dropdown[idx],
							 config_bridge_get_decimals(value_id));
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(g_decimals_dropdown[idx], decimal_dropdown_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(g_decimals_dropdown[idx], free_value_id_event_cb,
							LV_EVENT_DELETE, p);
	}
}

/* ─────────────────────────────────────────────────────────────────────────
 * Display tab — bars (value_id 12–13)
 * ─────────────────────────────────────────────────────────────────────────*/

static void build_display_tab_bar(lv_obj_t *tab, uint8_t value_id) {
	uint8_t idx = value_id - 1;
	uint8_t bar_idx = (value_id == 12) ? 0 : 1;
	settings_section_t *sec =
		settings_add_section(tab, "DISPLAY OPTIONS", THEME_COLOR_ACCENT_TEAL);

	g_label_input[idx] = settings_add_text_input(
		sec, "Label:", "bar label", config_bridge_get_label(value_id));
	lv_obj_add_event_cb(g_label_input[idx], keyboard_event_cb, LV_EVENT_ALL,
						NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(g_label_input[idx], label_input_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(g_label_input[idx], free_value_id_event_cb,
							LV_EVENT_DELETE, p);
	}

	g_decimals_dropdown[idx] =
		settings_add_dropdown(sec, "Decimals:", "0\n1\n2\n3", 0);
	lv_dropdown_set_selected(g_decimals_dropdown[idx],
							 config_bridge_get_decimals(value_id));
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(g_decimals_dropdown[idx], decimal_dropdown_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(g_decimals_dropdown[idx], free_value_id_event_cb,
							LV_EVENT_DELETE, p);
	}

	char buf[16];
	snprintf(buf, sizeof(buf), "%d",
			 (int)config_bridge_get_bar_min(value_id));
	lv_obj_t *bmin = settings_add_text_input(sec, "Min Value:", "min", buf);
	lv_obj_add_event_cb(bmin, keyboard_event_cb, LV_EVENT_ALL, NULL);
	snprintf(buf, sizeof(buf), "%d",
			 (int)config_bridge_get_bar_max(value_id));
	lv_obj_t *bmax = settings_add_text_input(sec, "Max Value:", "max", buf);
	lv_obj_add_event_cb(bmax, keyboard_event_cb, LV_EVENT_ALL, NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(bmin, bar_range_input_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(bmax, bar_range_input_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
	}

	lv_obj_t *sv = settings_add_switch(
		sec, "Show Value:", config_bridge_get_show_bar_value(value_id));
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(sv, show_value_switch_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(sv, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	lv_obj_t *iv = settings_add_switch(
		sec, "Invert Value:", config_bridge_get_invert_bar_value(value_id));
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(iv, invert_value_switch_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(iv, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	/* ── Fuel Sender ── */
	bool fs_on = config_bridge_get_fuel_sender(value_id);
	lv_obj_t *fss = settings_add_switch(sec, "Fuel Sender:", fs_on);

	/* Sub-container — hidden when fuel sender is off */
	lv_obj_t *fs_box = lv_obj_create(tab);
	lv_obj_set_size(fs_box, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(fs_box, THEME_COLOR_SECTION_BG, 0);
	lv_obj_set_style_bg_opa(fs_box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(fs_box, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(fs_box, 1, 0);
	lv_obj_set_style_radius(fs_box, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_pad_all(fs_box, 6, 0);
	lv_obj_set_style_pad_row(fs_box, 4, 0);
	lv_obj_clear_flag(fs_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(fs_box, LV_FLEX_FLOW_COLUMN);
	if (!fs_on)
		lv_obj_add_flag(fs_box, LV_OBJ_FLAG_HIDDEN);

	lv_obj_add_event_cb(fss, fuel_toggle_cb, LV_EVENT_VALUE_CHANGED, fs_box);

	/* Calibration rows inside fs_box */
	settings_section_t *fs_sec = fs_box; /* treat as inline section */

	char vb[12];
	snprintf(vb, sizeof(vb), "%.2f",
			 config_bridge_get_fuel_sender_empty_v(value_id));
	lv_obj_t *fs_ei =
		settings_add_text_input(fs_sec, "Empty Voltage:", "V", vb);
	lv_obj_add_event_cb(fs_ei, keyboard_event_cb, LV_EVENT_ALL, NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(fs_ei, fs_empty_v_input_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(fs_ei, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	snprintf(vb, sizeof(vb), "%.2f",
			 config_bridge_get_fuel_sender_full_v(value_id));
	lv_obj_t *fs_fi = settings_add_text_input(fs_sec, "Full Voltage:", "V", vb);
	lv_obj_add_event_cb(fs_fi, keyboard_event_cb, LV_EVENT_ALL, NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(fs_fi, fs_full_v_input_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(fs_fi, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	char cv[32];
	snprintf(cv, sizeof(cv), "%.2f V", fuel_sender_get_filtered_v(bar_idx));
	settings_add_info_row(fs_sec, "Current Reading:", cv);

	char fb[20];
	snprintf(fb, sizeof(fb), "Filter: %d%%",
			 config_bridge_get_fuel_sender_filter(value_id));
	lv_obj_t *fs_flbl = settings_add_info_row(fs_sec, "Filter:", fb);

	lv_obj_t *fs_sl = lv_slider_create(fs_box);
	lv_obj_set_width(fs_sl, lv_pct(94));
	lv_obj_set_height(fs_sl, 12);
	lv_slider_set_range(fs_sl, 0, 100);
	lv_slider_set_value(fs_sl, config_bridge_get_fuel_sender_filter(value_id),
						LV_ANIM_OFF);
	lv_obj_set_style_bg_color(fs_sl, THEME_COLOR_ACCENT,
							  LV_PART_INDICATOR | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(fs_sl, THEME_COLOR_BORDER,
							  LV_PART_MAIN | LV_STATE_DEFAULT);
	lv_obj_set_style_bg_color(fs_sl, THEME_COLOR_ACCENT,
							  LV_PART_KNOB | LV_STATE_DEFAULT);

	/* Calibrate buttons row */
	lv_obj_t *cal_row = lv_obj_create(fs_box);
	lv_obj_set_size(cal_row, lv_pct(100), 36);
	lv_obj_clear_flag(cal_row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_opa(cal_row, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(cal_row, 0, 0);
	lv_obj_set_style_pad_all(cal_row, 0, 0);

	lv_obj_t *fs_eb = lv_btn_create(cal_row);
	lv_obj_set_size(fs_eb, 120, 30);
	lv_obj_align(fs_eb, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_style_bg_color(fs_eb, THEME_COLOR_SCROLLBAR, 0);
	lv_obj_set_style_radius(fs_eb, THEME_RADIUS_SMALL, 0);
	lv_label_set_text(lv_label_create(fs_eb), "Set Empty");
	lv_obj_set_style_text_font(lv_obj_get_child(fs_eb, 0), THEME_FONT_BODY, 0);
	lv_obj_center(lv_obj_get_child(fs_eb, 0));
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(fs_eb, fs_empty_btn_event_cb, LV_EVENT_CLICKED, p);
		lv_obj_add_event_cb(fs_eb, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	lv_obj_t *fs_fb = lv_btn_create(cal_row);
	lv_obj_set_size(fs_fb, 120, 30);
	lv_obj_align(fs_fb, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_bg_color(fs_fb, THEME_COLOR_SCROLLBAR, 0);
	lv_obj_set_style_radius(fs_fb, THEME_RADIUS_SMALL, 0);
	lv_label_set_text(lv_label_create(fs_fb), "Set Full");
	lv_obj_set_style_text_font(lv_obj_get_child(fs_fb, 0), THEME_FONT_BODY, 0);
	lv_obj_center(lv_obj_get_child(fs_fb, 0));
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(fs_fb, fs_full_btn_event_cb, LV_EVENT_CLICKED, p);
		lv_obj_add_event_cb(fs_fb, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	/* Build fuel sender context for timer + slider callbacks */
	fuel_sender_ctx_t *ctx = lv_mem_alloc(sizeof(fuel_sender_ctx_t));
	ctx->value_id = value_id;
	ctx->set_label = NULL; /* not used in modal layout */
	ctx->empty_btn = fs_eb;
	ctx->full_btn = fs_fb;
	ctx->empty_input = fs_ei;
	ctx->full_input = fs_fi;
	ctx->current_label = (lv_obj_t *)fs_flbl; /* reuse filter-label slot */
	ctx->filter_label = (lv_obj_t *)fs_flbl;
	ctx->filter_slider = fs_sl;
	ctx->update_timer = lv_timer_create(fs_voltage_update_timer_cb, 200, ctx);

	lv_obj_add_event_cb(fs_sl, fs_filter_slider_event_cb,
						LV_EVENT_VALUE_CHANGED, ctx);
	lv_obj_add_event_cb(fs_sl, fs_filter_slider_event_cb, LV_EVENT_RELEASED,
						ctx);
	lv_obj_add_event_cb(fss, fuel_sender_switch_event_cb,
						LV_EVENT_VALUE_CHANGED, ctx);
	lv_obj_add_event_cb(fss, fuel_sender_ctx_free_event_cb, LV_EVENT_DELETE,
						ctx);

	/* Also wire the fuel-toggle cb to show/hide fs_box via the switch */
	lv_obj_add_event_cb(fss, fuel_toggle_cb, LV_EVENT_VALUE_CHANGED, fs_box);
}

/* ─────────────────────────────────────────────────────────────────────────
 * Alerts tab — panels (1-8)
 * ─────────────────────────────────────────────────────────────────────────*/

static void build_alerts_tab_panel(lv_obj_t *tab, uint8_t value_id) {
	uint8_t idx = value_id - 1;
	bool has_warnings =
		(config_bridge_get_warning_high_threshold(value_id) != 0.0f ||
		 config_bridge_get_warning_low_threshold(value_id) != 0.0f);

	settings_section_t *sec =
		settings_add_section(tab, "WARNING SETTINGS", THEME_COLOR_ACCENT_AMBER);

	lv_obj_t *en_sw =
		settings_add_switch(sec, "Enable Warnings:", has_warnings);

	/* Sub-container hidden when warnings are off */
	lv_obj_t *warn_box = lv_obj_create(tab);
	lv_obj_set_size(warn_box, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(warn_box, THEME_COLOR_SECTION_BG, 0);
	lv_obj_set_style_bg_opa(warn_box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(warn_box, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(warn_box, 1, 0);
	lv_obj_set_style_radius(warn_box, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_pad_all(warn_box, 6, 0);
	lv_obj_set_style_pad_row(warn_box, 4, 0);
	lv_obj_clear_flag(warn_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(warn_box, LV_FLEX_FLOW_COLUMN);
	if (!has_warnings)
		lv_obj_add_flag(warn_box, LV_OBJ_FLAG_HIDDEN);

	lv_obj_add_event_cb(en_sw, warnings_toggle_cb, LV_EVENT_VALUE_CHANGED,
						warn_box);

	settings_section_t *ws = warn_box;

	char buf[20];
	snprintf(buf, sizeof(buf), "%.2f",
			 config_bridge_get_warning_high_threshold(value_id));
	lv_obj_t *wh = settings_add_text_input(ws, "Range High:", "value", buf);
	lv_obj_add_event_cb(wh, keyboard_event_cb, LV_EVENT_ALL, NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(wh, warning_high_threshold_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(wh, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	lv_obj_t *whc = settings_add_dropdown(ws, "High Colour:", "Red\nBlue", 0);
	lv_dropdown_set_selected(
		whc, config_bridge_get_warning_high_color(value_id).full ==
					 THEME_COLOR_RED.full
				 ? 0
				 : 1);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(whc, warning_high_color_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(whc, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	snprintf(buf, sizeof(buf), "%.2f",
			 config_bridge_get_warning_low_threshold(value_id));
	lv_obj_t *wl = settings_add_text_input(ws, "Range Low:", "value", buf);
	lv_obj_add_event_cb(wl, keyboard_event_cb, LV_EVENT_ALL, NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(wl, warning_low_threshold_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(wl, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	lv_obj_t *wlc = settings_add_dropdown(ws, "Low Colour:", "Red\nBlue", 0);
	lv_dropdown_set_selected(
		wlc, config_bridge_get_warning_low_color(value_id).full ==
					 THEME_COLOR_RED.full
				 ? 0
				 : 1);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(wlc, warning_low_color_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(wlc, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}
}

/* ─────────────────────────────────────────────────────────────────────────
 * Alerts tab — bars (12-13)
 * ─────────────────────────────────────────────────────────────────────────*/

static void build_alerts_tab_bar(lv_obj_t *tab, uint8_t value_id) {
	uint8_t idx = value_id - 1;
	bool has_warnings = (config_bridge_get_bar_low(value_id) != 0 ||
						 config_bridge_get_bar_high(value_id) != 0);

	settings_section_t *sec =
		settings_add_section(tab, "BAR THRESHOLDS", THEME_COLOR_ACCENT_AMBER);

	lv_obj_t *en_sw =
		settings_add_switch(sec, "Enable Warnings:", has_warnings);

	lv_obj_t *warn_box = lv_obj_create(tab);
	lv_obj_set_size(warn_box, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(warn_box, THEME_COLOR_SECTION_BG, 0);
	lv_obj_set_style_bg_opa(warn_box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(warn_box, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(warn_box, 1, 0);
	lv_obj_set_style_radius(warn_box, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_pad_all(warn_box, 6, 0);
	lv_obj_set_style_pad_row(warn_box, 4, 0);
	lv_obj_clear_flag(warn_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(warn_box, LV_FLEX_FLOW_COLUMN);
	if (!has_warnings)
		lv_obj_add_flag(warn_box, LV_OBJ_FLAG_HIDDEN);

	lv_obj_add_event_cb(en_sw, warnings_toggle_cb, LV_EVENT_VALUE_CHANGED,
						warn_box);

	settings_section_t *ws = warn_box;
	char buf[16];

	snprintf(buf, sizeof(buf), "%d",
			 (int)config_bridge_get_bar_low(value_id));
	lv_obj_t *blow =
		settings_add_text_input(ws, "Low Threshold:", "value", buf);
	lv_obj_add_event_cb(blow, keyboard_event_cb, LV_EVENT_ALL, NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(blow, bar_low_value_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(blow, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	snprintf(buf, sizeof(buf), "%d",
			 (int)config_bridge_get_bar_high(value_id));
	lv_obj_t *bhigh =
		settings_add_text_input(ws, "High Threshold:", "value", buf);
	lv_obj_add_event_cb(bhigh, keyboard_event_cb, LV_EVENT_ALL, NULL);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(bhigh, bar_high_value_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(bhigh, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	lv_obj_t *blc = settings_add_dropdown(ws, "Low Colour:", BAR_COLOR_OPTS, 0);
	lv_dropdown_set_selected(
		blc, bar_color_idx(config_bridge_get_bar_low_color(value_id)));
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(blc, bar_low_color_event_cb, LV_EVENT_VALUE_CHANGED,
							p);
		lv_obj_add_event_cb(blc, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	lv_obj_t *bhc =
		settings_add_dropdown(ws, "High Colour:", BAR_COLOR_OPTS, 0);
	lv_dropdown_set_selected(
		bhc, bar_color_idx(config_bridge_get_bar_high_color(value_id)));
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(bhc, bar_high_color_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(bhc, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}

	lv_obj_t *brc =
		settings_add_dropdown(ws, "In-Range Colour:", BAR_COLOR_OPTS, 0);
	lv_dropdown_set_selected(
		brc, bar_color_idx(config_bridge_get_bar_in_range_color(value_id)));
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(brc, bar_in_range_color_event_cb,
							LV_EVENT_VALUE_CHANGED, p);
		lv_obj_add_event_cb(brc, free_value_id_event_cb, LV_EVENT_DELETE, p);
	}
}

/* =========================================================================
 * Display tab — RPM (value_id 9)
 *   Gauge & colour, limiter, RPM lights and background section.
 * ========================================================================= */

static void build_display_tab_rpm(lv_obj_t *tab, uint8_t value_id) {
	(void)value_id; /* used only for config_bridge calls */

	/* ── Gauge & Colour ── */
	settings_section_t *sec =
		settings_add_section(tab, "GAUGE & COLOUR", THEME_COLOR_ACCENT_TEAL);

	lv_obj_t *ecu = settings_add_dropdown(
		sec, "ECU Preset:", "Custom\nMaxxECU\nHaltech\nFord BA/BF/FG", 0);
	lv_obj_add_event_cb(ecu, rpm_ecu_dropdown_event_cb, LV_EVENT_VALUE_CHANGED,
						NULL);

	lv_obj_t *gm = settings_add_dropdown(sec, "Gauge Max:", RPM_OPTS, 0);
	lv_dropdown_set_selected(gm,
							 (rpm_gauge_max >= 3000 && rpm_gauge_max <= 12000)
								 ? (rpm_gauge_max - 3000) / 200
								 : 0);
	lv_obj_add_event_cb(gm, rpm_gauge_roller_event_cb, LV_EVENT_VALUE_CHANGED,
						NULL);

	lv_obj_t *rl = settings_add_dropdown(sec, "Redline RPM:", RPM_OPTS, 0);
	lv_dropdown_set_selected(
		rl, (rpm_redline_value >= 3000 && rpm_redline_value <= 12000)
				? (rpm_redline_value - 3000) / 200
				: 0);
	lv_obj_add_event_cb(rl, rpm_redline_roller_event_cb, LV_EVENT_VALUE_CHANGED,
						NULL);

	lv_obj_t *rc = settings_add_dropdown(sec, "RPM Colour:", RPM_COLOR_OPTS, 0);
	lv_dropdown_set_selected(
		rc, rpm_color_idx(config_bridge_get_rpm_bar_color()));
	lv_obj_add_event_cb(rc, rpm_color_dropdown_event_cb, LV_EVENT_VALUE_CHANGED,
						NULL);

	/* ── Limiter ── */
	settings_section_t *lim =
		settings_add_section(tab, "LIMITER", THEME_COLOR_ACCENT_AMBER);

	lv_obj_t *le = settings_add_dropdown(
		lim, "Effect:",
		"None\nBar Flash\nBar & Circles Flash\nCircles Flash\nBar Solid\nBar & "
		"Circles Solid\nCircles Solid",
		0);
	uint8_t eff = config_bridge_get_rpm_limiter_effect();
	lv_dropdown_set_selected(
		le, eff < 8 ? ((uint8_t[]){0, 0, 1, 2, 3, 4, 5, 6})[eff] : 0);
	lv_obj_add_event_cb(le, rpm_limiter_effect_dropdown_event_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	lv_obj_t *lr = settings_add_dropdown(lim, "Limiter RPM:", RPM_OPTS, 0);
	int32_t lv = config_bridge_get_rpm_limiter_value();
	lv_dropdown_set_selected(lr, (lv >= 3000 && lv <= 12000) ? (lv - 3000) / 200
															 : 0);
	lv_obj_add_event_cb(lr, rpm_limiter_roller_event_cb, LV_EVENT_VALUE_CHANGED,
						NULL);

	lv_obj_t *lc =
		settings_add_dropdown(lim, "Limiter Colour:", RPM_COLOR_OPTS, 0);
	lv_dropdown_set_selected(
		lc, rpm_color_idx(config_bridge_get_rpm_limiter_color()));
	lv_obj_add_event_cb(lc, rpm_limiter_color_dropdown_event_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	/* ── Lighting & Background ── */
	settings_section_t *light =
		settings_add_section(tab, "LIGHTING", THEME_COLOR_ACCENT);

	bool bg_on = config_bridge_get_rpm_background_enabled();
	lv_obj_t *bgsw = settings_add_switch(light, "RPM Background:", bg_on);
	lv_obj_add_event_cb(bgsw, rpm_background_switch_event_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	/* Conditional sub-box — hidden when background is off */
	lv_obj_t *bg_box = lv_obj_create(tab);
	lv_obj_set_size(bg_box, lv_pct(100), LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(bg_box, THEME_COLOR_SECTION_BG, 0);
	lv_obj_set_style_bg_opa(bg_box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(bg_box, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(bg_box, 1, 0);
	lv_obj_set_style_radius(bg_box, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_pad_all(bg_box, 6, 0);
	lv_obj_set_style_pad_row(bg_box, 4, 0);
	lv_obj_clear_flag(bg_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(bg_box, LV_FLEX_FLOW_COLUMN);
	if (!bg_on)
		lv_obj_add_flag(bg_box, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(bgsw, warnings_toggle_cb, LV_EVENT_VALUE_CHANGED,
						bg_box);

	settings_section_t *bgs = bg_box;
	lv_obj_t *bgc =
		settings_add_dropdown(bgs, "Background Colour:", RPM_COLOR_OPTS, 0);
	lv_dropdown_set_selected(
		bgc, rpm_color_idx(config_bridge_get_rpm_background_color()));
	lv_obj_add_event_cb(bgc, rpm_background_color_dropdown_event_cb,
						LV_EVENT_VALUE_CHANGED, NULL);

	lv_obj_t *bgt = settings_add_dropdown(bgs, "Background RPM:", RPM_OPTS, 0);
	int32_t bv = config_bridge_get_rpm_background_value();
	lv_dropdown_set_selected(
		bgt, (bv >= 3000 && bv <= 12000) ? (bv - 3000) / 200 : 0);
	lv_obj_add_event_cb(bgt, rpm_background_threshold_roller_event_cb,
						LV_EVENT_VALUE_CHANGED, NULL);
}

/* =========================================================================
 * Display tab — Speed (value_id 10)
 * ========================================================================= */

static void build_display_tab_speed(lv_obj_t *tab, uint8_t value_id) {
	(void)value_id; /* used only for config_bridge calls */
	settings_section_t *sec =
		settings_add_section(tab, "SPEED OPTIONS", THEME_COLOR_ACCENT_TEAL);
	lv_obj_t *ud = settings_add_dropdown(sec, "Speed Units:", "KMH\nMPH", 0);
	lv_dropdown_set_selected(ud, config_bridge_get_use_mph() ? 1 : 0);
	lv_obj_add_event_cb(ud, speed_units_dropdown_event_cb,
						LV_EVENT_VALUE_CHANGED, NULL);
}

/* =========================================================================
 * Display tab — Gear (value_id 11)
 *   Detection mode + optional custom gear values overlay.
 * ========================================================================= */

/* Close callback for the custom gear values full-screen overlay */
static void gear_overlay_close_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	lv_obj_t *ov = (lv_obj_t *)lv_event_get_user_data(e);
	if (ov && lv_obj_is_valid(ov))
		lv_obj_del(ov);
}

/* Context for the gear ECU dropdown — shows/hides the two edit buttons */
typedef struct {
	lv_obj_t *custom_btn;
	lv_obj_t *ratio_btn;
} gear_dd_ctx_t;

static void gear_dd_ctx_free_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_DELETE)
		return;
	gear_dd_ctx_t *ctx = (gear_dd_ctx_t *)lv_event_get_user_data(e);
	if (ctx)
		lv_mem_free(ctx);
}

/* Helper: show/hide a button safely */
static void set_btn_vis(lv_obj_t *btn, bool show) {
	if (!btn || !lv_obj_is_valid(btn))
		return;
	if (show)
		lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
}

/* Local gear ECU dropdown handler — updates config + shows/hides buttons */
static void gear_ecu_modal_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
		return;
	gear_dd_ctx_t *ctx = (gear_dd_ctx_t *)lv_event_get_user_data(e);
	uint16_t sel = lv_dropdown_get_selected(lv_event_get_target(e));
	config_bridge_set_gear_detection_mode((uint8_t)sel);
	if (ctx) {
		set_btn_vis(ctx->custom_btn, sel == 0);
		set_btn_vis(ctx->ratio_btn, sel == 4);
	}
}

/* ── Speed/RPM Ratio overlay callbacks (local — no dependency on ui_Screen3) */

static void tire_circ_input_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_DEFOCUSED)
		return;
	config_bridge_set_tire_circumference(
		(float)atof(lv_textarea_get_text(lv_event_get_target(e))));
}

static void final_drive_input_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_DEFOCUSED)
		return;
	config_bridge_set_final_drive_ratio(
		(float)atof(lv_textarea_get_text(lv_event_get_target(e))));
}

/* user_data: -1 = reverse, 0-9 = gear index */
static void ratio_input_cb(lv_event_t *e) {
	lv_event_code_t code = lv_event_get_code(e);
	if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_DEFOCUSED)
		return;
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	float val = (float)atof(lv_textarea_get_text(lv_event_get_target(e)));
	if (idx < 0)
		config_bridge_set_reverse_gear_ratio(val);
	else
		config_bridge_set_gear_ratio(idx, val);
}

/* Forward declaration — defined later in the Speed/RPM section */
static lv_obj_t *make_overlay_hdr(lv_obj_t *ov, const char *title);

/* Open a full-screen overlay hosting the custom gear value inputs */
static void open_custom_gear_overlay(uint8_t gear_mode) {
	lv_obj_t *ov = lv_obj_create(lv_layer_top());
	lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
	lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(ov, 230, 0);
	lv_obj_set_style_border_width(ov, 0, 0);
	lv_obj_set_style_radius(ov, 0, 0);

	/* Gear value inputs use absolute positioning — create before header */
	create_custom_gear_values_section(ov, gear_mode);

	/* Header rendered last so it draws over scroll content */
	make_overlay_hdr(ov, "CUSTOM GEAR VALUES");
}

static void gear_custom_btn_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	open_custom_gear_overlay(config_bridge_get_gear_detection_mode());
}

/* ── Speed/RPM Ratio overlay ────────────────────────────────────────────────
 */

static lv_obj_t *make_overlay_hdr(lv_obj_t *ov, const char *title) {
	lv_obj_t *ohdr = lv_obj_create(ov);
	lv_obj_set_size(ohdr, LV_PCT(100), 48);
	lv_obj_align(ohdr, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_clear_flag(ohdr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(ohdr, THEME_COLOR_INPUT_BG, 0);
	lv_obj_set_style_bg_opa(ohdr, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(ohdr, 0, 0);
	lv_obj_set_style_border_width(ohdr, 0, 0);
	lv_obj_set_style_border_side(ohdr, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(ohdr, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(ohdr, 1, 0);
	lv_obj_set_style_pad_hor(ohdr, 14, 0);

	lv_obj_t *otitle = lv_label_create(ohdr);
	lv_label_set_text(otitle, title);
	lv_obj_set_style_text_color(otitle, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(otitle, THEME_FONT_MEDIUM, 0);
	lv_obj_align(otitle, LV_ALIGN_LEFT_MID, 0, 0);

	lv_obj_t *ocls = lv_btn_create(ohdr);
	lv_obj_set_size(ocls, 100, 34);
	lv_obj_align(ocls, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_bg_color(ocls, THEME_COLOR_BTN_CANCEL, 0);
	lv_obj_set_style_radius(ocls, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_border_width(ocls, 0, 0);
	lv_obj_t *oclbl = lv_label_create(ocls);
	lv_label_set_text(oclbl, LV_SYMBOL_CLOSE "  CLOSE");
	lv_obj_set_style_text_font(oclbl, THEME_FONT_BODY, 0);
	lv_obj_set_style_text_color(oclbl, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_center(oclbl);
	lv_obj_add_event_cb(ocls, gear_overlay_close_cb, LV_EVENT_CLICKED, ov);
	return ohdr;
}

static lv_obj_t *make_ratio_input(lv_obj_t *parent, const char *label,
								  float value, int user_idx) {
	/* Reuse settings_add_text_input layout: label on left, textarea on right */
	char buf[20];
	if (value > 0.0001f)
		snprintf(buf, sizeof(buf), "%.3f", value);
	else
		buf[0] = '\0';

	lv_obj_t *ta = settings_add_text_input(parent, label, "ratio", buf);
	lv_obj_add_event_cb(ta, keyboard_event_cb, LV_EVENT_ALL, NULL);
	lv_obj_add_event_cb(ta, ratio_input_cb, LV_EVENT_VALUE_CHANGED,
						(void *)(intptr_t)user_idx);
	lv_obj_add_event_cb(ta, ratio_input_cb, LV_EVENT_DEFOCUSED,
						(void *)(intptr_t)user_idx);
	return ta;
}

static void open_speed_rpm_overlay(void) {
	lv_obj_t *ov = lv_obj_create(lv_layer_top());
	lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
	lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(ov, 230, 0);
	lv_obj_set_style_border_width(ov, 0, 0);
	lv_obj_set_style_radius(ov, 0, 0);

	/* Scrollable content area below the 48-px header */
	lv_obj_t *scroll = lv_obj_create(ov);
	lv_obj_set_size(scroll, LV_PCT(100), 480 - 48);
	lv_obj_set_pos(scroll, 0, 48);
	style_tab(scroll); /* surface bg + flex-column */

	/* ── Vehicle Setup section ── */
	settings_section_t *vs =
		settings_add_section(scroll, "VEHICLE SETUP", THEME_COLOR_ACCENT_TEAL);

	char buf[20];
	snprintf(buf, sizeof(buf), "%.1f",
			 config_bridge_get_tire_circumference());
	lv_obj_t *tc =
		settings_add_text_input(vs, "Tire Circ. (mm):", "e.g. 1980", buf);
	lv_obj_add_event_cb(tc, keyboard_event_cb, LV_EVENT_ALL, NULL);
	lv_obj_add_event_cb(tc, tire_circ_input_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(tc, tire_circ_input_cb, LV_EVENT_DEFOCUSED, NULL);

	snprintf(buf, sizeof(buf), "%.3f",
			 config_bridge_get_final_drive_ratio());
	lv_obj_t *fd =
		settings_add_text_input(vs, "Final Drive Ratio:", "e.g. 3.420", buf);
	lv_obj_add_event_cb(fd, keyboard_event_cb, LV_EVENT_ALL, NULL);
	lv_obj_add_event_cb(fd, final_drive_input_cb, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(fd, final_drive_input_cb, LV_EVENT_DEFOCUSED, NULL);

	/* ── Gear Ratios section ── */
	settings_section_t *gr =
		settings_add_section(scroll, "GEAR RATIOS", THEME_COLOR_ACCENT_AMBER);

	/* Gears 1-10 */
	const char *gear_names[] = {"1st:", "2nd:", "3rd:", "4th:", "5th:",
								"6th:", "7th:", "8th:", "9th:", "10th:"};
	for (int i = 0; i < 10; i++)
		make_ratio_input(gr, gear_names[i], config_bridge_get_gear_ratio(i),
						 i);

	/* Header rendered last so it draws over any scroll content */
	make_overlay_hdr(ov, "SPEED / RPM RATIO SETUP");
}

static void speed_ratio_btn_cb(lv_event_t *e) {
	if (lv_event_get_code(e) != LV_EVENT_CLICKED)
		return;
	open_speed_rpm_overlay();
}

/* Helper: create a styled action button inside the gear tab */
static lv_obj_t *make_gear_action_btn(lv_obj_t *tab, const char *label_txt) {
	lv_obj_t *btn = lv_btn_create(tab);
	lv_obj_set_size(btn, lv_pct(100), 38);
	lv_obj_set_style_bg_color(btn, THEME_COLOR_ACCENT_DIM, 0);
	lv_obj_set_style_bg_color(btn, THEME_COLOR_ACCENT, LV_STATE_PRESSED);
	lv_obj_set_style_radius(btn, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_border_width(btn, 1, 0);
	lv_obj_set_style_border_color(btn, THEME_COLOR_ACCENT, 0);
	lv_obj_t *lbl = lv_label_create(btn);
	lv_label_set_text(lbl, label_txt);
	lv_obj_set_style_text_font(lbl, THEME_FONT_BODY, 0);
	lv_obj_set_style_text_color(lbl, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_center(lbl);
	return btn;
}

static void build_display_tab_gear(lv_obj_t *tab) {
	uint8_t gear_mode = config_bridge_get_gear_detection_mode();
	settings_section_t *sec =
		settings_add_section(tab, "GEAR DETECTION", THEME_COLOR_ACCENT_TEAL);

	lv_obj_t *dd = settings_add_dropdown(
		sec,
		"Detection Mode:", "Custom\nMaxxECU\nHaltech\nFord\nSpeed/RPM Ratio",
		0);
	lv_dropdown_set_selected(dd, gear_mode);

	/* "Edit Custom Values" button — visible only for Custom mode (0) */
	lv_obj_t *cbtn =
		make_gear_action_btn(tab, LV_SYMBOL_EDIT "  EDIT CUSTOM GEAR VALUES");
	if (gear_mode != 0)
		lv_obj_add_flag(cbtn, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(cbtn, gear_custom_btn_cb, LV_EVENT_CLICKED, NULL);

	/* "Edit Speed/RPM Ratios" button — visible only for Speed/RPM Ratio mode
	 * (4) */
	lv_obj_t *rbtn =
		make_gear_action_btn(tab, LV_SYMBOL_SETTINGS "  EDIT SPEED/RPM RATIOS");
	if (gear_mode != 4)
		lv_obj_add_flag(rbtn, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(rbtn, speed_ratio_btn_cb, LV_EVENT_CLICKED, NULL);

	/* Wire dropdown: updates config + shows/hides correct button */
	gear_dd_ctx_t *ctx = lv_mem_alloc(sizeof(gear_dd_ctx_t));
	ctx->custom_btn = cbtn;
	ctx->ratio_btn = rbtn;
	lv_obj_add_event_cb(dd, gear_ecu_modal_cb, LV_EVENT_VALUE_CHANGED, ctx);
	lv_obj_add_event_cb(dd, gear_dd_ctx_free_cb, LV_EVENT_DELETE, ctx);
}

/* =========================================================================
 * Public: config_modal_open
 * ========================================================================= */

/* ── small helper: style a pane-header strip ─────────────────────────────── */
static void style_strip(lv_obj_t *obj, lv_coord_t w, lv_coord_t h) {
	lv_obj_set_size(obj, w, h);
	lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(obj, THEME_COLOR_INPUT_BG, 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(obj, 0, 0);
	lv_obj_set_style_border_width(obj, 0, 0);
	lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(obj, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(obj, 1, 0);
}

void config_modal_open(lv_obj_t *screen, uint8_t value_id) {
	uint8_t idx = value_id - 1;
	bool is_bar = (value_id == BAR1_VALUE_ID || value_id == BAR2_VALUE_ID);
	bool is_rpm = (value_id == RPM_VALUE_ID);
	bool is_speed = (value_id == SPEED_VALUE_ID);
	bool is_gear = (value_id == GEAR_VALUE_ID);
	bool is_panel = (!is_bar && !is_rpm && !is_speed && !is_gear);
	uint8_t bar_idx = (value_id == BAR1_VALUE_ID) ? 0 : 1;

	/* ── Keyboard ───────────────────────────────────────────────────────────
	 */
	keyboard = lv_keyboard_create(screen);
	lv_obj_set_parent(keyboard, lv_layer_top());
	lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(keyboard, keyboard_ready_event_cb, LV_EVENT_READY,
						NULL);

	/* ── Modal outer shell (flex-row: preview | settings) ────────────────── */
	lv_obj_t *modal = lv_obj_create(screen);
	lv_obj_set_size(modal, MODAL_W, MODAL_H);
	lv_obj_center(modal);
	lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_bg_color(modal, THEME_COLOR_SURFACE, 0);
	lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(modal, THEME_RADIUS_NORMAL, 0);
	lv_obj_set_style_border_color(modal, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(modal, 1, 0);
	lv_obj_set_style_pad_all(modal, 0, 0);
	lv_obj_set_style_pad_column(modal, 0, 0);
	lv_obj_set_style_shadow_width(modal, THEME_SHADOW_W_POPUP, 0);
	lv_obj_set_style_shadow_color(modal, lv_color_black(), 0);
	lv_obj_set_style_shadow_opa(modal, 140, 0);

	/* =======================================================================
	 * LEFT COLUMN — Live Preview Pane
	 * ===================================================================== */
	lv_obj_t *prev_pane = lv_obj_create(modal);
	lv_obj_set_size(prev_pane, PREV_W, MODAL_H);
	lv_obj_clear_flag(prev_pane, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(prev_pane, THEME_COLOR_INPUT_BG, 0);
	lv_obj_set_style_bg_opa(prev_pane, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(prev_pane, 0, 0);
	lv_obj_set_style_border_width(prev_pane, 0, 0);
	lv_obj_set_style_border_side(prev_pane, LV_BORDER_SIDE_RIGHT, 0);
	lv_obj_set_style_border_color(prev_pane, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(prev_pane, 1, 0);
	lv_obj_set_style_pad_all(prev_pane, 0, 0);

	/* "LIVE PREVIEW" header strip — same height as the settings header */
	lv_obj_t *prev_hdr = lv_obj_create(prev_pane);
	style_strip(prev_hdr, PREV_W, HDR_H);
	lv_obj_align(prev_hdr, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_pad_all(prev_hdr, 0, 0);
	lv_obj_t *prev_title = lv_label_create(prev_hdr);
	lv_label_set_text(prev_title, "LIVE PREVIEW");
	lv_obj_set_style_text_color(prev_title, THEME_COLOR_ACCENT, 0);
	lv_obj_set_style_text_font(prev_title, THEME_FONT_SMALL, 0);
	lv_obj_center(prev_title);

	/* ── Panel preview ─────────────────────────────────────────────────── */
	if (is_panel) {
		lv_obj_t *pbox = lv_obj_create(prev_pane);
		lv_obj_set_size(pbox, PREV_W - 20, 108);
		lv_obj_align(pbox, LV_ALIGN_CENTER, 0, -14);
		lv_obj_clear_flag(pbox, LV_OBJ_FLAG_SCROLLABLE);
		lv_obj_set_style_bg_color(pbox, THEME_COLOR_PANEL, 0);
		lv_obj_set_style_bg_opa(pbox, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(pbox, THEME_RADIUS_NORMAL, 0);
		lv_obj_set_style_border_color(pbox, THEME_COLOR_BORDER_MED, 0);
		lv_obj_set_style_border_width(pbox, 1, 0);
		lv_obj_set_style_pad_all(pbox, 6, 0);
		menu_panel_boxes[idx] = pbox;

		lv_obj_t *plbl = lv_label_create(pbox);
		lv_label_set_text(plbl, config_bridge_get_label(value_id));
		lv_obj_set_style_text_color(plbl, THEME_COLOR_TEXT_MUTED, 0);
		lv_obj_set_style_text_font(plbl, THEME_FONT_DASH_LABEL, 0);
		lv_obj_align(plbl, LV_ALIGN_TOP_MID, 0, 2);
		menu_panel_labels[idx] = plbl;

		const char *cur =
			(ui_Value[idx] && lv_obj_is_valid(ui_Value[idx]))
				? lv_label_get_text(ui_Value[idx])
				: (strlen(previous_values[idx]) > 0 ? previous_values[idx]
													: "---");
		lv_obj_t *pval = lv_label_create(pbox);
		lv_label_set_text(pval, cur);
		lv_obj_set_style_text_color(pval, THEME_COLOR_TEXT_PRIMARY, 0);
		lv_obj_set_style_text_font(pval, THEME_FONT_DASH_VALUE, 0);
		lv_obj_align(pval, LV_ALIGN_CENTER, 0, 8);
		menu_panel_value_labels[idx] = pval;

		prev_ctx_t *pctx = lv_mem_alloc(sizeof(prev_ctx_t));
		pctx->value_id = value_id;
		pctx->val_lbl = pval;
		lv_timer_t *ptimer = lv_timer_create(panel_preview_timer_cb, 500, pctx);
		lv_obj_add_event_cb(prev_pane, prev_timer_del_cb, LV_EVENT_DELETE,
							ptimer);
		lv_obj_add_event_cb(prev_pane, prev_ctx_del_cb, LV_EVENT_DELETE, pctx);

		lv_obj_t *hint = lv_label_create(prev_pane);
		lv_label_set_text(hint, "Label updates\nas you type");
		lv_obj_set_style_text_color(hint, THEME_COLOR_TEXT_GHOST, 0);
		lv_obj_set_style_text_font(hint, THEME_FONT_TINY, 0);
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(hint, PREV_W - 16);
		lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

		/* ── Bar preview ──────────────────────────────────────────────────────
		 */
	} else if (is_bar) {
		lv_obj_t *bar_lbl = lv_label_create(prev_pane);
		lv_label_set_text(bar_lbl, config_bridge_get_label(value_id));
		lv_obj_set_style_text_color(bar_lbl, THEME_COLOR_TEXT_MUTED, 0);
		lv_obj_set_style_text_font(bar_lbl, THEME_FONT_BODY, 0);
		lv_obj_align(bar_lbl, LV_ALIGN_CENTER, 0, -36);
		menu_bar_labels[bar_idx] = bar_lbl;

		lv_obj_t *pbar = lv_bar_create(prev_pane);
		menu_bar_objects[bar_idx] = config_bars[idx] = pbar;
		lv_obj_set_size(pbar, PREV_W - 20, 22);
		lv_obj_align(pbar, LV_ALIGN_CENTER, 0, -8);
		int bar_min = (int)config_bridge_get_bar_min(value_id);
		int bar_max = (int)config_bridge_get_bar_max(value_id);
		lv_bar_set_range(pbar, bar_min, bar_max);
		lv_bar_set_value(pbar, bar_min + (bar_max - bar_min) / 2, LV_ANIM_OFF);
		lv_obj_set_style_bg_color(pbar, THEME_COLOR_PANEL,
								  LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_radius(pbar, THEME_RADIUS_SMALL,
								LV_PART_MAIN | LV_STATE_DEFAULT);
		lv_obj_set_style_bg_color(
			pbar, config_bridge_get_bar_in_range_color(value_id),
			LV_PART_INDICATOR | LV_STATE_DEFAULT);
		lv_obj_set_style_radius(pbar, THEME_RADIUS_SMALL,
								LV_PART_INDICATOR | LV_STATE_DEFAULT);

		char rl[24];
		snprintf(rl, sizeof(rl), "%d — %d", bar_min, bar_max);
		lv_obj_t *rlbl = lv_label_create(prev_pane);
		lv_label_set_text(rlbl, rl);
		lv_obj_set_style_text_color(rlbl, THEME_COLOR_TEXT_MUTED, 0);
		lv_obj_set_style_text_font(rlbl, THEME_FONT_TINY, 0);
		lv_obj_align(rlbl, LV_ALIGN_CENTER, 0, 20);

		lv_obj_t *hint = lv_label_create(prev_pane);
		lv_label_set_text(hint, "Colors & range\nupdate live");
		lv_obj_set_style_text_color(hint, THEME_COLOR_TEXT_GHOST, 0);
		lv_obj_set_style_text_font(hint, THEME_FONT_TINY, 0);
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
		lv_obj_set_width(hint, PREV_W - 16);
		lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

		/* ── RPM preview
		 * ─────────────────────────────────────────────────────── */
	} else if (is_rpm) {
		lv_obj_t *rlbl = lv_label_create(prev_pane);
		lv_label_set_text(rlbl, "RPM");
		lv_obj_set_style_text_color(rlbl, THEME_COLOR_TEXT_MUTED, 0);
		lv_obj_set_style_text_font(rlbl, THEME_FONT_DASH_LABEL, 0);
		lv_obj_align(rlbl, LV_ALIGN_CENTER, 0, -28);

		const char *crpm = (ui_RPM_Value && lv_obj_is_valid(ui_RPM_Value))
							   ? lv_label_get_text(ui_RPM_Value)
							   : "---";
		lv_obj_t *rval = lv_label_create(prev_pane);
		lv_label_set_text(rval, crpm);
		lv_obj_set_style_text_color(rval, THEME_COLOR_TEXT_PRIMARY, 0);
		lv_obj_set_style_text_font(rval, THEME_FONT_DASH_VALUE, 0);
		lv_obj_align(rval, LV_ALIGN_CENTER, 0, 8);
		menu_rpm_value_label =
			rval; /* existing update callbacks keep this live */

		/* Colour swatch — reflects current rpm_bar_color */
		lv_obj_t *swatch = lv_obj_create(prev_pane);
		lv_obj_set_size(swatch, PREV_W - 20, 6);
		lv_obj_align(swatch, LV_ALIGN_CENTER, 0, 38);
		lv_obj_set_style_bg_color(swatch, config_bridge_get_rpm_bar_color(), 0);
		lv_obj_set_style_bg_opa(swatch, LV_OPA_COVER, 0);
		lv_obj_set_style_radius(swatch, 3, 0);
		lv_obj_set_style_border_width(swatch, 0, 0);

		lv_obj_t *hint = lv_label_create(prev_pane);
		lv_label_set_text(hint, "Live CAN value");
		lv_obj_set_style_text_color(hint, THEME_COLOR_TEXT_GHOST, 0);
		lv_obj_set_style_text_font(hint, THEME_FONT_TINY, 0);
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

		/* ── Speed preview
		 * ───────────────────────────────────────────────────── */
	} else if (is_speed) {
		lv_obj_t *slbl = lv_label_create(prev_pane);
		lv_label_set_text(slbl, "SPEED");
		lv_obj_set_style_text_color(slbl, THEME_COLOR_TEXT_MUTED, 0);
		lv_obj_set_style_text_font(slbl, THEME_FONT_DASH_LABEL, 0);
		lv_obj_align(slbl, LV_ALIGN_CENTER, 0, -28);

		const char *cspd = (ui_Speed_Value && lv_obj_is_valid(ui_Speed_Value))
							   ? lv_label_get_text(ui_Speed_Value)
							   : "---";
		lv_obj_t *sval = lv_label_create(prev_pane);
		lv_label_set_text(sval, cspd);
		lv_obj_set_style_text_color(sval, THEME_COLOR_TEXT_PRIMARY, 0);
		lv_obj_set_style_text_font(sval, THEME_FONT_DASH_VALUE, 0);
		lv_obj_align(sval, LV_ALIGN_CENTER, 0, 8);
		menu_speed_value_label = sval;

		const char *cunit = (ui_Kmh && lv_obj_is_valid(ui_Kmh))
								? lv_label_get_text(ui_Kmh)
								: "KMH";
		lv_obj_t *unitlbl = lv_label_create(prev_pane);
		lv_label_set_text(unitlbl, cunit);
		lv_obj_set_style_text_color(unitlbl, THEME_COLOR_TEXT_MUTED, 0);
		lv_obj_set_style_text_font(unitlbl, THEME_FONT_SMALL, 0);
		lv_obj_align(unitlbl, LV_ALIGN_CENTER, 0, 38);
		menu_speed_units_label = unitlbl;

		lv_obj_t *hint = lv_label_create(prev_pane);
		lv_label_set_text(hint, "Units update\non change");
		lv_obj_set_style_text_color(hint, THEME_COLOR_TEXT_GHOST, 0);
		lv_obj_set_style_text_font(hint, THEME_FONT_TINY, 0);
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);

		/* ── Gear preview
		 * ────────────────────────────────────────────────────── */
	} else if (is_gear) {
		lv_obj_t *glbl = lv_label_create(prev_pane);
		lv_label_set_text(glbl, "GEAR");
		lv_obj_set_style_text_color(glbl, THEME_COLOR_TEXT_MUTED, 0);
		lv_obj_set_style_text_font(glbl, THEME_FONT_DASH_LABEL, 0);
		lv_obj_align(glbl, LV_ALIGN_CENTER, 0, -28);

		const char *cg = (ui_GEAR_Value && lv_obj_is_valid(ui_GEAR_Value))
							 ? lv_label_get_text(ui_GEAR_Value)
							 : "-";
		lv_obj_t *gval = lv_label_create(prev_pane);
		lv_label_set_text(gval, cg);
		lv_obj_set_style_text_color(gval, THEME_COLOR_TEXT_PRIMARY, 0);
		lv_obj_set_style_text_font(gval, THEME_FONT_DASH_GEAR, 0);
		lv_obj_align(gval, LV_ALIGN_CENTER, 0, 10);
		menu_gear_value_label = gval;

		lv_obj_t *hint = lv_label_create(prev_pane);
		lv_label_set_text(hint, "Live CAN value");
		lv_obj_set_style_text_color(hint, THEME_COLOR_TEXT_GHOST, 0);
		lv_obj_set_style_text_font(hint, THEME_FONT_TINY, 0);
		lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
	}

	/* =======================================================================
	 * RIGHT COLUMN — Settings Pane (flex-column: header | tabview | footer)
	 * ===================================================================== */
	lv_obj_t *set_pane = lv_obj_create(modal);
	lv_obj_set_size(set_pane, SETTINGS_W, MODAL_H);
	lv_obj_clear_flag(set_pane, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(set_pane, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_bg_color(set_pane, THEME_COLOR_SURFACE, 0);
	lv_obj_set_style_bg_opa(set_pane, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(set_pane, 0, 0);
	lv_obj_set_style_border_width(set_pane, 0, 0);
	lv_obj_set_style_pad_all(set_pane, 0, 0);
	lv_obj_set_style_pad_row(set_pane, 0, 0);

	/* ── Settings Header ─────────────────────────────────────────────────── */
	lv_obj_t *hdr = lv_obj_create(set_pane);
	style_strip(hdr, SETTINGS_W, HDR_H);
	lv_obj_set_style_pad_left(hdr, 14, 0);
	lv_obj_set_style_pad_right(hdr, 14, 0);

	char title[48];
	if (is_bar)
		snprintf(title, sizeof(title), "BAR %d SETTINGS", bar_idx + 1);
	else if (is_rpm)
		snprintf(title, sizeof(title), "RPM SETTINGS");
	else if (is_speed)
		snprintf(title, sizeof(title), "SPEED SETTINGS");
	else if (is_gear)
		snprintf(title, sizeof(title), "GEAR SETTINGS");
	else
		snprintf(title, sizeof(title), "PANEL %d SETTINGS", value_id);
	lv_obj_t *title_lbl = lv_label_create(hdr);
	lv_label_set_text(title_lbl, title);
	lv_obj_set_style_text_color(title_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(title_lbl, THEME_FONT_MEDIUM, 0);
	lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 0, 0);

	lv_obj_t *preset_btn = lv_btn_create(hdr);
	lv_obj_set_size(preset_btn, 140, 36);
	lv_obj_align(preset_btn, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_bg_color(preset_btn, THEME_COLOR_ACCENT, 0);
	lv_obj_set_style_bg_color(preset_btn, THEME_COLOR_ACCENT_DIM,
							  LV_STATE_PRESSED);
	lv_obj_set_style_radius(preset_btn, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_border_width(preset_btn, 0, 0);
	lv_obj_t *preset_lbl = lv_label_create(preset_btn);
	lv_label_set_text(preset_lbl, LV_SYMBOL_DOWNLOAD "  LOAD PRESET");
	lv_obj_set_style_text_color(preset_lbl, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_set_style_text_font(preset_lbl, THEME_FONT_BODY, 0);
	lv_obj_center(preset_lbl);
	{
		uint8_t *p = id_alloc(value_id);
		lv_obj_add_event_cb(preset_btn, preset_btn_cb, LV_EVENT_CLICKED, p);
		lv_obj_add_event_cb(preset_btn, free_value_id_event_cb, LV_EVENT_DELETE,
							p);
	}

	/* ── Tabview ─────────────────────────────────────────────────────────── */
	lv_coord_t tab_total_h = MODAL_H - HDR_H - FOOTER_H;
	lv_obj_t *tv = lv_tabview_create(set_pane, LV_DIR_TOP, TABBAR_H);
	lv_obj_set_size(tv, SETTINGS_W, tab_total_h);
	lv_obj_set_style_bg_color(tv, THEME_COLOR_SURFACE, 0);
	lv_obj_set_style_border_width(tv, 0, 0);
	lv_obj_set_style_pad_all(tv, 0, 0);

	lv_obj_t *tab_btns = lv_tabview_get_tab_btns(tv);
	lv_obj_set_style_bg_color(tab_btns, THEME_COLOR_INPUT_BG, 0);
	lv_obj_set_style_bg_opa(tab_btns, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(tab_btns, THEME_COLOR_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(tab_btns, THEME_FONT_BODY, 0);
	lv_obj_set_style_border_side(tab_btns, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(tab_btns, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(tab_btns, 1, 0);

	lv_obj_set_style_bg_color(tab_btns, THEME_COLOR_ACCENT_DIM,
							  LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_bg_opa(tab_btns, LV_OPA_COVER,
							LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_text_color(tab_btns, THEME_COLOR_TEXT_PRIMARY,
								LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_border_side(tab_btns, LV_BORDER_SIDE_BOTTOM,
								 LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_border_color(tab_btns, THEME_COLOR_ACCENT,
								  LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_border_width(tab_btns, 3,
								  LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_bg_opa(tab_btns, LV_OPA_TRANSP, LV_PART_ITEMS);
	lv_obj_set_style_border_width(tab_btns, 0, LV_PART_ITEMS);

	lv_obj_t *tab_can = lv_tabview_add_tab(tv, "  CAN SIGNAL  ");
	lv_obj_t *tab_display = lv_tabview_add_tab(tv, "  DISPLAY  ");
	lv_obj_t *tab_alerts = lv_tabview_add_tab(tv, "  ALERTS  ");

	style_tab(tab_can);
	style_tab(tab_display);
	style_tab(tab_alerts);

	build_can_tab(tab_can, value_id);

	if (is_bar)
		build_display_tab_bar(tab_display, value_id);
	else if (is_rpm)
		build_display_tab_rpm(tab_display, value_id);
	else if (is_speed)
		build_display_tab_speed(tab_display, value_id);
	else if (is_gear)
		build_display_tab_gear(tab_display);
	else
		build_display_tab_panel(tab_display, value_id);

	if (is_bar)
		build_alerts_tab_bar(tab_alerts, value_id);
	else
		build_alerts_tab_panel(tab_alerts, value_id);

	/* ── Footer ──────────────────────────────────────────────────────────── */
	lv_obj_t *footer = lv_obj_create(set_pane);
	lv_obj_set_size(footer, SETTINGS_W, FOOTER_H);
	lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_bg_color(footer, THEME_COLOR_INPUT_BG, 0);
	lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(footer, 0, 0);
	lv_obj_set_style_border_width(footer, 0, 0);
	lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
	lv_obj_set_style_border_color(footer, THEME_COLOR_BORDER, 0);
	lv_obj_set_style_border_width(footer, 1, 0);
	lv_obj_set_style_pad_all(footer, 6, 0);
	lv_obj_set_style_pad_column(footer, 8, 0);

	lv_coord_t btn_w = (SETTINGS_W - 20) / 2;

	lv_obj_t *cancel_btn = lv_btn_create(footer);
	lv_obj_set_size(cancel_btn, btn_w, FOOTER_H - 12);
	lv_obj_align(cancel_btn, LV_ALIGN_LEFT_MID, 0, 0);
	lv_obj_set_style_bg_color(cancel_btn, THEME_COLOR_BTN_CANCEL, 0);
	lv_obj_set_style_radius(cancel_btn, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_border_width(cancel_btn, 0, 0);
	lv_obj_t *clbl = lv_label_create(cancel_btn);
	lv_label_set_text(clbl, LV_SYMBOL_CLOSE "  CANCEL");
	lv_obj_set_style_text_font(clbl, THEME_FONT_BODY, 0);
	lv_obj_set_style_text_color(clbl, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_center(clbl);
	lv_obj_add_event_cb(cancel_btn, cancel_menu_event_cb, LV_EVENT_CLICKED,
						NULL);

	lv_obj_t *save_btn = lv_btn_create(footer);
	lv_obj_set_size(save_btn, btn_w, FOOTER_H - 12);
	lv_obj_align(save_btn, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_set_style_bg_color(save_btn, THEME_COLOR_BTN_SAVE, 0);
	lv_obj_set_style_radius(save_btn, THEME_RADIUS_SMALL, 0);
	lv_obj_set_style_border_width(save_btn, 0, 0);
	lv_obj_t *slbl = lv_label_create(save_btn);
	lv_label_set_text(slbl, LV_SYMBOL_SAVE "  SAVE");
	lv_obj_set_style_text_font(slbl, THEME_FONT_BODY, 0);
	lv_obj_set_style_text_color(slbl, THEME_COLOR_TEXT_PRIMARY, 0);
	lv_obj_center(slbl);
	lv_obj_add_event_cb(save_btn, close_menu_event_cb, LV_EVENT_CLICKED, NULL);
}
