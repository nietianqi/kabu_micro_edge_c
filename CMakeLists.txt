cmake_minimum_required(VERSION 3.21)

project(kabu_micro_edge_c VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(KABU_ENABLE_FETCHCONTENT "Download dependencies when local packages are unavailable" ON)

if(MSVC)
  add_compile_options(/W4)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

include(CTest)

if(KABU_ENABLE_FETCHCONTENT)
  include(FetchContent)
endif()

find_package(nlohmann_json CONFIG QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
  if(NOT KABU_ENABLE_FETCHCONTENT)
    message(FATAL_ERROR "nlohmann_json not found. Enable KABU_ENABLE_FETCHCONTENT or provide the package.")
  endif()
  FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(nlohmann_json)
endif()

find_package(spdlog CONFIG QUIET)
if(NOT TARGET spdlog::spdlog AND NOT TARGET spdlog::spdlog_header_only)
  if(NOT KABU_ENABLE_FETCHCONTENT)
    message(FATAL_ERROR "spdlog not found. Enable KABU_ENABLE_FETCHCONTENT or provide the package.")
  endif()
  FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.15.3.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(spdlog)
endif()

set(KABU_SPDLOG_TARGET spdlog::spdlog)
if(TARGET spdlog::spdlog_header_only)
  set(KABU_SPDLOG_TARGET spdlog::spdlog_header_only)
elseif(NOT TARGET spdlog::spdlog)
  message(FATAL_ERROR "spdlog target was not created.")
endif()

add_library(kabu_micro_edge_core INTERFACE)
target_include_directories(kabu_micro_edge_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(
  kabu_micro_edge_core
  INTERFACE
    nlohmann_json::nlohmann_json
    ${KABU_SPDLOG_TARGET}
)

if(WIN32)
  target_link_libraries(kabu_micro_edge_core INTERFACE winhttp)
endif()

add_executable(kabu_micro_edge src/main.cpp)
target_link_libraries(kabu_micro_edge PRIVATE kabu_micro_edge_core)

if(BUILD_TESTING)
  find_package(GTest CONFIG QUIET)
  if(NOT TARGET GTest::gtest)
    if(NOT KABU_ENABLE_FETCHCONTENT)
      message(FATAL_ERROR "GTest not found. Enable KABU_ENABLE_FETCHCONTENT or provide the package.")
    endif()
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
      googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(
    kabu_micro_edge_tests
    tests/test_config.cpp
    tests/test_execution.cpp
    tests/test_gateway.cpp
    tests/test_journal.cpp
    tests/test_latency_report.cpp
    tests/test_live_rest_executor.cpp
    tests/test_oms.cpp
    tests/test_python_oracle.cpp
    tests/test_replay.cpp
    tests/test_risk.cpp
    tests/test_runtime.cpp
    tests/test_signals.cpp
    tests/test_strategy.cpp
  )
  target_link_libraries(kabu_micro_edge_tests PRIVATE kabu_micro_edge_core GTest::gtest GTest::gtest_main)
  target_compile_definitions(kabu_micro_edge_tests PRIVATE KABU_MICRO_EDGE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

  include(GoogleTest)
  gtest_discover_tests(kabu_micro_edge_tests DISCOVERY_MODE PRE_TEST)
endif()
