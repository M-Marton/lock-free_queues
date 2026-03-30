/**
 * @file benchmark_mpmc_provider.c
 * @brief LTTng-UST probe provider for MPMC benchmark tracepoints
 * @author MPMC Benchmark Project
 * @version 1.0.0
 * 
 * This file creates the actual tracepoint probes that are inserted into the
 * benchmark executable. It must be compiled and linked with the main benchmark.
 * 
 * @warning This file must be compiled as C, not C++.
 */

#define TRACEPOINT_CREATE_PROBES
#define TRACEPOINT_DEFINE

#include "benchmark_mpmc_tp.h"
