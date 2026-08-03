# Makefile for stub unit tests.
#
# Usage:
#   make                       - Build and run tests (defaults to BUILD_MODE=dev)
#   make build                 - Build the test binaries without running them
#   make BUILD_MODE=prod test  - Run in prod mode
#   make clean                 - Clean build artifacts

CC = cc
CFLAGS = -Wall -Wextra -std=c11 -g -I../src -I../../../../../../bin-infra/src

# Mirror the main package layout build/<mode>/<platform-arch>/out/ so CI
# and local builds don't collide and the **/build/ gitignore rule covers it.
BUILD_MODE ?= dev
PLATFORM ?= $(shell uname -s | tr '[:upper:]' '[:lower:]' | sed 's/darwin/darwin/;s/linux/linux/')
ARCH ?= $(shell uname -m | sed 's/x86_64/x64/;s/aarch64/arm64/;s/arm64/arm64/')
PLATFORM_ARCH ?= $(PLATFORM)-$(ARCH)
OUT_DIR = build/$(BUILD_MODE)/$(PLATFORM_ARCH)/out

# Test targets.
TESTS = stub_environ_test update_config_test

.PHONY: all build test clean

all: test

# Compile only. The vitest wrapper builds with this target and runs each
# binary itself, so a failing case is reported per binary.
build: $(TESTS:%=$(OUT_DIR)/%)

test: build
	@echo ""
	@echo "Running tests..."
	@for test in $(TESTS:%=$(OUT_DIR)/%); do \
		./$$test || exit 1; \
	done

$(OUT_DIR)/stub_environ_test: stub_environ_test.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT_DIR)/update_config_test: update_config_test.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

clean:
	rm -rf $(OUT_DIR)
