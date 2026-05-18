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
TESTS = test_can_decode test_widget_rules test_layout_migration test_widget_arc_precedence

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

clean:
	rm -rf $(BIN_DIR)
