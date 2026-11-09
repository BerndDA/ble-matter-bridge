#
# Shared parts of the gateway apps (firmware/*-gateway). An app includes this file after
# project() and before its own sources; it ends with ncs_configure_data_model().
# Based on the nRF Connect SDK Matter Bridge application
# (Copyright (c) 2023 Nordic Semiconductor ASA, LicenseRef-Nordic-5-Clause).
#

set(GATEWAY_COMMON_DIR ${CMAKE_CURRENT_LIST_DIR})

# The port to the nRF52840 (board files, MCUboot layout) is validated against one SDK version only.
set(GATEWAY_NCS_VERSION "3.4.2")
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
  ${ZAP_PARENT_DIR}
  ${GATEWAY_COMMON_DIR}
  ${GATEWAY_COMMON_DIR}/core
  ${GATEWAY_COMMON_DIR}/core/util
  ${GATEWAY_COMMON_DIR}/bridged_device_types
)

target_sources(app PRIVATE
  ${GATEWAY_COMMON_DIR}/core/bridge_manager.cpp
  ${GATEWAY_COMMON_DIR}/core/matter_bridged_device.cpp
  ${GATEWAY_COMMON_DIR}/core/bridge_storage_manager.cpp
  ${GATEWAY_COMMON_DIR}/core/bridged_device_data_provider.cpp
  ${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/src/binding/binding_handler.cpp
)

target_sources_ifdef(CONFIG_GATEWAY_IDENTITY_FROM_CHIP_ID app PRIVATE
  ${GATEWAY_COMMON_DIR}/identity/chip_id_identity.cpp)
target_sources_ifdef(CONFIG_GATEWAY_NET_LOG app PRIVATE ${GATEWAY_COMMON_DIR}/netlog/net_log.cpp)

# Matter device types: an app adds the ones it uses, for example
#   target_sources(app PRIVATE ${GATEWAY_BRIDGED_DEVICE_TYPES}/temperature_sensor.cpp)
set(GATEWAY_BRIDGED_DEVICE_TYPES ${GATEWAY_COMMON_DIR}/bridged_device_types)
