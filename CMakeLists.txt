# HomePlanner firmware for the Elecrow CrowPanel Advance ESP32-P4 10.1" (ESP-IDF).
cmake_minimum_required(VERSION 3.16)

# The Google sign-in client (ID/secret) lives in secrets.defaults, which is not in git.
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.defaults")
  list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/secrets.defaults")
endif()
# Signed firmware (over-the-air updates only accept firmware signed with the same key).
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/signing_key.pem")
  list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.signing")
endif()

# Demo firmware (made-up family, for screenshots): tools/demo.sh builds it with -D HP_DEMO=1.
if(HP_DEMO)
  list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.demo")
  file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" HP_VERSION LIMIT_COUNT 1)
  set(PROJECT_VER "${HP_VERSION}-demo")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(homeplanner)
