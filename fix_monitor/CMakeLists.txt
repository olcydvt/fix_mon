cmake_minimum_required(VERSION 3.19)
project(fixmon VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# SQLite::SQLite3 comes from the system, a local amalgamation, or a download.
# See cmake/ResolveSQLite3.cmake for the knobs.
include(ResolveSQLite3)
fixmon_resolve_sqlite3()

find_package(Threads REQUIRED)

add_library(fixmon_core STATIC
  src/event.cpp
  src/redact.cpp
  src/config.cpp
  src/quickfix_config.cpp
  src/fix_parser.cpp
  src/tail_reader.cpp
  src/message_log_adapter.cpp
  src/event_log_adapter.cpp
  src/session_state.cpp
  src/metrics.cpp
  src/event_store.cpp
  src/http_server.cpp
)
target_include_directories(fixmon_core PUBLIC include)
target_link_libraries(fixmon_core PUBLIC SQLite::SQLite3 Threads::Threads)

# Warnings apply to our code only; the resolver already silences the
# amalgamation on its own target.
if(MSVC)
  target_compile_options(fixmon_core PRIVATE /W4)
else()
  target_compile_options(fixmon_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  target_link_libraries(fixmon_core PUBLIC ws2_32)
endif()

add_executable(fixmon src/main.cpp)
target_link_libraries(fixmon PRIVATE fixmon_core)
target_compile_definitions(fixmon PRIVATE FIXMON_VERSION="${PROJECT_VERSION}")

add_executable(fixmon_selftest tests/selftest.cpp)
target_link_libraries(fixmon_selftest PRIVATE fixmon_core)

enable_testing()
add_test(NAME selftest COMMAND fixmon_selftest)

install(TARGETS fixmon RUNTIME DESTINATION bin)

message(STATUS "fixmon ${PROJECT_VERSION}  build=${CMAKE_BUILD_TYPE}  sqlite=${FIXMON_SQLITE_ORIGIN}")
