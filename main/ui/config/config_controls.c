#include "config_controls.h"
#include "../theme.h"
#include "../settings/settings_panel.h"
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "../screens/ui_Screen3.h"
#include "../ui.h"
#include "preset_picker.h"
#include "../callbacks/ui_callbacks.h"

/* External input-widget arrays written by this function */
extern lv_obj_t *g_label_input[];
extern lv_obj_t *g_can_id_input[];
extern lv_obj_t *g_endian_dropdown[];
extern lv_obj_t *g_bit_start_dropdown[];
extern lv_obj_t *g_bit_length_dropdown[];
extern lv_obj_t *g_scale_input[];
extern lv_obj_t *g_offset_input[];
extern lv_obj_t *g_decimals_dropdown[];
extern lv_obj_t *g_type_dropdown[];

extern value_config_t values_config[];
extern char label_texts[13][64];
extern lv_obj_t *ui_MenuScreen;

#define RPM_VALUE_ID   9
#define SPEED_VALUE_ID 10

/* Bit-start options string (0..63) */
static const char *BIT_START_OPTS =
    "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n"
    "16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n"
    "32\n33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n"
    "48\n49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60\n61\n62\n63";

/* Bit-length options string (1..64) */
static const char *BIT_LEN_OPTS =
    "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n"
    "17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31\n32\n"
    "33\n34\n35\n36\n37\n38\n39\n40\n41\n42\n43\n44\n45\n46\n47\n48\n"
    "49\n50\n51\n52\n53\n54\n55\n56\n57\n58\n59\n60\n61\n62\n63\n64";

/* Helper: allocate a value_id heap copy for event-callback user-data */
static uint8_t *id_alloc(uint8_t value_id)
{
    uint8_t *p = lv_mem_alloc(sizeof(uint8_t));
    *p = value_id;
    return p;
}

/* =========================================================================
 * Public function
 * ========================================================================= */
void create_config_controls(lv_obj_t *parent, uint8_t value_id)
{
    uint8_t idx = value_id - 1;

    /* CAN config settings panel — left side of the menu screen */
    settings_panel_t *sp = settings_panel_create(parent, 5, 100, 378, 368);

    char sec_title[32];
    if (value_id != RPM_VALUE_ID)
        snprintf(sec_title, sizeof(sec_title), "PANEL %d CONFIG", value_id);
    else
        snprintf(sec_title, sizeof(sec_title), "CANBUS CONFIG");

    settings_section_t *sec = settings_add_section(sp, sec_title, THEME_COLOR_STATUS_CONNECTED);

    /* -- Label input (panels only, not RPM / Speed) ---------------------- */
    if (value_id != RPM_VALUE_ID && value_id != SPEED_VALUE_ID) {
        show_preconfig_menu(parent);
        g_label_input[idx] = settings_add_text_input(sec, "Label:", "Enter Label",
                                                      label_texts[value_id - 1]);
        lv_obj_add_event_cb(g_label_input[idx], keyboard_event_cb, LV_EVENT_ALL, NULL);
        uint8_t *p = id_alloc(value_id);
        lv_obj_add_event_cb(g_label_input[idx], label_input_event_cb, LV_EVENT_VALUE_CHANGED, p);
        lv_obj_add_event_cb(g_label_input[idx], free_value_id_event_cb, LV_EVENT_DELETE, p);
    }

    /* -- CAN ID ---------------------------------------------------------- */
    char can_id_str[16];
    snprintf(can_id_str, sizeof(can_id_str), "%X", values_config[idx].can_id);
    g_can_id_input[idx] = settings_add_text_input(sec, "CAN ID (0x):", "CAN ID hex", can_id_str);
    lv_obj_add_event_cb(g_can_id_input[idx], keyboard_event_cb, LV_EVENT_ALL, NULL);
    {
        uint8_t *p = id_alloc(value_id);
        lv_obj_add_event_cb(g_can_id_input[idx], can_id_input_event_cb, LV_EVENT_VALUE_CHANGED, p);
        lv_obj_add_event_cb(g_can_id_input[idx], free_value_id_event_cb, LV_EVENT_DELETE, p);
    }

    /* -- Endianness ------------------------------------------------------ */
    g_endian_dropdown[idx] = settings_add_dropdown(sec, "Endian:",
                                                    "Big Endian\nLittle Endian", 0);
    lv_dropdown_set_selected(g_endian_dropdown[idx],
                             values_config[idx].endianess == BIG_ENDIAN_ORDER ? 0 : 1);
    {
        uint8_t *p = id_alloc(value_id);
        lv_obj_add_event_cb(g_endian_dropdown[idx], endianess_roller_event_cb, LV_EVENT_VALUE_CHANGED, p);
        lv_obj_add_event_cb(g_endian_dropdown[idx], free_value_id_event_cb, LV_EVENT_DELETE, p);
    }

    /* -- Bit start ------------------------------------------------------- */
    g_bit_start_dropdown[idx] = settings_add_dropdown(sec, "Bit Start:", BIT_START_OPTS, 0);
    lv_dropdown_set_selected(g_bit_start_dropdown[idx], values_config[idx].bit_start);
    {
        uint8_t *p = id_alloc(value_id);
        lv_obj_add_event_cb(g_bit_start_dropdown[idx], bit_start_roller_event_cb, LV_EVENT_VALUE_CHANGED, p);
        lv_obj_add_event_cb(g_bit_start_dropdown[idx], free_value_id_event_cb, LV_EVENT_DELETE, p);
    }

    /* -- Bit length ------------------------------------------------------ */
    g_bit_length_dropdown[idx] = settings_add_dropdown(sec, "Bit Length:", BIT_LEN_OPTS, 0);
    lv_dropdown_set_selected(g_bit_length_dropdown[idx], values_config[idx].bit_length - 1);
    {
        uint8_t *p = id_alloc(value_id);
        lv_obj_add_event_cb(g_bit_length_dropdown[idx], bit_length_roller_event_cb, LV_EVENT_VALUE_CHANGED, p);
        lv_obj_add_event_cb(g_bit_length_dropdown[idx], free_value_id_event_cb, LV_EVENT_DELETE, p);
    }

    /* -- Scale ----------------------------------------------------------- */
    char scale_str[16];
    snprintf(scale_str, sizeof(scale_str), "%.6g", values_config[idx].scale);
    g_scale_input[idx] = settings_add_text_input(sec, "Scale:", "Scale factor", scale_str);
    lv_obj_add_event_cb(g_scale_input[idx], keyboard_event_cb, LV_EVENT_ALL, NULL);
    {
        uint8_t *p = id_alloc(value_id);
        lv_obj_add_event_cb(g_scale_input[idx], scale_input_event_cb, LV_EVENT_VALUE_CHANGED, p);
        lv_obj_add_event_cb(g_scale_input[idx], free_value_id_event_cb, LV_EVENT_DELETE, p);
    }

    /* -- Value offset ---------------------------------------------------- */
    char offset_str[16];
    snprintf(offset_str, sizeof(offset_str), "%.6g", values_config[idx].value_offset);
    g_offset_input[idx] = settings_add_text_input(sec, "Offset:", "Value Offset", offset_str);
    lv_obj_add_event_cb(g_offset_input[idx], keyboard_event_cb, LV_EVENT_ALL, NULL);
    {
        uint8_t *p = id_alloc(value_id);
        lv_obj_add_event_cb(g_offset_input[idx], value_offset_input_event_cb, LV_EVENT_VALUE_CHANGED, p);
        lv_obj_add_event_cb(g_offset_input[idx], free_value_id_event_cb, LV_EVENT_DELETE, p);
    }

    /* -- Decimals -------------------------------------------------------- */
    g_decimals_dropdown[idx] = settings_add_dropdown(sec, "Decimals:", "0\n1\n2\n3", 0);
    lv_dropdown_set_selected(g_decimals_dropdown[idx], values_config[idx].decimals);
    {
        uint8_t *p = id_alloc(value_id);
        lv_obj_add_event_cb(g_decimals_dropdown[idx], decimal_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, p);
        lv_obj_add_event_cb(g_decimals_dropdown[idx], free_value_id_event_cb, LV_EVENT_DELETE, p);
    }

    /* -- Type ------------------------------------------------------------ */
    g_type_dropdown[idx] = settings_add_dropdown(sec, "Type:", "Unsigned\nSigned", 0);
    lv_dropdown_set_selected(g_type_dropdown[idx], values_config[idx].is_signed ? 1 : 0);
    {
        uint8_t *p = id_alloc(value_id);
        lv_obj_add_event_cb(g_type_dropdown[idx], type_dropdown_event_cb, LV_EVENT_VALUE_CHANGED, p);
        lv_obj_add_event_cb(g_type_dropdown[idx], free_value_id_event_cb, LV_EVENT_DELETE, p);
    }
}
