#ifndef BENCHMARK_MPMC_TP_H
#define BENCHMARK_MPMC_TP_H

#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER benchmark_mpmc

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "benchmark_mpmc_tp.h"

#include <lttng/tracepoint.h>
#include <stdint.h>

TRACEPOINT_EVENT(
    benchmark_mpmc,
    queue_operation,
    TP_ARGS(
        const char*, queue_type,
        const char*, operation,
        uint64_t, thread_id,
        uint64_t, latency_ns
    ),
    TP_FIELDS(
        ctf_string(queue_type, queue_type)
        ctf_string(operation, operation)
        ctf_integer(uint64_t, thread_id, thread_id)
        ctf_integer(uint64_t, latency_ns, latency_ns)
    )
)

#endif

#include <lttng/tracepoint-event.h>
