cmake_minimum_required(VERSION 3.10)
project(MPMCBenchmark VERSION 2.0.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LTTNG_UST QUIET lttng-ust)

if(LTTNG_UST_FOUND)
    message(STATUS "LTTng-UST found - tracing enabled")
    add_definitions(-DLTTNG_ENABLED)
else()
    message(STATUS "LTTng-UST not found - tracing disabled")
    message(STATUS "Install with: sudo apt install lttng-tools lttng-ust liblttng-ust-dev")
endif()

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LTTNG_UST_INCLUDE_DIRS}
)

set(QUEUE_SOURCES
    src/benchmark_mpmc_provider.c
)

add_executable(benchmark src/benchmark_main.cpp ${QUEUE_SOURCES})

target_link_libraries(benchmark 
    ${LTTNG_UST_LIBRARIES}
    pthread
    dl
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(benchmark PRIVATE -O3 -march=native -mtune=native)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(benchmark PRIVATE -g -O0 -fsanitize=address)
    target_link_options(benchmark PRIVATE -fsanitize=address)
endif()

install(TARGETS benchmark DESTINATION bin)
