# Shortcuts for building, flashing and testing the gateway. `make` lists the targets.
# Every target sets up the SDK itself (scripts/ncs-env.sh); no need to source it first.

SHELL := /bin/bash
.DEFAULT_GOAL := help

APP      := firmware/gateway
BOARD    := nrf52840dk/nrf52840
BUILD    := $(APP)/build
DIAG     := $(APP)/build-diag
RELEASE  := $(APP)/build-release
PORT     ?= $(firstword $(wildcard /dev/cu.usbmodem000683*))

# Measurement settings (make measure MINUTES=20 THERMOSTAT=00:1A:22:0F:3F:02)
MINUTES    ?= 20
INTERVAL   ?= 60
THERMOSTAT ?=
TARGETS    ?= 18.5,18

NCS := source scripts/ncs-env.sh >/dev/null &&

.PHONY: help build flash flash-erase release diag diag-flash test zap term measure summary clean

help: ## List the targets
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  make %-12s %s\n", $$1, $$2}'

build: ## Build the gateway (incremental)
	@$(NCS) west build -b $(BOARD) --sysbuild -d $(BUILD) $(APP)

flash: build ## Build and flash; keeps commissioning and stored devices
	@$(NCS) west flash -d $(BUILD)

flash-erase: build ## Build and flash with a full erase; the gateway must be commissioned again
	@$(NCS) west flash -d $(BUILD) --erase

release: ## Build with prj_release.conf (no logging, watchdog, reset on fatal error) into build-release
	@$(NCS) west build -b $(BOARD) --sysbuild -d $(RELEASE) $(APP) -- -DFILE_SUFFIX=release

diag: ## Build the diagnostics firmware (stack, heap, thread statistics) into build-diag
	@$(NCS) west build -b $(BOARD) --sysbuild -d $(DIAG) $(APP) -- -DEXTRA_CONF_FILE=diag.conf

diag-flash: diag ## Build and flash the diagnostics firmware
	@$(NCS) west flash -d $(DIAG)

test: ## Run the host unit tests (BTHome parser)
	@$(MAKE) --no-print-directory -C $(APP)/tests/host/bthome

zap: ## Regenerate the Matter data model code after editing bridge.zap
	@$(NCS) cd $(APP) && west zap-generate -z src/default_zap/bridge.zap

term: ## Open the gateway shell (115200 baud; Ctrl+] quits). Close nRF Terminal first
	@test -n "$(PORT)" || { echo "No DK serial port found; set PORT=/dev/cu.usbmodem..."; exit 1; }
	@$(NCS) python3 -m serial.tools.miniterm --raw --eol CR $(PORT) 115200

measure: ## Capture diagnostics (needs `make diag-flash`); MINUTES, INTERVAL, THERMOSTAT, TARGETS
	@$(NCS) python3 scripts/diag-capture.py --minutes $(MINUTES) --interval $(INTERVAL) \
		$(if $(THERMOSTAT),--thermostat $(THERMOSTAT) --targets $(TARGETS))

summary: ## Summarize the newest capture in measurements/
	@python3 scripts/diag-summary.py $$(ls -t measurements/diag-*.log | head -1)

clean: ## Delete the gateway build folders (build, build-diag, build-release, test build)
	rm -rf $(BUILD) $(DIAG) $(RELEASE) $(APP)/tests/host/bthome/build
