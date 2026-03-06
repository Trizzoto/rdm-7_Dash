#include "widget_dispatcher.h"
#include "ui/screens/ui_Screen3.h"
#include "ui/ui.h"
#include "ui/theme.h"
#include "can/can_decode.h"
#include "can/can_dispatch.h"
#include "storage/config_store.h"
#include "lvgl.h"
#include "lvgl_helpers.h"
#include "driver/twai.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui/menu/menu_screen.h"
#include "ui/settings/device_settings.h"
#include "ui/settings/preset_picker.h"
#include "ui/callbacks/ui_callbacks.h"

#include "widget_panel.h"
#include "widget_rpm_bar.h"
#include "widget_speed.h"
#include "widget_gear.h"
#include "widget_bar.h"
#include "widget_indicator.h"
#include "widget_warning.h"
#include "widget_text.h"
#include "widget_meter.h"
#include "widget_registry.h"

/* All _update_t typedefs are defined in widget_dispatcher.h — do not repeat. */

/** Notify all text widgets bound to value_idx. Uses stack buffer, no heap. */
static void _notify_text_widgets(uint8_t value_idx) {
	widget_t *arr[TEXT_PER_VALUE_MAX];
	uint8_t cnt;
	can_dispatch_get_text_widgets_for_value(value_idx, arr, TEXT_PER_VALUE_MAX, &cnt);
	if (cnt == 0)
		return;
	text_update_t tud;
	tud.value_idx = value_idx;
	strncpy(tud.value_str, previous_values[value_idx], EXAMPLE_MAX_CHAR_SIZE - 1);
	tud.value_str[EXAMPLE_MAX_CHAR_SIZE - 1] = '\0';
	for (uint8_t j = 0; j < cnt; j++) {
		if (arr[j] && arr[j]->update)
			arr[j]->update(arr[j], &tud);
	}
}

/** Notify all meter widgets bound to value_idx. Uses stack int32_t, no heap. */
static void _notify_meter_widgets(uint8_t value_idx, int32_t raw_value) {
	widget_t *arr[METER_PER_VALUE_MAX];
	uint8_t cnt;
	can_dispatch_get_meter_widgets_for_value(value_idx, arr, METER_PER_VALUE_MAX, &cnt);
	if (cnt == 0)
		return;
	int32_t v = raw_value;
	for (uint8_t j = 0; j < cnt; j++) {
		if (arr[j] && arr[j]->update)
			arr[j]->update(arr[j], &v);
	}
}

/* Externs for per-widget CAN-receive timestamps (owned by each widget .c) */
extern uint64_t last_panel_can_received[8];
extern uint64_t last_bar_can_received[2];
extern uint64_t last_speed_can_received;
extern uint64_t last_gear_can_received;
extern uint64_t last_rpm_can_received;

/* previous_bit_states and warning toggle state — owned by widget_warning.c */
extern bool     previous_bit_states[8];
extern uint64_t last_signal_times[8];
extern bool     toggle_debounce[8];
extern uint64_t toggle_start_time[8];

/* Indicator toggle state — owned by widget_indicator.c */
extern bool     previous_indicator_bit_states[2];
extern bool     indicator_toggle_debounce[2];
extern uint64_t indicator_toggle_start_time[2];

#define BAR_UPDATE_THRESHOLD 0.01f

/* Debug stub — was a verbose logger in original code, now a no-op. */
void print_value_config(uint8_t value_id) { (void)value_id; }

void check_can_timeouts(lv_timer_t *timer) {
	(void)timer; // Suppress unused parameter warning
	uint64_t current_time = esp_timer_get_time() / 1000; // ms

	// Determine the most recent CAN activity across all values
	uint64_t last_any = last_rpm_can_received;
	for (int k = 0; k < 8; k++)
		if (last_panel_can_received[k] > last_any) last_any = last_panel_can_received[k];
	if (last_speed_can_received > last_any) last_any = last_speed_can_received;
	if (last_gear_can_received  > last_any) last_any = last_gear_can_received;
	for (int k = 0; k < 2; k++)
		if (last_bar_can_received[k] > last_any) last_any = last_bar_can_received[k];

	// If CAN bus is alive (any message within 4 s) give each value 10 s before
	// showing "---".  If the bus has been silent for >4 s, drop everything
	// to "---" quickly using the 4 s dead-bus threshold.
	bool can_active = (current_time - last_any) < 4000;
	const uint64_t CAN_TIMEOUT_MS = can_active ? 10000 : 4000;

	// Check panel timeouts (excluding RPM which is panel 9)
	for (int i = 0; i < 8; i++) {
		if (values_config[i].enabled &&
			(current_time - last_panel_can_received[i]) > CAN_TIMEOUT_MS) {

			// Set panel to "---" if it's not already
			if (strcmp(previous_values[i], "---") != 0) {
				strcpy(previous_values[i], "---");
				panel_update_t *p_upd = malloc(sizeof(panel_update_t));
				if (p_upd) {
					p_upd->panel_index = i;
					strcpy(p_upd->value_str, "---");
					p_upd->final_value =
						0; // Use 0 to ensure no threshold warnings
					lv_async_call(update_panel_ui, p_upd);
				}
				_notify_text_widgets((uint8_t)i);
				_notify_meter_widgets((uint8_t)i, 0);
			}
		}
	}

	// Check speed timeout
	if (values_config[SPEED_VALUE_ID - 1].enabled &&
		(current_time - last_speed_can_received) > CAN_TIMEOUT_MS) {

		// Set speed to "---" if it's not already
		if (strcmp(previous_values[SPEED_VALUE_ID - 1], "---") != 0) {
			strcpy(previous_values[SPEED_VALUE_ID - 1], "---");
			speed_update_t *s_upd = malloc(sizeof(speed_update_t));
			if (s_upd) {
				strcpy(s_upd->speed_str, "---");
				lv_async_call(update_speed_ui, s_upd);
			}
			_notify_text_widgets(SPEED_VALUE_ID - 1);
			_notify_meter_widgets(SPEED_VALUE_ID - 1, 0);
		}
	}

	// Check gear timeout (skip if in Speed/RPM Ratio mode - mode 4)
	if (values_config[GEAR_VALUE_ID - 1].enabled &&
		values_config[GEAR_VALUE_ID - 1].gear_detection_mode != 4 &&
		(current_time - last_gear_can_received) > CAN_TIMEOUT_MS) {

		// Set gear to "---" if it's not already
		if (strcmp(previous_values[GEAR_VALUE_ID - 1], "---") != 0) {
			strcpy(previous_values[GEAR_VALUE_ID - 1], "---");
			gear_update_t *g_upd = malloc(sizeof(gear_update_t));
			if (g_upd) {
				strcpy(g_upd->gear_str, "---");
				g_upd->raw_value = 0;
				lv_async_call(update_gear_ui, g_upd);
			}
			_notify_meter_widgets(GEAR_VALUE_ID - 1, 0);
		}
	}

	// Check bar timeouts (skip bars driven by the fuel sender ADC)
	for (int i = 0; i < 2; i++) {
		int value_index = (i == 0) ? BAR1_VALUE_ID - 1 : BAR2_VALUE_ID - 1;
		if (values_config[value_index].fuel_sender) continue;
		if (values_config[value_index].enabled &&
			(current_time - last_bar_can_received[i]) > CAN_TIMEOUT_MS) {

			// Set bar to minimum value (representing "no data")
			int32_t bar_min = values_config[value_index].bar_min;
			char bar_str[EXAMPLE_MAX_CHAR_SIZE];
			snprintf(bar_str, sizeof(bar_str), "%d", bar_min);
			strcpy(previous_values[value_index], bar_str);

			bar_update_t *b_upd = malloc(sizeof(bar_update_t));
			if (b_upd) {
				b_upd->bar_index = i;
				b_upd->bar_value = bar_min;
				b_upd->final_value = (double)bar_min;
				b_upd->config_index = value_index;
				b_upd->is_timeout = true;
				lv_async_call(update_bar_ui, b_upd);
			}
			_notify_text_widgets((uint8_t)value_index);
			_notify_meter_widgets((uint8_t)value_index, bar_min);
		}
	}

	// Check RPM timeout - instantly go to 0
	if (values_config[RPM_VALUE_ID - 1].enabled &&
		(current_time - last_rpm_can_received) > CAN_TIMEOUT_MS) {

		// Set RPM to "---" if it's not already
		if (strcmp(previous_values[RPM_VALUE_ID - 1], "---") != 0) {
			strcpy(previous_values[RPM_VALUE_ID - 1], "---");
			rpm_update_t *r_upd = malloc(sizeof(rpm_update_t));
			if (r_upd) {
				strcpy(r_upd->rpm_str, "---");
				r_upd->rpm_value = 0; // Use 0 for gauge
				lv_async_call(update_rpm_ui, r_upd);
			}
			_notify_text_widgets(RPM_VALUE_ID - 1);
			_notify_meter_widgets(RPM_VALUE_ID - 1, 0);
		}
	}
}
static inline int64_t extract_bits(const uint8_t *data, uint8_t bit_offset,
                                   uint8_t bit_length, int endian,
                                   bool is_signed)
{
    return can_extract_bits(data, bit_offset, bit_length, endian, is_signed);
}

/* Dispatch table (can_dispatch_entry_t, can_dispatch_entries, etc.) and
 * rebuild_can_dispatch() have moved to can/can_dispatch.c.  The dispatch
 * table globals are declared in can/can_dispatch.h which is included above,
 * so process_can_message() can still access them directly.              */

void process_can_message(const twai_message_t *message) {
	static uint64_t last_panel_updates[8] = {0};
	static uint64_t last_bar_updates[2] = {0};
	uint64_t current_time = esp_timer_get_time() / 1000; // ms
	uint32_t received_id = message->identifier;

	// Reset tracking variables after screen recreation to force UI updates
	if (reset_can_tracking) {
		reset_can_tracking = false;
		memset(last_panel_updates, 0, sizeof(last_panel_updates));
		memset(last_bar_updates, 0, sizeof(last_bar_updates));
		memset(previous_bit_states, 0, sizeof(previous_bit_states));
		// Reset CAN timeout tracking
		memset(last_panel_can_received, 0, sizeof(last_panel_can_received));
		memset(last_bar_can_received, 0, sizeof(last_bar_can_received));
		last_speed_can_received = 0;
		last_gear_can_received = 0;
		last_rpm_can_received = 0;
		// Force immediate updates by setting all times to 0
		for (int i = 0; i < 2; i++) {
			previous_bar_values[i] =
				-999; // Use impossible values to force bar updates
		}
	}

	// Fast-dispatch path using prebuilt mapping; fallback to linear scans if
	// not found
	int16_t didx =
		(received_id <= 0x7FF) ? can_id_to_dispatch_index[received_id] : -1;
	if (didx >= 0) {
		// Precompute 64-bit data value once for bit-extraction consumers
		uint64_t data_value = 0;
		for (int j = 0; j < message->data_length_code && j < 8; j++) {
			data_value |= (uint64_t)message->data[j] << (j * 8);
		}

		// Warnings
		for (uint8_t wi = 0; wi < can_dispatch_entries[didx].num_warning;
			 wi++) {
			int i = can_dispatch_entries[didx].warning_indices[wi];
			bool current_bit_state =
				(data_value >> warning_configs[i].bit_position) & 0x01;
			// Apply inversion if enabled
			if (warning_configs[i].invert_toggle) {
				current_bit_state = !current_bit_state;
			}
			if (warning_configs[i].is_momentary) {
				if (current_bit_state != warning_configs[i].current_state) {
					warning_configs[i].current_state = current_bit_state;
					if (current_bit_state) {
						last_signal_times[i] = current_time;
					}
				}
			} else {
				if (previous_bit_states[i] && !current_bit_state) {
					if (!toggle_debounce[i]) {
						toggle_debounce[i] = true;
						toggle_start_time[i] = current_time;
						warning_configs[i].current_state =
							!warning_configs[i].current_state;
					}
				} else if (!current_bit_state) {
					toggle_debounce[i] = false;
				}
			}
			previous_bit_states[i] = current_bit_state;
			update_warning_ui_immediate((uint8_t)i);
		}

		// Indicators (only when source is CAN BUS)
		for (uint8_t ii = 0; ii < can_dispatch_entries[didx].num_indicator;
			 ii++) {
			int i = can_dispatch_entries[didx].indicator_indices[ii];
			if (indicator_configs[i].input_source != 1)
				continue; /* Wire - state driven by analog */
			bool current_bit_state =
				(data_value >> indicator_configs[i].bit_position) & 0x01;
			if (indicator_configs[i].is_momentary) {
				if (current_bit_state != indicator_configs[i].current_state) {
					indicator_configs[i].current_state = current_bit_state;
				}
			} else {
				if (previous_indicator_bit_states[i] && !current_bit_state) {
					if (!indicator_toggle_debounce[i]) {
						indicator_toggle_debounce[i] = true;
						indicator_toggle_start_time[i] = current_time;
						indicator_configs[i].current_state =
							!indicator_configs[i].current_state;
					}
				} else if (!current_bit_state) {
					indicator_toggle_debounce[i] = false;
				}
			}
			previous_indicator_bit_states[i] = current_bit_state;
			update_indicator_ui_immediate((uint8_t)i);
		}

		// Values
		for (uint8_t vi = 0; vi < can_dispatch_entries[didx].num_values; vi++) {
			int i = can_dispatch_entries[didx].value_indices[vi];
			if (values_config[i].enabled) {
				uint8_t value_id = i + 1;
				int64_t raw_value = extract_bits(
					message->data, values_config[i].bit_start,
					values_config[i].bit_length, values_config[i].endianess,
					values_config[i].is_signed);
				double final_value =
					(double)raw_value * values_config[i].scale +
					values_config[i].value_offset;

				if (i < 8) {
					last_panel_can_received[i] = current_time;
					if (i == 6) {
						char new_value_str[EXAMPLE_MAX_CHAR_SIZE];
						if (values_config[i].decimals == 0) {
							snprintf(new_value_str, sizeof(new_value_str), "%d",
									 (int)final_value);
						} else {
							snprintf(new_value_str, sizeof(new_value_str),
									 "%.*f", values_config[i].decimals,
									 final_value);
						}
						if (strcmp(new_value_str, previous_values[i]) != 0) {
							strcpy(previous_values[i], new_value_str);
							update_panel_ui_immediate((uint8_t)i, new_value_str,
													  final_value);
							_notify_meter_widgets((uint8_t)i, (int32_t)final_value);
						}
					} else {
						if (current_time - last_panel_updates[i] >= 25) {
							char new_value_str[EXAMPLE_MAX_CHAR_SIZE];
							if (values_config[i].decimals == 0) {
								snprintf(new_value_str, sizeof(new_value_str),
										 "%d", (int)final_value);
							} else {
								snprintf(new_value_str, sizeof(new_value_str),
										 "%.*f", values_config[i].decimals,
										 final_value);
							}
							if (strcmp(new_value_str, previous_values[i]) !=
								0) {
								strcpy(previous_values[i], new_value_str);
								update_panel_ui_immediate(
									(uint8_t)i, new_value_str, final_value);
								_notify_text_widgets((uint8_t)i);
								_notify_meter_widgets((uint8_t)i, (int32_t)final_value);
							}
							last_panel_updates[i] = current_time;
						}
					}
					continue;
				}

				if (value_id == BAR1_VALUE_ID || value_id == BAR2_VALUE_ID) {
					int bar_index = value_id - BAR1_VALUE_ID;
					if (values_config[bar_index + BAR1_VALUE_ID - 1].fuel_sender) continue;
					last_bar_can_received[bar_index] = current_time;
					// Match panel refresh cadence: max 40 Hz (every 25 ms) and
					// update on any value change (threshold handled separately).
					if (current_time - last_bar_updates[bar_index] >= 25) {
						if (fabs(final_value - previous_bar_values[bar_index]) >=
							BAR_UPDATE_THRESHOLD) {
							previous_bar_values[bar_index] = final_value;
							lv_obj_t *bar_obj =
								(value_id == BAR1_VALUE_ID) ? ui_Bar_1 : ui_Bar_2;
							if (bar_obj) {
								int32_t bar_value = (int32_t)final_value;
								if (bar_value < values_config[i].bar_min)
									bar_value = values_config[i].bar_min;
								else if (bar_value > values_config[i].bar_max)
									bar_value = values_config[i].bar_max;

								// Apply inversion if enabled
								if (values_config[i].invert_bar_value) {
									bar_value = values_config[i].bar_max +
												values_config[i].bar_min -
												bar_value;
								}

								update_bar_ui_immediate(bar_index, bar_value,
														final_value, i);
								char bar_str[EXAMPLE_MAX_CHAR_SIZE];
								snprintf(bar_str, sizeof(bar_str), "%d", bar_value);
								strcpy(previous_values[i], bar_str);
								_notify_text_widgets((uint8_t)i);
								_notify_meter_widgets((uint8_t)i, bar_value);
							}
							last_bar_updates[bar_index] = current_time;
						}
					}
					continue;
				}

				if (value_id == RPM_VALUE_ID) {
					last_rpm_can_received = current_time;
					int rpm_value = (int)final_value;
					int gauge_rpm_value =
						rpm_value < 0
							? 0
							: (rpm_value > rpm_gauge_max ? rpm_gauge_max
														 : rpm_value);
					int display_rpm_value = (int)((float)rpm_value * 1.0229f);
					char rpm_str[EXAMPLE_MAX_CHAR_SIZE];
					snprintf(rpm_str, sizeof(rpm_str), "%d", display_rpm_value);
					static int last_rpm_value = -1;
					if (strcmp(rpm_str, previous_values[i]) != 0 ||
						rpm_value != last_rpm_value) {
						strcpy(previous_values[i], rpm_str);
						last_rpm_value = rpm_value;
						update_rpm_ui_immediate(rpm_str, gauge_rpm_value);
						_notify_text_widgets((uint8_t)i);
						_notify_meter_widgets((uint8_t)i, rpm_value);
					}
				} else if (value_id == SPEED_VALUE_ID) {
					last_speed_can_received = current_time;
					double speed_value = final_value;
					char speed_str[EXAMPLE_MAX_CHAR_SIZE];
					snprintf(speed_str, sizeof(speed_str), "%.0f", speed_value);
					if (strcmp(speed_str, previous_values[i]) != 0) {
						strcpy(previous_values[i], speed_str);
						update_speed_ui_immediate(speed_str);
						_notify_text_widgets((uint8_t)i);
						_notify_meter_widgets((uint8_t)i, (int32_t)speed_value);
					}
				} else if (value_id == GEAR_VALUE_ID) {
					// Skip CAN gear processing if in Speed/RPM Ratio mode
					if (values_config[GEAR_VALUE_ID - 1].gear_detection_mode == 4) {
						continue;
					}
					last_gear_can_received = current_time;
					
					// Get raw value BEFORE scaling/offset for icon matching
					// This is the actual CAN bus value that should match custom_icon_values
					int64_t gear_raw_value = extract_bits(
						message->data, values_config[GEAR_VALUE_ID - 1].bit_start,
						values_config[GEAR_VALUE_ID - 1].bit_length, 
						values_config[GEAR_VALUE_ID - 1].endianess,
						values_config[GEAR_VALUE_ID - 1].is_signed);
					
					char gear_str[EXAMPLE_MAX_CHAR_SIZE];
					if (strcasecmp(label_texts[GEAR_VALUE_ID - 1], "GEAR") ==
						0) {
						uint8_t gear_mode = values_config[GEAR_VALUE_ID - 1]
												.gear_detection_mode;
						if (gear_mode == 1) {
							// MaxxECU gear detection: -3=Park, -1=Reverse, 0=Neutral, >0=Forward gears
							if (final_value == 0) {
								snprintf(gear_str, sizeof(gear_str), "N");
							} else if (final_value == -1) {
								snprintf(gear_str, sizeof(gear_str), "R");
							} else if (final_value >= 1 && final_value <= 10) {
								snprintf(gear_str, sizeof(gear_str), "%d",
										 (int)final_value);
							} else {
								snprintf(gear_str, sizeof(gear_str), "-");
							}
						} else if (gear_mode == 2) {
							if (final_value == 0) {
								snprintf(gear_str, sizeof(gear_str), "N");
							} else if (final_value == 255 ||
									   final_value == 0xFE) {
								snprintf(gear_str, sizeof(gear_str), "R");
							} else if (final_value >= 1 && final_value <= 8) {
								snprintf(gear_str, sizeof(gear_str), "%d",
										 (int)final_value);
							} else {
								snprintf(gear_str, sizeof(gear_str), "-");
							}
						} else {
							// Custom gear detection - check if received value
							// matches any configured gear value
							bool gear_found = false;
							for (int gear_idx = 0; gear_idx < 14;
								 gear_idx++) { // 0-13: P, R, N, D, 1-10
							if (values_config[GEAR_VALUE_ID - 1]
										.gear_custom_values[gear_idx] ==
									(uint32_t)gear_raw_value) {
								if (gear_idx == 0) {
									snprintf(gear_str, sizeof(gear_str),
											 "P");
								} else if (gear_idx == 1) {
									snprintf(gear_str, sizeof(gear_str),
											 "R");
								} else if (gear_idx == 2) {
									snprintf(gear_str, sizeof(gear_str),
											 "N");
								} else if (gear_idx == 3) {
									snprintf(gear_str, sizeof(gear_str),
											 "D");
								} else {
									snprintf(gear_str, sizeof(gear_str),
											 "%d", gear_idx - 3);
								}
								gear_found = true;
								break;
							}
						}
						if (!gear_found) {
							snprintf(
								gear_str, sizeof(gear_str),
								"-"); // Show dash if no gear value matches
						}
					}
				} else {
					snprintf(gear_str, sizeof(gear_str), "%.0f",
							 final_value);
				}
				// Always update if gear string changed OR if raw value changed (for icon matching)
				// This ensures icon appears even if gear_str is "-" but icon value matches
				bool gear_str_changed = (strcmp(gear_str, previous_values[GEAR_VALUE_ID - 1]) != 0);
				static uint32_t last_raw_value = 0;
				bool raw_value_changed = ((uint32_t)gear_raw_value != last_raw_value);
				
				if (gear_str_changed || raw_value_changed) {
					if (gear_str_changed) {
						strcpy(previous_values[GEAR_VALUE_ID - 1], gear_str);
					}
					last_raw_value = (uint32_t)gear_raw_value;
					// Use raw value (before scaling/offset) for icon matching
					update_gear_ui_immediate(gear_str, (uint32_t)gear_raw_value);
					_notify_text_widgets(GEAR_VALUE_ID - 1);
					_notify_meter_widgets(GEAR_VALUE_ID - 1, (int32_t)final_value);
				}
				}
			}
		}
		return; // handled via fast path
	}

	// Process warning configurations (for warnings 0-7)
	for (int i = 0; i < 8; i++) {
		if (warning_configs[i].can_id == received_id) {
			// Create 64-bit value from message data
			uint64_t data_value = 0;
			for (int j = 0; j < message->data_length_code && j < 8; j++) {
				data_value |= (uint64_t)message->data[j] << (j * 8);
			}

			// Extract bit using 64-bit position
			bool current_bit_state =
				(data_value >> warning_configs[i].bit_position) & 0x01;
			// Apply inversion if enabled
			if (warning_configs[i].invert_toggle) {
				current_bit_state = !current_bit_state;
			}

			if (warning_configs[i].is_momentary) {
				// For momentary mode: activate warning directly based on bit
				// state (active high) This will show warning when bit is 1,
				// hide when bit is 0
				if (current_bit_state != warning_configs[i].current_state) {
					warning_configs[i].current_state = current_bit_state;
					if (current_bit_state) {
						last_signal_times[i] = current_time;
					}
				}
			} else {
				// For toggle mode: toggle on falling edge (1->0 transition)
				if (previous_bit_states[i] && !current_bit_state) {
					if (!toggle_debounce[i]) {
						toggle_debounce[i] = true;
						toggle_start_time[i] = current_time;
						warning_configs[i].current_state =
							!warning_configs[i].current_state;
					}
				} else if (!current_bit_state) {
					toggle_debounce[i] = false;
				}
			}
			previous_bit_states[i] = current_bit_state;

			// Update warning UI
			uint8_t *w_idx = malloc(sizeof(uint8_t));
			if (w_idx) {
				*w_idx = i;
				lv_async_call(update_warning_ui, w_idx);
			}
		}
	}

	// Process brightness dimmer switch
	extern brightness_dimmer_config_t dimmer_config;
	extern bool previous_dimmer_bit_state;
	if (dimmer_config.enabled && dimmer_config.can_id == received_id) {
		// Create 64-bit value from message data
		uint64_t data_value = 0;
		for (int j = 0; j < message->data_length_code && j < 8; j++) {
			data_value |= (uint64_t)message->data[j] << (j * 8);
		}
		
		// Extract bit
		bool current_bit_state = (data_value >> dimmer_config.bit_position) & 0x01;
		
		// Apply inversion if enabled
		if (dimmer_config.invert_toggle) {
			current_bit_state = !current_bit_state;
		}
		
		if (dimmer_config.is_momentary) {
			// Momentary mode: set brightness when bit is active
			if (current_bit_state) {
				extern void set_display_brightness(int percent);
				set_display_brightness(dimmer_config.brightness_value);
			}
		} else {
			// Toggle mode: toggle brightness on falling edge (1->0 transition)
			if (previous_dimmer_bit_state && !current_bit_state) {
				extern void set_display_brightness(int percent);
				extern uint8_t current_brightness;
				// Toggle: if currently at dimmer brightness, restore to 100%, otherwise set to dimmer brightness
				if (current_brightness == dimmer_config.brightness_value) {
					set_display_brightness(100);
				} else {
					set_display_brightness(dimmer_config.brightness_value);
				}
			}
		}
		previous_dimmer_bit_state = current_bit_state;
	}

	// Process indicator configurations (for left and right indicators; only when source is CAN BUS)
	for (int i = 0; i < 2; i++) {
		if (indicator_configs[i].input_source != 1)
			continue; /* Wire - state driven by analog */
		if (indicator_configs[i].can_id == received_id) {
			// Create 64-bit value from message data
			uint64_t data_value = 0;
			for (int j = 0; j < message->data_length_code && j < 8; j++) {
				data_value |= (uint64_t)message->data[j] << (j * 8);
			}

			// Extract bit using 64-bit position
			bool current_bit_state =
				(data_value >> indicator_configs[i].bit_position) & 0x01;

			if (indicator_configs[i].is_momentary) {
				// For momentary mode: activate indicator directly based on bit
				// state (active high)
				if (current_bit_state != indicator_configs[i].current_state) {
					indicator_configs[i].current_state = current_bit_state;
				}
			} else {
				// For toggle mode: toggle on falling edge (1->0 transition)
				if (previous_indicator_bit_states[i] && !current_bit_state) {
					if (!indicator_toggle_debounce[i]) {
						indicator_toggle_debounce[i] = true;
						indicator_toggle_start_time[i] = current_time;
						indicator_configs[i].current_state =
							!indicator_configs[i].current_state;
					}
				} else if (!current_bit_state) {
					indicator_toggle_debounce[i] = false;
				}
			}
			previous_indicator_bit_states[i] = current_bit_state;

			// Update indicator UI
			uint8_t *ind_idx = malloc(sizeof(uint8_t));
			if (ind_idx) {
				*ind_idx = i;
				lv_async_call(update_indicator_ui, ind_idx);
			}
		}
	}

	// Process value configurations
	for (int i = 0; i < 13; i++) {
		if (values_config[i].enabled &&
			values_config[i].can_id == received_id) {
			uint8_t value_id = i + 1;
			int64_t raw_value = extract_bits(
				message->data, values_config[i].bit_start,
				values_config[i].bit_length, values_config[i].endianess,
				values_config[i].is_signed);
			double final_value = (double)raw_value * values_config[i].scale +
								 values_config[i].value_offset;

			// Handle panels (values 1-8)
			if (i < 8) {
				// Update CAN received timestamp for this panel
				last_panel_can_received[i] = current_time;

				// Special handling for wideband (panel 7)
				if (i == 6) { // Panel 7 (wideband)
					char new_value_str[EXAMPLE_MAX_CHAR_SIZE];
					if (values_config[i].decimals == 0) {
						snprintf(new_value_str, sizeof(new_value_str), "%d",
								 (int)final_value);
					} else {
						snprintf(new_value_str, sizeof(new_value_str), "%.*f",
								 values_config[i].decimals, final_value);
					}
					if (strcmp(new_value_str, previous_values[i]) != 0) {
						strcpy(previous_values[i], new_value_str);
						panel_update_t *p_upd = malloc(sizeof(panel_update_t));
						if (p_upd) {
							p_upd->panel_index = i;
							strcpy(p_upd->value_str, new_value_str);
							p_upd->final_value = final_value;
							lv_async_call(update_panel_ui, p_upd);
						}
						_notify_text_widgets((uint8_t)i);
						_notify_meter_widgets((uint8_t)i, (int32_t)final_value);
					}
				} else {
					// Other panels with 25ms update rate
					if (current_time - last_panel_updates[i] >= 25) {
						char new_value_str[EXAMPLE_MAX_CHAR_SIZE];
						if (values_config[i].decimals == 0) {
							snprintf(new_value_str, sizeof(new_value_str), "%d",
									 (int)final_value);
						} else {
							snprintf(new_value_str, sizeof(new_value_str),
									 "%.*f", values_config[i].decimals,
									 final_value);
						}
						if (strcmp(new_value_str, previous_values[i]) != 0) {
							strcpy(previous_values[i], new_value_str);
							panel_update_t *p_upd =
								malloc(sizeof(panel_update_t));
							if (p_upd) {
								p_upd->panel_index = i;
								strcpy(p_upd->value_str, new_value_str);
								p_upd->final_value = final_value;
								lv_async_call(update_panel_ui, p_upd);
							}
							_notify_text_widgets((uint8_t)i);
							_notify_meter_widgets((uint8_t)i, (int32_t)final_value);
						}
						last_panel_updates[i] = current_time;
					}
				}
				continue;
			}

			// Handle bars (for BAR1 and BAR2)
			if (value_id == BAR1_VALUE_ID || value_id == BAR2_VALUE_ID) {
				int bar_index = value_id - BAR1_VALUE_ID;
				if (values_config[bar_index + BAR1_VALUE_ID - 1].fuel_sender) continue;
				// Update CAN received timestamp for this bar
				last_bar_can_received[bar_index] = current_time;

				// Update both bars immediately without throttling for better
				// responsiveness
				if (fabs(final_value - previous_bar_values[bar_index]) >=
					BAR_UPDATE_THRESHOLD) {
					previous_bar_values[bar_index] = final_value;
					lv_obj_t *bar_obj =
						(value_id == BAR1_VALUE_ID) ? ui_Bar_1 : ui_Bar_2;
					if (bar_obj) {
						int32_t bar_value = (int32_t)final_value;
						// Clamp the value per configuration.
						if (bar_value < values_config[i].bar_min) {
							bar_value = values_config[i].bar_min;
						} else if (bar_value > values_config[i].bar_max) {
							bar_value = values_config[i].bar_max;
						}
						
						// Apply inversion if enabled
						if (values_config[i].invert_bar_value) {
							bar_value = values_config[i].bar_max + values_config[i].bar_min - bar_value;
						}

						// Create and fill our bar update data.
						bar_update_t *b_upd = malloc(sizeof(bar_update_t));
						if (b_upd) {
							b_upd->bar_index = bar_index;
							b_upd->bar_value = bar_value;
							b_upd->final_value = final_value;
							b_upd->config_index = i;
							b_upd->is_timeout = false;
							lv_async_call(update_bar_ui, b_upd);
						}
						char bar_str[EXAMPLE_MAX_CHAR_SIZE];
						snprintf(bar_str, sizeof(bar_str), "%d", bar_value);
						strcpy(previous_values[i], bar_str);
						_notify_text_widgets((uint8_t)i);
						_notify_meter_widgets((uint8_t)i, bar_value);
					}
				}
				continue;
			}

			// Handle RPM
			if (value_id == RPM_VALUE_ID) {
				// Update CAN received timestamp for RPM
				last_rpm_can_received = current_time;

				// Always process RPM updates for better responsiveness
				int rpm_value = (int)final_value;

				// For the gauge bar, limit to rpm_gauge_max
				int gauge_rpm_value =
					rpm_value < 0 ? 0
								  : (rpm_value > rpm_gauge_max ? rpm_gauge_max
															   : rpm_value);

				// For the display value, apply 102.3% scaling to the actual CAN
				// value (no gauge limit)
				int display_rpm_value =
					(int)((float)rpm_value *
						  1.0229f); // 102.3% scaling of actual CAN value

				// Update the display string with the scaled actual CAN value
				char rpm_str[EXAMPLE_MAX_CHAR_SIZE];
				snprintf(rpm_str, sizeof(rpm_str), "%d", display_rpm_value);

				// Update if string changed OR if actual RPM value changed
				// This ensures limiter effects activate immediately
				static int last_rpm_value = -1;
				if (strcmp(rpm_str, previous_values[i]) != 0 ||
					rpm_value != last_rpm_value) {
					strcpy(previous_values[i], rpm_str);
					last_rpm_value = rpm_value;

					rpm_update_t *r_upd = malloc(sizeof(rpm_update_t));
					if (r_upd) {
						strcpy(r_upd->rpm_str, rpm_str);
						r_upd->rpm_value =
							gauge_rpm_value; // Use gauge-limited value for bar
						lv_async_call(update_rpm_ui, r_upd);
					}
					_notify_text_widgets((uint8_t)i);
					_notify_meter_widgets((uint8_t)i, rpm_value);
				}
			}
			// Handle Speed
			else if (value_id == SPEED_VALUE_ID) {
				// Update CAN received timestamp for speed
				last_speed_can_received = current_time;

				// Use raw CAN bus value without conversion - let CAN provide
				// the value in desired units
				double speed_value = final_value;

				char speed_str[EXAMPLE_MAX_CHAR_SIZE];
				snprintf(speed_str, sizeof(speed_str), "%.0f", speed_value);

				if (strcmp(speed_str, previous_values[i]) != 0) {
					strcpy(previous_values[i], speed_str);
					speed_update_t *s_upd = malloc(sizeof(speed_update_t));
					if (s_upd) {
						strcpy(s_upd->speed_str, speed_str);
						lv_async_call(update_speed_ui, s_upd);
					}
					_notify_text_widgets((uint8_t)i);
					_notify_meter_widgets((uint8_t)i, (int32_t)speed_value);
				}
			}
			// Handle Gear
			else if (value_id == GEAR_VALUE_ID) {
				// Skip CAN gear processing if in Speed/RPM Ratio mode
				if (values_config[GEAR_VALUE_ID - 1].gear_detection_mode == 4) {
					continue;
				}
				
				// Update CAN received timestamp for gear
				last_gear_can_received = current_time;

				// Get raw value BEFORE scaling/offset for icon matching
				// This is the actual CAN bus value that should match custom_icon_values
				int64_t gear_raw_value = extract_bits(
					message->data, values_config[GEAR_VALUE_ID - 1].bit_start,
					values_config[GEAR_VALUE_ID - 1].bit_length, 
					values_config[GEAR_VALUE_ID - 1].endianess,
					values_config[GEAR_VALUE_ID - 1].is_signed);

				char gear_str[EXAMPLE_MAX_CHAR_SIZE];

				// Check if label is "GEAR" (case insensitive)
				if (strcasecmp(label_texts[GEAR_VALUE_ID - 1], "GEAR") == 0) {
					// Format gear value based on detection mode
					uint8_t gear_mode =
						values_config[GEAR_VALUE_ID - 1].gear_detection_mode;

					if (gear_mode == 1) {
						// MaxxECU gear detection: -3=Park, -1=Reverse, 0=Neutral, >0=Forward gears
						if (final_value == 0) {
							snprintf(gear_str, sizeof(gear_str),
									 "N"); // Neutral
						} else if (final_value == -1) {
							snprintf(gear_str, sizeof(gear_str),
									 "R"); // Reverse
						} else if (final_value >= 1 && final_value <= 10) {
							snprintf(gear_str, sizeof(gear_str), "%d",
									 (int)final_value);
						} else {
							snprintf(gear_str, sizeof(gear_str),
									 "-"); // Invalid gear
						}
					} else if (gear_mode == 2) {
						// Haltech gear detection (different encoding)
						if (final_value == 0) {
							snprintf(gear_str, sizeof(gear_str),
									 "N"); // Neutral
						} else if (final_value == 255 || final_value == 0xFE) {
							snprintf(gear_str, sizeof(gear_str),
									 "R"); // Reverse
						} else if (final_value >= 1 && final_value <= 8) {
							snprintf(gear_str, sizeof(gear_str), "%d",
									 (int)final_value);
						} else {
							snprintf(gear_str, sizeof(gear_str),
									 "-"); // Invalid gear
						}
					} else {
						// Custom gear detection (mode 0)
						// Check if received value matches any configured gear
						// value
						// Use raw value (before scaling/offset) for matching
						bool gear_found = false;

						// Check each custom gear value
						// UINT32_MAX means "not configured", so skip those
						// 0 is now a valid configured value
						for (int gear_idx = 0; gear_idx < 14;
							 gear_idx++) { // 0-13: P, R, N, D, 1-10
							uint32_t configured_value = values_config[GEAR_VALUE_ID - 1].gear_custom_values[gear_idx];
							// Skip if not configured (UINT32_MAX)
							if (configured_value == UINT32_MAX) {
								continue;
							}
							if (configured_value == (uint32_t)gear_raw_value) {
								if (gear_idx == 0) {
									snprintf(gear_str, sizeof(gear_str),
											 "P"); // Park
								} else if (gear_idx == 1) {
									snprintf(gear_str, sizeof(gear_str),
											 "R"); // Reverse
								} else if (gear_idx == 2) {
									snprintf(gear_str, sizeof(gear_str),
											 "N"); // Neutral
								} else if (gear_idx == 3) {
									snprintf(gear_str, sizeof(gear_str),
											 "D"); // Drive
								} else {
									snprintf(gear_str, sizeof(gear_str), "%d",
											 gear_idx - 3); // Gears 1-10
								}
								gear_found = true;
								break;
							}
						}

						if (!gear_found) {
							snprintf(gear_str, sizeof(gear_str),
									 "-"); // Show dash if no gear value matches
						}
					}
				} else {
					// Use normal numeric formatting if label isn't "GEAR"
					snprintf(gear_str, sizeof(gear_str), "%.0f", final_value);
				}

				// Always update if gear string changed OR if raw value changed (for icon matching)
				// This ensures icon appears even if gear_str is "-" but icon value matches
				bool gear_str_changed = (strcmp(gear_str, previous_values[GEAR_VALUE_ID - 1]) != 0);
				static uint32_t last_raw_value_slow = 0;
				bool raw_value_changed = ((uint32_t)gear_raw_value != last_raw_value_slow);
				
				if (gear_str_changed || raw_value_changed) {
					if (gear_str_changed) {
						strcpy(previous_values[GEAR_VALUE_ID - 1], gear_str);
					}
					last_raw_value_slow = (uint32_t)gear_raw_value;
					gear_update_t *g_upd = malloc(sizeof(gear_update_t));
					if (g_upd) {
						strcpy(g_upd->gear_str, gear_str);
						// Use raw value (before scaling/offset) for icon matching
						g_upd->raw_value = (uint32_t)gear_raw_value;
						lv_async_call(update_gear_ui, g_upd);
					}
					_notify_text_widgets(GEAR_VALUE_ID - 1);
					_notify_meter_widgets(GEAR_VALUE_ID - 1, (int32_t)final_value);
				}
			}
		}
	}
}
void init_values_config_defaults(void) {
	for (int i = 0; i < 13; i++) {
		values_config[i].enabled = false;
		values_config[i].can_id = 0;
		values_config[i].endianess = 0;
		values_config[i].bit_start = 0;
		values_config[i].bit_length = 0;
		values_config[i].decimals = 0;
		values_config[i].value_offset = 0;
		values_config[i].scale = 1;
		values_config[i].is_signed = false;
		// Initialize custom text field to empty string
		memset(values_config[i].custom_text, 0,
			   sizeof(values_config[i].custom_text));

		if (i < 8) {
			values_config[i].warning_high_threshold = 0;
			values_config[i].warning_low_threshold = 0;
			values_config[i].warning_high_color =
				THEME_COLOR_RED; // Default red
			values_config[i].warning_low_color =
				THEME_COLOR_BLUE_DARK; // Default blue
			values_config[i].warning_high_enabled = false;
			values_config[i].warning_low_enabled = false;
		}
	}

	values_config[RPM_VALUE_ID - 1].enabled = true;
	values_config[RPM_VALUE_ID - 1].rpm_bar_color =
		THEME_COLOR_RED;								// Default red
	values_config[RPM_VALUE_ID - 1].rpm_limiter_effect = 0; // Default: None
	values_config[RPM_VALUE_ID - 1].rpm_limiter_value =
		7000; // Default: 7000 RPM
	values_config[RPM_VALUE_ID - 1].rpm_limiter_color =
		THEME_COLOR_RED; // Default: Red
	values_config[RPM_VALUE_ID - 1].rpm_lights_enabled =
		false; // Default: Disabled
	values_config[RPM_VALUE_ID - 1].rpm_background_enabled =
		false; // Default: Disabled
	values_config[RPM_VALUE_ID - 1].rpm_background_value =
		7000; // Default: 7000 RPM
	values_config[RPM_VALUE_ID - 1].rpm_background_color =
		THEME_COLOR_GREEN; // Default: Green
	values_config[SPEED_VALUE_ID - 1].enabled = true;
	values_config[GEAR_VALUE_ID - 1].enabled = true;
	values_config[GEAR_VALUE_ID - 1].gear_detection_mode =
		1; // Default to MaxxECU
	// Initialize custom gear values to UINT32_MAX (not configured)
	for (int j = 0; j < 14; j++) {
		values_config[GEAR_VALUE_ID - 1].gear_custom_values[j] = UINT32_MAX;
	}
	// Initialize custom icon values to UINT32_MAX (not configured)
	for (int j = 0; j < 7; j++) {
		values_config[GEAR_VALUE_ID - 1].custom_icon_values[j] = UINT32_MAX;
	}
	// Initialize Speed/RPM Ratio default values
	values_config[GEAR_VALUE_ID - 1].tire_circumference_mm = 2000.0f; // Default: 2000mm (typical car tire)
	values_config[GEAR_VALUE_ID - 1].final_drive_ratio = 3.420f; // Default: 3.42 (common ratio)
	values_config[GEAR_VALUE_ID - 1].reverse_gear_ratio = 3.50f; // Default: 3.50 (typical reverse gear ratio)
	// Initialize gear ratios with typical 6-speed manual transmission ratios
	values_config[GEAR_VALUE_ID - 1].gear_ratios[0] = 3.36f;  // 1st gear
	values_config[GEAR_VALUE_ID - 1].gear_ratios[1] = 2.07f;  // 2nd gear
	values_config[GEAR_VALUE_ID - 1].gear_ratios[2] = 1.40f;  // 3rd gear
	values_config[GEAR_VALUE_ID - 1].gear_ratios[3] = 1.00f;  // 4th gear
	values_config[GEAR_VALUE_ID - 1].gear_ratios[4] = 0.84f;  // 5th gear
	values_config[GEAR_VALUE_ID - 1].gear_ratios[5] = 0.56f;  // 6th gear
	values_config[GEAR_VALUE_ID - 1].gear_ratios[6] = 0.0f;   // 7th gear (unused)
	values_config[GEAR_VALUE_ID - 1].gear_ratios[7] = 0.0f;   // 8th gear (unused)
	values_config[GEAR_VALUE_ID - 1].gear_ratios[8] = 0.0f;   // 9th gear (unused)
	values_config[GEAR_VALUE_ID - 1].gear_ratios[9] = 0.0f;   // 10th gear (unused)
	values_config[BAR1_VALUE_ID - 1].enabled = true;
	values_config[BAR2_VALUE_ID - 1].enabled = true;

	// Set default ranges for bars
	values_config[BAR1_VALUE_ID - 1].bar_min = 0;
	values_config[BAR1_VALUE_ID - 1].bar_max = 100;
	values_config[BAR2_VALUE_ID - 1].bar_min = 0;
	values_config[BAR2_VALUE_ID - 1].bar_max = 100;

	// Set default bar colors for BAR1 and BAR2
	values_config[BAR1_VALUE_ID - 1].bar_low_color =
		THEME_COLOR_BLUE_DARK; // Blue
	values_config[BAR1_VALUE_ID - 1].bar_high_color =
		THEME_COLOR_RED; // Red
	values_config[BAR1_VALUE_ID - 1].bar_in_range_color =
		THEME_COLOR_GREEN_BRIGHT; // Green
	values_config[BAR1_VALUE_ID - 1].show_bar_value =
		true; // Show value by default
	values_config[BAR1_VALUE_ID - 1].invert_bar_value =
		false; // Don't invert by default

	values_config[BAR2_VALUE_ID - 1].bar_low_color =
		THEME_COLOR_BLUE_DARK; // Blue
	values_config[BAR2_VALUE_ID - 1].bar_high_color =
		THEME_COLOR_RED; // Red
	values_config[BAR2_VALUE_ID - 1].bar_in_range_color =
		THEME_COLOR_GREEN_BRIGHT; // Green
	values_config[BAR2_VALUE_ID - 1].show_bar_value =
		true; // Show value by default
	values_config[BAR2_VALUE_ID - 1].invert_bar_value =
		false; // Don't invert by default
}
void refresh_screen3_labels(void) {
	printf("DEBUG: Refreshing Screen3 labels\n");

	// Update panel labels (1-8)
	for (int i = 0; i < 8; i++) {
		if (ui_Label[i] && lv_obj_is_valid(ui_Label[i])) {
			lv_label_set_text(ui_Label[i], label_texts[i]);
		}
	}

	// Update gear label
	if (ui_Gear_Label && lv_obj_is_valid(ui_Gear_Label)) {
		lv_label_set_text(ui_Gear_Label, label_texts[GEAR_VALUE_ID - 1]);
	}

	// Update bar labels
	if (ui_Bar_1_Label && lv_obj_is_valid(ui_Bar_1_Label)) {
		lv_label_set_text(ui_Bar_1_Label, label_texts[BAR1_VALUE_ID - 1]);
	}
	if (ui_Bar_2_Label && lv_obj_is_valid(ui_Bar_2_Label)) {
		lv_label_set_text(ui_Bar_2_Label, label_texts[BAR2_VALUE_ID - 1]);
	}

	printf("DEBUG: Screen3 labels refreshed\n");
}
