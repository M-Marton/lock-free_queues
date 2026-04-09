#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER mpmc_benchmark

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "./benchmark_tp.h"

#if !defined(_BENCHMARK_TP_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _BENCHMARK_TP_H

#include <lttng/tracepoint.h>
#include <stdint.h>

TRACEPOINT_EVENT(
    mpmc_benchmark,
    queue_op,
    TP_ARGS(
        const char*, queue_name,
        const char*, operation,
        uint64_t, thread_id,
        uint64_t, latency_ns
    ),
    TP_FIELDS(
        ctf_string(queue_name, queue_name)
        ctf_string(operation, operation)
        ctf_integer(uint64_t, thread_id, thread_id)
        ctf_integer(uint64_t, latency_ns, latency_ns)
    )
)

#endif

#include <lttng/tracepoint-event.h>
