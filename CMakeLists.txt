cmake_minimum_required(VERSION 3.21)

project(kabu_micro_edge_c VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(Boost REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

set(KABU_BOOST_HEADERS_TARGET "")
if(TARGET Boost::headers)
  set(KABU_BOOST_HEADERS_TARGET Boost::headers)
elseif(TARGET Boost::boost)
  set(KABU_BOOST_HEADERS_TARGET Boost::boost)
endif()

add_library(kabu_micro_edge_core INTERFACE)
target_include_directories(kabu_micro_edge_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(
  kabu_micro_edge_core
  INTERFACE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)
if(KABU_BOOST_HEADERS_TARGET)
  target_link_libraries(kabu_micro_edge_core INTERFACE ${KABU_BOOST_HEADERS_TARGET})
endif()

add_executable(kabu_micro_edge src/main.cpp)
target_link_libraries(kabu_micro_edge PRIVATE kabu_micro_edge_core)

add_executable(
  kabu_micro_edge_tests
  tests/test_config.cpp
  tests/test_execution.cpp
  tests/test_gateway.cpp
  tests/test_journal.cpp
  tests/test_latency_report.cpp
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
gtest_discover_tests(kabu_micro_edge_tests)
