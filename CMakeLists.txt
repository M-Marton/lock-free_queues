cmake_minimum_required(VERSION 3.10)
project(MPMCBenchmark LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD 11)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LTTNG_UST lttng-ust)

if(LTTNG_UST_FOUND)
    add_definitions(-DLTTNG_ENABLED)
    set(EXTRA_LIBS ${LTTNG_UST_LIBRARIES})
    set(EXTRA_INC ${LTTNG_UST_INCLUDE_DIRS})
    message(STATUS "LTTng-UST found – tracing enabled")
else()
    message(WARNING "LTTng-UST not found – compile without tracing")
    set(EXTRA_LIBS "")
    set(EXTRA_INC "")
endif()

add_executable(benchmark
    benchmark.cpp
    benchmark_tp.c
)

target_include_directories(benchmark PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${EXTRA_INC})
target_link_libraries(benchmark pthread dl ${EXTRA_LIBS})
target_compile_options(benchmark PRIVATE -O3 -march=native)
