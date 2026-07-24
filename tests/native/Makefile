# Native-host test runner for RDM-7 Dash.
# No ESP-IDF, no FreeRTOS, no LVGL — just stock GCC/Clang.
#
# Usage:
#   make                          # build & run every test, fail on first failure
#   make test_can_decode          # build & run a single test
#   make test_widget_rules        # build & run a single test
#   make test_layout_migration    # build & run a single test
#   make clean

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wno-unused-function -O0 -g
LDFLAGS ?= -lm

# One test binary per file in the TESTS list. Add new tests here.
#
# NOTE: run.ps1 discovers tests by globbing test_*.c, this list is by hand, and
# the two drifted apart — test_obd2_pid_table and test_widget_warning_precedence
# existed for a while but only ever ran on Windows. If you add a test, add it
# here as well as writing the file.
TESTS = test_can_decode test_widget_rules test_layout_migration test_widget_arc_precedence test_obd2_freeze_frame test_obd2_one_shot_decode test_obd2_pid_table test_ota_skip_version test_calculated_gear test_web_path_safety test_ecu_preset_match test_widget_fields test_widget_warning_precedence test_hardening_fixes test_lap_core

BIN_DIR := build
BINS    := $(addprefix $(BIN_DIR)/, $(TESTS))

.PHONY: all clean $(TESTS)

all: $(TESTS)

# Per-test phony target: build then run.
$(TESTS): %: $(BIN_DIR)/%
	@echo ""
	@echo "▶ Running $@"
	@$<

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

# Default pattern rule: test_<name>.c + unity.c → build/test_<name>.
# Tests with extra sources / include paths get explicit recipes below
# that override this pattern.
$(BIN_DIR)/%: %.c unity.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $< unity.c -o $@ $(LDFLAGS)

# widget_rules pulls widget_rules.c (the firmware source) plus a vendored
# cJSON, with the mocks/ stubs first on the include path so widget_types.h
# resolves <lvgl.h>, <esp_log.h>, <esp_heap_caps.h> to our shims.
$(BIN_DIR)/test_widget_rules: test_widget_rules.c unity.c cjson/cJSON.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -Imocks -Icjson -I. \
	    test_widget_rules.c unity.c cjson/cJSON.c \
	    -o $@ $(LDFLAGS)

# Schema-migration test reimplements the small migration helpers verbatim
# from layout_manager.c / widget_rpm_bar.c (see header comment) and pulls
# vendored cJSON. No firmware source compiled.
$(BIN_DIR)/test_layout_migration: test_layout_migration.c unity.c cjson/cJSON.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -Icjson \
	    test_layout_migration.c unity.c cjson/cJSON.c \
	    -o $@ $(LDFLAGS)

# widget_fields.h is header-only (no firmware .c to compile) but pulls
# widget_types.h via "widgets/widget_types.h" — needs main/ itself on the
# include path, plus the same LVGL/cJSON mocks widget_types.h requires.
$(BIN_DIR)/test_widget_fields: test_widget_fields.c unity.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -Imocks -Icjson -I../../main \
	    test_widget_fields.c unity.c \
	    -o $@ $(LDFLAGS)

# obd2_pid_table compiles the real PID table straight out of the firmware.
$(BIN_DIR)/test_obd2_pid_table: test_obd2_pid_table.c unity.c ../../main/can/obd2_pids.c | $(BIN_DIR)
	$(CC) $(CFLAGS) \
	    test_obd2_pid_table.c unity.c ../../main/can/obd2_pids.c \
	    -o $@ $(LDFLAGS)

# lap_core compiles the firmware source DIRECTLY rather than mirroring its
# arithmetic in the test the way test_calculated_gear.c has to. That is
# possible because main/lap/lap_core.c deliberately has no LVGL/FreeRTOS/IDF
# dependency, and it is the pattern to prefer for new modules — a mirrored
# test can drift out of lockstep without anything failing.
$(BIN_DIR)/test_lap_core: test_lap_core.c unity.c ../../main/lap/lap_core.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -I../../main \
	    test_lap_core.c unity.c ../../main/lap/lap_core.c \
	    -o $@ $(LDFLAGS)

clean:
	rm -rf $(BIN_DIR)
