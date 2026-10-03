# Host (Mac/Linux) unit tests for the pure-C logic. Uses cJSON and Unity from ESP-IDF.
#   cmake -S tests/host -B build/host && cmake --build build/host && ctest --test-dir build/host --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(homeplanner_host_tests C)

set(CMAKE_C_STANDARD 11)
if(NOT DEFINED ENV{IDF_PATH})
  set(IDF_PATH "$ENV{HOME}/esp/esp-idf")
else()
  set(IDF_PATH "$ENV{IDF_PATH}")
endif()

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(CJSON ${IDF_PATH}/components/json/cJSON)
set(UNITY ${IDF_PATH}/components/unity/unity/src)

add_library(cjson STATIC ${CJSON}/cJSON.c)
target_include_directories(cjson PUBLIC ${CJSON})

add_library(unity STATIC ${UNITY}/unity.c)
target_include_directories(unity PUBLIC ${UNITY})

add_library(hp_logic STATIC ${ROOT}/components/logic/hp_logic.c ${ROOT}/components/logic/hp_edit.c)
target_include_directories(hp_logic PUBLIC ${ROOT}/components/logic/include)
target_link_libraries(hp_logic PUBLIC cjson)
target_compile_options(hp_logic PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_executable(test_logic test_logic.c)
target_link_libraries(test_logic hp_logic unity)
target_compile_options(test_logic PRIVATE -Wall -Wextra)
add_test(NAME logic COMMAND test_logic)

add_library(hp_tz STATIC ${ROOT}/components/tz/tz.c ${ROOT}/components/tz/tz_table.c)
target_include_directories(hp_tz PUBLIC ${ROOT}/components/tz/include)
target_compile_options(hp_tz PRIVATE -Wall -Wextra -Werror)

set(TZ_FIXTURE ${CMAKE_CURRENT_LIST_DIR}/fixtures/tz_fixture.h)
if(NOT EXISTS ${TZ_FIXTURE})  # reference values from Python's zoneinfo
  execute_process(COMMAND python3 ${ROOT}/tools/gen_tz_fixture.py)
endif()
add_executable(test_tz test_tz.c)
target_include_directories(test_tz PRIVATE ${CMAKE_CURRENT_LIST_DIR}/fixtures)
target_link_libraries(test_tz hp_tz unity)
add_test(NAME tz COMMAND test_tz)

add_library(hp_settings STATIC ${ROOT}/components/settings/settings_json.c)
target_include_directories(hp_settings PUBLIC ${ROOT}/components/settings/include)
target_link_libraries(hp_settings PUBLIC hp_logic hp_tz cjson)
target_compile_options(hp_settings PRIVATE -Wall -Wextra -Werror)

add_executable(test_settings test_settings.c)
target_link_libraries(test_settings hp_settings unity)
add_test(NAME settings COMMAND test_settings)

add_library(hp_weather STATIC ${ROOT}/components/weather/weather_parse.c)
target_include_directories(hp_weather PUBLIC ${ROOT}/components/weather/include ${CMAKE_CURRENT_LIST_DIR}/stubs)
target_link_libraries(hp_weather PUBLIC cjson)
target_compile_options(hp_weather PRIVATE -Wall -Wextra -Werror)

add_executable(test_weather test_weather.c)
target_link_libraries(test_weather hp_weather unity)
add_test(NAME weather COMMAND test_weather)

# --- Sign in with Google (OAuth + PKCE): built against ESP-IDF's own mbedTLS ---
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
add_subdirectory(${IDF_PATH}/components/mbedtls/mbedtls ${CMAKE_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)

add_library(hp_oauth STATIC ${ROOT}/components/gauth/oauth.c)
target_include_directories(hp_oauth PUBLIC ${ROOT}/components/gauth/include)
target_link_libraries(hp_oauth PUBLIC cjson mbedcrypto)
target_compile_options(hp_oauth PRIVATE -Wall -Wextra -Werror)

add_executable(test_oauth test_oauth.c)
target_link_libraries(test_oauth hp_oauth unity)
add_test(NAME oauth COMMAND test_oauth)

add_executable(test_edit test_edit.c)
target_link_libraries(test_edit hp_logic unity)
add_test(NAME edit COMMAND test_edit)

# --- Other calendars (iCal): checked against the Pi version's own expansion ---
add_library(hp_ical STATIC ${ROOT}/components/ical/ical_parse.c ${ROOT}/components/ical/ical_expand.c)
target_include_directories(hp_ical PUBLIC ${ROOT}/components/ical/include PRIVATE ${ROOT}/components/ical)
target_link_libraries(hp_ical PUBLIC hp_logic hp_tz)
target_compile_options(hp_ical PRIVATE -Wall -Wextra -Werror)

set(ICAL_FIXTURE ${CMAKE_CURRENT_LIST_DIR}/fixtures/ical_fixture.h)
set(PI_PYTHON ${ROOT}/../homeplanner/.venv/bin/python)
if(NOT EXISTS ${ICAL_FIXTURE} AND EXISTS ${PI_PYTHON})  # needs the Pi repo next to this one
  execute_process(COMMAND ${PI_PYTHON} ${ROOT}/tools/gen_ical_fixture.py)
endif()
if(EXISTS ${ICAL_FIXTURE})
  add_executable(test_ical test_ical.c)
  target_include_directories(test_ical PRIVATE ${CMAKE_CURRENT_LIST_DIR}/fixtures)
  target_compile_definitions(test_ical PRIVATE ICAL_DIR="${CMAKE_CURRENT_LIST_DIR}/ical")
  target_link_libraries(test_ical hp_ical unity)
  add_test(NAME ical COMMAND test_ical)
else()
  message(WARNING "test_ical skipped: generate fixtures/ical_fixture.h with tools/gen_ical_fixture.py (needs the Pi repo's .venv)")
endif()

# --- Settings page sign-in (one-time code, session cookies) ---
add_library(hp_web_auth STATIC ${ROOT}/components/web/web_auth.c)
target_include_directories(hp_web_auth PUBLIC ${ROOT}/components/web/include)
target_link_libraries(hp_web_auth PUBLIC mbedcrypto)
target_compile_options(hp_web_auth PRIVATE -Wall -Wextra -Werror)

add_executable(test_web_auth test_web_auth.c)
target_compile_definitions(test_web_auth PRIVATE _GNU_SOURCE)
target_link_libraries(test_web_auth hp_web_auth unity)
add_test(NAME web_auth COMMAND test_web_auth)

# --- The sign-in relay page (docs/relay.js), when node is installed ---
find_program(NODE node)
if(NODE)
  add_test(NAME relay_page COMMAND ${NODE} ${ROOT}/tests/web/relay_test.js)
endif()
