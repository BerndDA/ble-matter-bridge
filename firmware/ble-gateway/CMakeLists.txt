# BLE-to-Matter-over-Thread gateway.
# Based on the nRF Connect SDK Matter Bridge application
# (Copyright (c) 2023 Nordic Semiconductor ASA, LicenseRef-Nordic-5-Clause).
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})

project(ble-matter-gateway)

# Bridge core, Matter identity, network log: firmware/common
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/common.cmake)

target_include_directories(app PRIVATE
  src
  src/ble
  src/ble/data_providers
  src/sensors
  src/thermostat
)

target_sources(app PRIVATE
  src/app_task.cpp
  src/main.cpp
  src/bridge_shell.cpp
  src/zcl_callbacks.cpp
  src/ble/ble_connectivity_manager.cpp
  src/ble/ble_bridged_device_factory.cpp
  ${GATEWAY_BRIDGED_DEVICE_TYPES}/temperature_sensor.cpp
  ${GATEWAY_BRIDGED_DEVICE_TYPES}/humidity_sensor.cpp
  ${GATEWAY_BRIDGED_DEVICE_TYPES}/thermostat.cpp
  src/thermostat/eq3_client.cpp
  src/ble/data_providers/ble_environmental_data_provider.cpp
  src/sensors/bthome.c
  src/sensors/sensor_scanner.c
  src/sensors/sensor_shell.cpp
  src/sensors/bthome_sensor_provider.cpp
  src/sensors/bthome_sensor_manager.cpp
)

ncs_configure_data_model()
