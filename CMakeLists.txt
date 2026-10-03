# HomePlanner firmware for the Elecrow CrowPanel Advance ESP32-P4 10.1" (ESP-IDF).
cmake_minimum_required(VERSION 3.16)

# The Google sign-in client (ID/secret) lives in secrets.defaults, which is not in git.
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.defaults")
  list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/secrets.defaults")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(homeplanner)
