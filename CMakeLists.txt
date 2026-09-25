cmake_minimum_required(VERSION 3.20)
project(stok VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(STOK_NATIVE "Tune code for the build machine's CPU (-march=native)" ON)
option(STOK_LTO "Link-time optimization" ON)
option(STOK_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(STOK_BUILD_TESTS "Build unit tests" ON)

find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wno-unused-parameter)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O3 -fno-plt -fno-semantic-interposition)
endif()
if(STOK_NATIVE)
  add_compile_options(-march=native)
endif()
if(STOK_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(STOK_LTO AND CMAKE_BUILD_TYPE STREQUAL "Release" AND NOT STOK_SANITIZE)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT ipo_ok OUTPUT ipo_msg)
  if(ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  endif()
endif()

add_library(stok_core STATIC
  src/core/config.cpp
  src/core/log.cpp
  src/core/thread.cpp
  src/util/file.cpp
  src/util/json.cpp
  src/util/text.cpp
  src/util/time.cpp
  src/net/connection.cpp
  src/net/http.cpp
  src/net/http_client.cpp
  src/net/resolver.cpp
  src/net/tls.cpp
  src/net/url.cpp
  src/ref/filings.cpp
  src/ref/symbols.cpp
  src/feeds/parsers.cpp
  src/feeds/poller.cpp
  src/feeds/tickers.cpp
  src/market/baseline.cpp
  src/market/board.cpp
  src/market/itch_book.cpp
  src/market/recorder.cpp
  src/market/runner.cpp
  src/market/sources.cpp
  src/engine/engine.cpp
  src/engine/paper.cpp
  src/engine/scorer.cpp
  src/research/backtest.cpp
  src/research/edgar_history.cpp
  src/research/news_loader.cpp
  src/research/report.cpp
  src/sink/alerts.cpp
  src/sink/journal.cpp
  src/app/settings.cpp
)
target_include_directories(stok_core PUBLIC src)
target_link_libraries(stok_core PUBLIC OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB Threads::Threads)

add_executable(stokd src/app/stokd.cpp)
target_link_libraries(stokd PRIVATE stok_core)

add_executable(stok-ref src/app/stok_ref.cpp)
target_link_libraries(stok-ref PRIVATE stok_core)

add_executable(stok-replay src/app/stok_replay.cpp)
target_link_libraries(stok-replay PRIVATE stok_core)

add_executable(stok-report src/app/stok_report.cpp)
target_link_libraries(stok-report PRIVATE stok_core)

add_executable(stok-backtest src/app/stok_backtest.cpp)
target_link_libraries(stok-backtest PRIVATE stok_core)

add_executable(stok-history src/app/stok_history.cpp)
target_link_libraries(stok-history PRIVATE stok_core)

add_executable(stok-bench bench/bench.cpp)
target_link_libraries(stok-bench PRIVATE stok_core)
target_compile_definitions(stok-bench PRIVATE STOK_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

if(STOK_BUILD_TESTS)
  enable_testing()
  file(GLOB STOK_TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
  add_executable(stok-tests ${STOK_TEST_SOURCES})
  target_link_libraries(stok-tests PRIVATE stok_core)
  target_compile_definitions(stok-tests PRIVATE STOK_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME unit COMMAND stok-tests WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

  add_executable(stok-it-http tests/integration/it_http.cpp)
  target_link_libraries(stok-it-http PRIVATE stok_core)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME http_integration
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/integration/run_http_test.py
                     $<TARGET_FILE:stok-it-http>
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endif()
endif()
