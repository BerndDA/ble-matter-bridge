#
# EnOcean-to-Matter-over-Thread gateway.
# Based on the nRF Connect SDK Matter Bridge application
# (Copyright (c) 2023 Nordic Semiconductor ASA, LicenseRef-Nordic-5-Clause).
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})

project(enocean-matter-gateway)

# Bridge core, Matter identity, network log: firmware/common
include(${CMAKE_CURRENT_SOURCE_DIR}/../common/common.cmake)

target_include_directories(app PRIVATE
  src
  src/enocean
)

target_sources(app PRIVATE
  src/app_task.cpp
  src/main.cpp
  ${GATEWAY_BRIDGED_DEVICE_TYPES}/onoff_light.cpp
  src/enocean/esp3.c
  src/enocean/eep.c
  src/enocean/tcm.c
  src/enocean/enocean_manager.cpp
  src/enocean/enocean_shell.cpp
  src/gateway_shell.cpp
)

# Status page (firmware/common/web): src/web/index.html and its JSON
target_sources_ifdef(CONFIG_GATEWAY_WEB app PRIVATE src/web_status.cpp)

ncs_configure_data_model()
