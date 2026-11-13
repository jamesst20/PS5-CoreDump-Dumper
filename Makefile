# SPDX-License-Identifier: MIT
#   Copyright (C) 2026

PS5_HOST ?= ps5
PS5_PORT ?= 9021

VERSION := $(shell cat VERSION)
DIST    := dist
ELF     := $(DIST)/coredump-dumper-v$(VERSION).elf
SOURCES := $(wildcard src/*.c)
HEADERS := $(wildcard src/*.h)
CLANG_FORMAT ?= clang-format

# The SDK's prospero-llvm-config needs an llvm-config on PATH.
ifeq ($(shell command -v llvm-config),)
ifneq ($(wildcard /opt/homebrew/opt/llvm@18/bin/llvm-config),)
export PATH := /opt/homebrew/opt/llvm@18/bin:$(PATH)
endif
endif

# Only the payload targets need the PS5 toolchain; the host test does not.
GOALS := $(or $(MAKECMDGOALS),all)
ifneq ($(filter all payload deploy,$(GOALS)),)
    ifndef PS5_PAYLOAD_SDK
        $(error PS5_PAYLOAD_SDK is undefined)
    endif
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

CFLAGS := -Wall -Werror -Wextra -O2 -g -Isrc -DDUMPER_VERSION='"$(VERSION)"'

.PHONY: all payload deploy version clean format host-test

all: payload

payload: $(ELF)

$(ELF): $(SOURCES) $(HEADERS) VERSION
	@mkdir -p $(DIST)
	$(CC) $(CFLAGS) -o $@ $(SOURCES)

deploy: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

version:
	@echo $(VERSION)

clean:
	rm -rf $(DIST)

format:
	$(CLANG_FORMAT) -i $(SOURCES) $(HEADERS) test/host_stubs.c

host-test:
	./test/run_host_test.sh
