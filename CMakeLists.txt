cmake_minimum_required(VERSION 3.10)
project(MPMCBenchmark VERSION 1.0.0 DESCRIPTION "MPMC Queue Benchmarking with LTTng")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Find LTTng-UST
find_package(PkgConfig REQUIRED)
pkg_check_modules(LTTNG_UST REQUIRED lttng-ust)

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${LTTNG_UST_INCLUDE_DIRS}
)

# Queue implementations
set(QUEUE_SOURCES
    src/queues/mutex_queue.cpp
    src/queues/two_lock_queue.cpp
    src/queues/ring_buffer_queue.cpp
)

# Benchmark executable
set(BENCHMARK_SOURCES
    src/benchmark_main.cpp
    src/benchmark_mpmc_provider.c
)

add_executable(benchmark ${BENCHMARK_SOURCES} ${QUEUE_SOURCES})

# Link libraries
target_link_libraries(benchmark 
    ${LTTNG_UST_LIBRARIES}
    pthread
    dl
    stdc++fs
)

# Compiler optimizations
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(benchmark PRIVATE -O3 -march=native -mtune=native)
    target_compile_options(benchmark PRIVATE -funroll-loops)
endif()

# Debug symbols in debug build
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(benchmark PRIVATE -g -O0)
endif()

# Doxygen documentation
find_package(Doxygen)
if(DOXYGEN_FOUND)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
endif()

# Install targets
install(TARGETS benchmark DESTINATION bin)
install(DIRECTORY include/ DESTINATION include)
