/**
 * @file benchmark_mpmc_tp.h
 * @brief LTTng-UST tracepoint definitions for MPMC queue benchmarking
 * @author MPMC Benchmark Project
 * @version 1.0.0
 * 
 * @defgroup tracepoints LTTng Tracepoints
 * @brief Performance tracing infrastructure
 * 
 * This file defines tracepoints used to instrument the benchmark harness.
 * Tracepoints capture per-operation latency for enqueue and dequeue operations,
 * enabling detailed performance analysis with Trace Compass and other tools.
 * 
 * @section usage Usage Example
 * @code
 * // Record an enqueue operation
 * auto start = std::chrono::high_resolution_clock::now();
 * queue.enqueue(item);
 * auto end = std::chrono::high_resolution_clock::now();
 * auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
 * 
 * tracepoint(benchmark_mpmc, queue_operation, "RingBuffer", "enqueue", thread_id, latency);
 * @endcode
 * 
 * @see https://lttng.org/docs/ LTTng Documentation
 * @see https://www.eclipse.org/tracecompass/ Trace Compass Documentation
 */

#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER benchmark_mpmc

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "benchmark_mpmc_tp.h"

#if !defined(_BENCHMARK_MPMC_TP_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _BENCHMARK_MPMC_TP_H

#include <lttng/tracepoint.h>
#include <stdint.h>

/**
 * @brief Tracepoint for queue operations
 * 
 * This tracepoint records detailed timing information for each queue operation,
 * enabling post-mortem analysis of latency distribution, contention patterns,
 * and thread behavior.
 * 
 * @param queue_type Name of the queue implementation being tested
 *                   (e.g., "MutexMPMCQueue", "TwoLockMPMCQueue", "RingBufferMPMC")
 * @param operation Operation type ("enqueue" or "dequeue")
 * @param thread_id Thread identifier executing the operation
 * @param latency_ns Operation latency in nanoseconds
 * 
 * @performance The tracepoint overhead is minimal (sub-microsecond) and
 *              includes a timestamp capture and string recording.
 * 
 * @sa BenchmarkHarness::producer_worker
 * @sa BenchmarkHarness::consumer_worker
 */
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

/**
 * @brief Tracepoint for batch operations
 * 
 * Optional tracepoint for recording batch statistics to reduce tracing overhead
 * when measuring extremely high throughput scenarios.
 */
TRACEPOINT_EVENT(
    benchmark_mpmc,
    batch_stats,
    TP_ARGS(
        const char*, queue_type,
        const char*, operation,
        uint64_t, thread_id,
        uint64_t, count,
        uint64_t, total_latency_ns
    ),
    TP_FIELDS(
        ctf_string(queue_type, queue_type)
        ctf_string(operation, operation)
        ctf_integer(uint64_t, thread_id, thread_id)
        ctf_integer(uint64_t, count, count)
        ctf_integer(uint64_t, total_latency_ns, total_latency_ns)
    )
)

#endif /* _BENCHMARK_MPMC_TP_H */

#include <lttng/tracepoint-event.h>
