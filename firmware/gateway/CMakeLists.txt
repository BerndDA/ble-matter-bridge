#
# BLE-to-Matter-over-Thread gateway.
# Based on the nRF Connect SDK Matter Bridge application
# (Copyright (c) 2023 Nordic Semiconductor ASA, LicenseRef-Nordic-5-Clause).
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})

project(ble-matter-gateway)

# The port to the nRF52840 (board files, MCUboot layout) is validated against one SDK version only.
set(GATEWAY_NCS_VERSION "3.4.1")
file(STRINGS ${ZEPHYR_NRF_MODULE_DIR}/VERSION _ncs_version_lines REGEX "^(VERSION_MAJOR|VERSION_MINOR|PATCHLEVEL) ")
foreach(_line ${_ncs_version_lines})
  string(REGEX REPLACE "^([A-Z_]+) = ([0-9]+)$" "\\1;\\2" _kv "${_line}")
  list(GET _kv 0 _key)
  list(GET _kv 1 _val)
  set(_ncs_${_key} ${_val})
endforeach()
set(_ncs_version "${_ncs_VERSION_MAJOR}.${_ncs_VERSION_MINOR}.${_ncs_PATCHLEVEL}")
if(NOT _ncs_version STREQUAL GATEWAY_NCS_VERSION)
  message(FATAL_ERROR "This application is pinned to nRF Connect SDK v${GATEWAY_NCS_VERSION}, "
                      "but is being built with v${_ncs_version}.")
endif()

# Enable GNU STD support.
include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)

# Include all source files that are located in the Matter common directory.
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/source_common.cmake)

# Include Data Model utils
include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/data_model.cmake)

include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/zap_helpers.cmake)
ncs_get_zap_parent_dir(ZAP_PARENT_DIR)

target_include_directories(app PRIVATE
  src
  src/bridged_device_types
  ${ZAP_PARENT_DIR}
  src/core/util
  src/core
  src/ble
  src/ble/data_providers
)

target_sources(app PRIVATE
  src/app_task.cpp
  src/main.cpp
  src/bridge_shell.cpp
  src/zcl_callbacks.cpp
  src/core/bridge_manager.cpp
  src/core/matter_bridged_device.cpp
  src/core/bridge_storage_manager.cpp
  src/core/bridged_device_data_provider.cpp
  src/ble/ble_connectivity_manager.cpp
  src/ble/ble_bridged_device_factory.cpp
  src/bridged_device_types/temperature_sensor.cpp
  src/bridged_device_types/humidity_sensor.cpp
  src/ble/data_providers/ble_environmental_data_provider.cpp
  ${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/src/binding/binding_handler.cpp
)

ncs_configure_data_model()
