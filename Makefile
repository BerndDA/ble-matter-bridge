# Shortcuts for building, flashing and testing the gateway. `make` lists the targets.
# Every target sets up the SDK itself (scripts/ncs-env.sh); no need to source it first.
# Add BOARD=promicro or BOARD=promicro-uf2 to any target for the Pro Micro nRF52840.

SHELL := /bin/bash
.DEFAULT_GOAL := help

APP      := firmware/gateway

# Board:
#   dk            nRF52840 DK (default)
#   promicro      Pro Micro nRF52840, flashed over SWD through the DK's Debug Out header
#   promicro-uf2  Pro Micro nRF52840 with its factory UF2 bootloader, flashed over USB
# An exported BOARD from the shell is ignored.
ifeq ($(origin BOARD),environment)
BOARD    := dk
endif
BOARD    ?= dk
ifeq ($(BOARD),dk)
TARGET   := nrf52840dk/nrf52840
SUFFIX   :=
PORT     ?= $(firstword $(wildcard /dev/cu.usbmodem000683*))
else ifeq ($(BOARD),promicro)
TARGET   := promicro_nrf52840/nrf52840
SUFFIX   := -promicro
# USB CDC port of the Pro Micro: any usbmodem port that isn't the DK's J-Link
PORT     ?= $(firstword $(filter-out /dev/cu.usbmodem000683%,$(wildcard /dev/cu.usbmodem*)))
else ifeq ($(BOARD),promicro-uf2)
TARGET   := promicro_nrf52840/nrf52840/uf2
SUFFIX   := -promicro-uf2
UF2      := 1
PORT     ?= $(firstword $(filter-out /dev/cu.usbmodem000683%,$(wildcard /dev/cu.usbmodem*)))
else
$(error BOARD must be dk, promicro or promicro-uf2)
endif

# Flash a build folder: west flash, or for promicro-uf2 the .uf2 copied to the bootloader drive
# promicro-uf2: each board gets its own Matter identity, derived from its chip ID (see scripts/make-uf2.py)
ifdef UF2
# Without a UF2 drive, make-uf2.py first sends `gateway bootloader` on PORT.
flash_dir = $(NCS) python3 scripts/make-uf2.py -o $(1)/gateway.uf2 --deploy --identity $(1) \
	$(if $(PORT),--port $(PORT)) $(1)/gateway/zephyr/zephyr.hex
else
flash_dir = $(NCS) west flash -d $(1) $(2)
endif

BUILD    := $(APP)/build$(SUFFIX)
DIAG     := $(APP)/build-diag$(SUFFIX)
RELEASE  := $(APP)/build-release$(SUFFIX)

# Measurement settings (make measure MINUTES=20 THERMOSTAT=00:1A:22:0F:3F:02)
MINUTES    ?= 20
INTERVAL   ?= 60
THERMOSTAT ?=
TARGETS    ?= 18.5,18

NCS := source scripts/ncs-env.sh >/dev/null &&

# chip-tool, built by `make chip-tool` from the Matter SDK submodule (pinned release)
CHIP      := tools/connectedhomeip
CHIP_TOOL := $(CHIP)/out/darwin-arm64-chip-tool/chip-tool
# ZAP release matching the pin in $(CHIP)/scripts/setup/zap.json (bump both together)
CHIP_ZAP     := $(CHIP)/.environment/zap-arm64
CHIP_ZAP_VER := v2026.05.12-nightly

.PHONY: help build flash flash-erase release diag diag-flash test zap term measure summary identity chip-tool clean

help: ## List the targets
	@grep -E '^[a-z-]+:.*## ' $(MAKEFILE_LIST) | awk -F':.*## ' '{printf "  make %-12s %s\n", $$1, $$2}'

build: ## Build the gateway (incremental)
	@$(NCS) west build -b $(TARGET) --sysbuild -d $(BUILD) $(APP)

flash: build ## Build and flash; keeps commissioning and stored devices
	@$(call flash_dir,$(BUILD))

flash-erase: build ## Build and flash with a full erase; the gateway must be commissioned again
ifdef UF2
	@echo "BOARD=promicro-uf2 can't erase over UF2: the settings lie outside what the bootloader writes."
	@echo "Use 'make flash BOARD=promicro-uf2', then 'matter factoryreset' in the shell or hold the button 6 s."
	@exit 1
else
	@$(call flash_dir,$(BUILD),--erase)
endif

release: ## Build with prj_release.conf (no logging, watchdog, reset on fatal error) into build-release
	@$(NCS) west build -b $(TARGET) --sysbuild -d $(RELEASE) $(APP) -- -DFILE_SUFFIX=release

diag: ## Build the diagnostics firmware (stack, heap, thread statistics) into build-diag
	@$(NCS) west build -b $(TARGET) --sysbuild -d $(DIAG) $(APP) -- -DEXTRA_CONF_FILE=diag.conf

diag-flash: diag ## Build and flash the diagnostics firmware
	@$(call flash_dir,$(DIAG))

test: ## Run the host unit tests (BTHome parser)
	@$(MAKE) --no-print-directory -C $(APP)/tests/host/bthome

zap: ## Regenerate the Matter data model code after editing bridge.zap
	@$(NCS) cd $(APP) && west zap-generate -z src/default_zap/bridge.zap

term: ## Open the gateway shell (115200 baud; Ctrl+] quits). Close nRF Terminal first
	@test -n "$(PORT)" || { echo "No serial port found for BOARD=$(BOARD); set PORT=/dev/cu.usbmodem..."; exit 1; }
	@$(NCS) python3 -m serial.tools.miniterm --raw --eol CR $(PORT) 115200

identity: ## promicro-uf2: show the setup code of the board on the UF2 drive (or of CHIP_ID=<16 hex digits>)
ifndef UF2
	@echo "Only for BOARD=promicro-uf2; the DK's identity is set in firmware/gateway/boards/nrf52840dk_nrf52840.conf."
	@exit 1
else
	@$(NCS) python3 scripts/make-uf2.py --identity $(BUILD) --show $(if $(CHIP_ID),--chip-id $(CHIP_ID))
endif

chip-tool: ## Build chip-tool (Matter controller for the Mac) from tools/connectedhomeip; first run takes long
	@git submodule update --init tools/connectedhomeip
	@cd $(CHIP) && python3 scripts/checkout_submodules.py --shallow --platform darwin
	@test -f $(CHIP)/.environment/activate.sh || { cd $(CHIP) && bash -c 'source scripts/bootstrap.sh -p all,darwin >/dev/null'; }
	@# The bootstrap fetches an x64 zap-cli, which needs Rosetta; use the native arm64 build of the same version
	@test -x $(CHIP_ZAP)/zap-cli || { mkdir -p $(CHIP_ZAP) && \
		gh release download $(CHIP_ZAP_VER) -R project-chip/zap -p zap-mac-arm64.zip -D $(CHIP_ZAP) && \
		unzip -q -o $(CHIP_ZAP)/zap-mac-arm64.zip -d $(CHIP_ZAP) && rm $(CHIP_ZAP)/zap-mac-arm64.zip; }
	@cd $(CHIP) && bash -c 'source scripts/activate.sh >/dev/null && ZAP_INSTALL_PATH=$(abspath $(CHIP_ZAP)) \
		./scripts/build/build_examples.py --target darwin-arm64-chip-tool build'
	@echo "chip-tool: $(CHIP_TOOL)"

measure: ## Capture diagnostics (needs `make diag-flash`); MINUTES, INTERVAL, THERMOSTAT, TARGETS
	@$(NCS) python3 scripts/diag-capture.py --minutes $(MINUTES) --interval $(INTERVAL) \
		$(if $(THERMOSTAT),--thermostat $(THERMOSTAT) --targets $(TARGETS))

summary: ## Summarize the newest capture in measurements/
	@python3 scripts/diag-summary.py $$(ls -t measurements/diag-*.log | head -1)

clean: ## Delete the build folders of BOARD (build, build-diag, build-release) and the test build
	rm -rf $(BUILD) $(DIAG) $(RELEASE) $(APP)/tests/host/bthome/build
