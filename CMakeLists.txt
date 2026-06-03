cmake_minimum_required(VERSION 3.16)
project(Chronos VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Release flags ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG -g -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -fno-omit-frame-pointer -march=native")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ── Compiler warnings ──────────────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter)

# ── Thread support ─────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── Google Benchmark (optional – gracefully skip if absent) ───────────────────
find_package(benchmark QUIET)
if(NOT benchmark_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.3
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
    set(benchmark_FOUND TRUE)
endif()

# ── Include directories ────────────────────────────────────────────────────────
include_directories(include)

# ── Core library (header-only logic compiled into a thin object lib) ──────────
add_library(chronos_core STATIC
    src/memory_arena.cpp
    src/order_book.cpp
    src/itch_parser.cpp
    src/spsc_logger.cpp
    src/dpdk_receiver.cpp
)

option(USE_DPDK "Build with DPDK kernel bypass networking support" OFF)
if(USE_DPDK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DPDK REQUIRED libdpdk)
    target_compile_definitions(chronos_core PUBLIC USE_DPDK)
    target_link_libraries(chronos_core PUBLIC ${DPDK_LIBRARIES})
    target_include_directories(chronos_core PUBLIC ${DPDK_INCLUDE_DIRS})
    message(STATUS "DPDK Support ENABLED")
else()
    message(STATUS "DPDK Support DISABLED (USE_DPDK=OFF)")
endif()
target_include_directories(chronos_core PUBLIC include)
target_link_libraries(chronos_core PUBLIC Threads::Threads)

# ── Main engine binary ─────────────────────────────────────────────────────────
add_executable(chronos_engine src/main.cpp)
target_link_libraries(chronos_engine PRIVATE chronos_core)

# ── ITCH file replay tool ──────────────────────────────────────────────────────
add_executable(itch_replay tools/itch_replay.cpp)
target_link_libraries(itch_replay PRIVATE chronos_core)

# ── Synthetic order generator ──────────────────────────────────────────────────
add_executable(gen_orders tools/gen_orders.cpp)
target_link_libraries(gen_orders PRIVATE chronos_core)

# ── Benchmarks ─────────────────────────────────────────────────────────────────
if(benchmark_FOUND)
  add_executable(bench_engine bench/bench_engine.cpp)
  target_link_libraries(bench_engine PRIVATE chronos_core benchmark::benchmark benchmark::benchmark_main)
  message(STATUS "Google Benchmark found – bench_engine will be built")
  
  add_executable(bench_latency bench/bench_engine.cpp)
  target_link_libraries(bench_latency PRIVATE chronos_core)
  target_compile_definitions(bench_latency PRIVATE CHRONOS_STANDALONE_BENCH)
  message(STATUS "Added bench_latency for standalone p50/p90/p99 percentiles")
else()
  # Fallback: standalone latency benchmark (no external deps)
  add_executable(bench_engine bench/bench_engine.cpp)
  target_link_libraries(bench_engine PRIVATE chronos_core)
  target_compile_definitions(bench_engine PRIVATE CHRONOS_STANDALONE_BENCH)
  message(STATUS "Google Benchmark NOT found – using built-in latency harness")
endif()

message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "CXX flags  : ${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE}}")

# ── Catch2 unit tests ──────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
)
FetchContent_MakeAvailable(Catch2)
add_executable(chronos_tests tests/test_matching_engine.cpp)
target_link_libraries(chronos_tests PRIVATE chronos_core Catch2::Catch2WithMain)
include(CTest)
include(Catch)
catch_discover_tests(chronos_tests)
message(STATUS "Catch2: target 'chronos_tests' enabled")
