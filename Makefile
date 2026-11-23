# SPDX-License-Identifier: MIT
#   Copyright (C) 2026

PS5_HOST ?= ps5
PS5_PORT ?= 9021

VERSION := $(shell cat VERSION)
DIST    := dist
ELF     := $(DIST)/coredump-dumper-v$(VERSION).elf
SOURCES := $(wildcard src/*.c)
HEADERS := $(filter-out src/web_assets.h,$(wildcard src/*.h))
CLANG_FORMAT ?= clang-format

WEB        := web
WEB_DIST   := $(WEB)/dist
WEB_ASSETS := src/web_assets.h
WEB_INPUTS := $(shell find $(WEB)/src -type f 2>/dev/null) \
	$(WEB)/index.html $(WEB)/vite.config.js $(WEB)/svelte.config.js \
	$(WEB)/package.json $(WEB)/package-lock.json

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

.PHONY: all payload deploy version clean format host-test ui

all: payload

payload: $(ELF)

# The UI is compiled into the ELF, so a payload build needs Node once to
# install the frontend dependencies.
ui: $(WEB_ASSETS)

$(WEB)/node_modules: $(WEB)/package-lock.json
	cd $(WEB) && npm ci

$(WEB_DIST)/index.html: $(WEB_INPUTS) $(WEB)/node_modules
	cd $(WEB) && npm run build

$(WEB_ASSETS): $(WEB_DIST)/index.html scripts/embed-ui.mjs
	node scripts/embed-ui.mjs

$(ELF): $(SOURCES) $(HEADERS) $(WEB_ASSETS) VERSION
	@mkdir -p $(DIST)
	$(CC) $(CFLAGS) -o $@ $(SOURCES) -lpthread

deploy: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

version:
	@echo $(VERSION)

clean:
	rm -rf $(DIST) $(WEB_DIST) $(WEB_ASSETS)

format:
	$(CLANG_FORMAT) -i $(SOURCES) $(HEADERS) test/host_stubs.c

host-test:
	./test/run_host_test.sh
