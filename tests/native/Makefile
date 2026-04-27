# Native-host test runner for RDM-7 Dash.
# No ESP-IDF, no FreeRTOS, no LVGL — just stock GCC/Clang.
#
# Usage:
#   make             # build & run every test, fail on first failure
#   make test_can_decode  # build & run a single test
#   make clean

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wno-unused-function -O0 -g
LDFLAGS ?= -lm

# One test binary per file in the TESTS list. Add new tests here.
TESTS = test_can_decode

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

# Pattern rule: test_<name>.c + unity.c → build/test_<name>
$(BIN_DIR)/%: %.c unity.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $< unity.c -o $@ $(LDFLAGS)

clean:
	rm -rf $(BIN_DIR)
