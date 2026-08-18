# somfy-remote — thin wrapper over PlatformIO.
#
# The targets exist so the safe form of each command is the short one. In particular there
# is no `monitor` target that shells out to `pio device monitor`: that needs a TTY on stdin
# and leaves an orphan holding the port when it does not have one (platformio-core#5113).
# `make log` runs tools/serial_log.py instead, which always exits.

.DEFAULT_GOAL := help
SHELL := /bin/bash

PORT ?=                                   # override: make upload PORT=/dev/cu.usbserial-110
SECONDS ?= 8
PIOFLAGS := $(if $(PORT),--upload-port $(PORT),)

.PHONY: help build test upload clean compiledb ports log bootlog run check ota radio

help:  ## Show this help
	@echo "somfy-remote — make targets"
	@echo
	@# Greedy .* is deliberate: BSD sed has no non-greedy operator, and '## ' occurs once.
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) \
		| sed -E 's/^([a-z-]+):.*## /  \1\t/' \
		| expand -t 14
	@echo
	@echo "Variables:  PORT=/dev/cu.usbserial-N   SECONDS=<n>"

build:  ## Compile the firmware
	pio run

test:   ## Run the desktop unit tests
	pio test -e native

upload: ## Compile and flash over USB
	@lsof $${PORT:-/dev/cu.usbserial*} >/dev/null 2>&1 \
		&& { echo "error: something is holding the serial port:"; lsof $${PORT:-/dev/cu.usbserial*}; \
		     echo "stop it first — PlatformIO will not, and the upload will fail with Errno 35"; \
		     exit 1; } || true
	pio run -t upload $(PIOFLAGS)

radio:  ## Flash the standalone CC1101 self-test and read its result
	pio run -e d1_mini-radio -t upload $(PIOFLAGS)
	tools/serial_log.py $(if $(PORT),--port $(PORT),) --seconds 6

ota:    ## Flash over WiFi: make ota OTA_HOST=<hostname-or-ip>
	@test -n "$(OTA_HOST)" || { echo "error: set OTA_HOST=<hostname-or-ip>"; exit 1; }
	@test -f include/secrets.h || { echo "error: include/secrets.h missing"; exit 1; }
	OTA_HOST="$(OTA_HOST)" \
	OTA_PASSWORD="$$(sed -n 's/^#define OTA_PASSWORD[[:space:]]*"\(.*\)".*/\1/p' include/secrets.h)" \
	pio run -e d1_mini-ota -t upload

run: upload  ## Flash, then capture the boot banner — the usual edit/verify loop
	@$(MAKE) --no-print-directory log

log:    ## Capture serial at 115200 for SECONDS seconds (safe from a script)
	tools/serial_log.py $(if $(PORT),--port $(PORT),) --seconds $(SECONDS) --timestamp

bootlog: ## Capture the 74880-baud boot ROM banner — use when the board will not start
	tools/serial_log.py $(if $(PORT),--port $(PORT),) --boot --seconds 5

ports:  ## List candidate serial ports
	pio device list

compiledb: ## Regenerate compile_commands.json for clangd
	pio run -t compiledb

clean:  ## Remove build artefacts
	pio run -t clean

check:  ## RULE 0 scan: look for site-specific values in tracked files
	tools/check_secrets.sh
