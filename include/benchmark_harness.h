/**
 * @file benchmark_harness.h
 * @brief Benchmark harness for MPMC queue implementations with LTTng tracing
 * @author MPMC Benchmark Project
 * @version 1.0.0
 * 
 * @defgroup benchmark Benchmark Framework
 * @brief Unified testing framework for MPMC queue implementations
 * 
 * This harness provides a consistent testing environment for comparing different
 * MPMC queue implementations. It includes configurable thread counts, warm-up
 * phases, and integrated LTTng tracepoints for performance analysis.
 * 
 * @section methodology Benchmark Methodology
 * 1. **Warm-up phase**: 1 second to stabilize CPU caches and branch predictors
 * 2. **Measurement phase**: All producers enqueue items_per_producer elements
 * 3. **Metrics collected**:
 *    - Total throughput (ops/sec)
 *    - Per-operation latency distribution (min, max, average, p50, p99, p99.9)
 *    - CPU utilization via system counters
 * 4. **Tracepoints**: Record every operation for detailed analysis
 * 
 * @section usage Usage Example
 * @code
 * #include "benchmark_harness.h"
 * #include "queues/ring_buffer_queue.h"
 * 
 * RingBufferMPMC<uint64_t> queue(1024);
 * BenchmarkHarness<RingBufferMPMC<uint64_t>> harness(queue, 4, 4, 1000000);
 * harness.run();
 * @endcode
 * 
 * @see https://lttng.org/ LTTng Tracing
 */

#pragma once

#include "benchmark_mpmc_tp.h"
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <numeric>
#include <typeinfo>
#include <cxxabi.h>

/**
 * @brief Benchmark harness for MPMC queue implementations
 * 
 * This class manages the complete benchmarking process including thread
 * management, timing measurement, and statistics collection.
 * 
 * @tparam Queue The queue type to benchmark. Must provide:
 *               - `bool enqueue(const T&)` or `bool enqueue(T&&)`
 *               - `bool dequeue(T&)`
 * 
 * @invariant The queue must be thread-safe for MPMC operations
 * @invariant num_producers_ > 0 && num_consumers_ > 0
 * @invariant items_per_producer_ > 0
 * 
 * @performance Designed for minimal overhead during measurement
 */
template<typename Queue>
class BenchmarkHarness {
private:
    Queue& queue_;                      ///< Queue being benchmarked
    size_t num_producers_;              ///< Number of producer threads
    size_t num_consumers_;              ///< Number of consumer threads
    size_t items_per_producer_;         ///< Items each producer will enqueue
    
    std::atomic<bool> stop_{false};     ///< Signal for consumers to stop
    std::atomic<uint64_t> total_enqueued_{0};   ///< Total enqueue operations completed
    std::atomic<uint64_t> total_dequeued_{0};   ///< Total dequeue operations completed
    
    /**
     * @struct Statistics
     * @brief Performance statistics for a set of operations
     */
    struct Statistics {
        uint64_t count;         ///< Number of operations
        uint64_t min;           ///< Minimum latency (ns)
        uint64_t max;           ///< Maximum latency (ns)
        uint64_t sum;           ///< Sum of latencies (ns)
        std::vector<uint64_t> sorted_latencies;  ///< Sorted latencies for percentiles
        
        /**
         * @brief Add a latency measurement
         * @param latency_ns Latency in nanoseconds
         */
        void add(uint64_t latency_ns) {
            if (count == 0 || latency_ns < min) min = latency_ns;
            if (count == 0 || latency_ns > max) max = latency_ns;
            sum += latency_ns;
            sorted_latencies.push_back(latency_ns);
            count++;
        }
        
        /**
         * @brief Get average latency
         * @return Average latency in nanoseconds
         */
        double avg() const {
            return count > 0 ? static_cast<double>(sum) / count : 0.0;
        }
        
        /**
         * @brief Get percentile latency
         * @param percentile Value between 0 and 100
         * @return Latency at the given percentile
         */
        uint64_t percentile(double percentile) const {
            if (sorted_latencies.empty()) return 0;
            size_t idx = static_cast<size_t>(percentile / 100.0 * sorted_latencies.size());
            idx = std::min(idx, sorted_latencies.size() - 1);
            return sorted_latencies[idx];
        }
        
        /**
         * @brief Sort latencies for percentile calculation
         */
        void sort() {
            std::sort(sorted_latencies.begin(), sorted_latencies.end());
        }
        
        /**
         * @brief Reset statistics
         */
        void reset() {
            count = 0;
            min = 0;
            max = 0;
            sum = 0;
            sorted_latencies.clear();
        }
    };
    
    Statistics enqueue_stats_;          ///< Enqueue operation statistics
    Statistics dequeue_stats_;          ///< Dequeue operation statistics
    std::mutex stats_mutex_;            ///< Mutex for protecting statistics
    
    /**
     * @brief Demangle C++ type name for readable output
     * @param mangled_name The mangled type name from typeid()
     * @return Demangled human-readable type name
     */
    std::string demangle_type_name(const char* mangled_name) {
        int status;
        char* demangled = abi::__cxa_demangle(mangled_name, nullptr, nullptr, &status);
        std::string result = (status == 0) ? demangled : mangled_name;
        free(demangled);
        return result;
    }
    
public:
    /**
     * @brief Construct a benchmark harness
     * @param q Reference to the queue to benchmark
     * @param producers Number of producer threads
     * @param consumers Number of consumer threads
     * @param items Items each producer will enqueue
     * 
     * @pre producers > 0
     * @pre consumers > 0
     * @pre items > 0
     */
    BenchmarkHarness(Queue& q, size_t producers, size_t consumers, size_t items)
        : queue_(q)
        , num_producers_(producers)
        , num_consumers_(consumers)
        , items_per_producer_(items) {
        
        if (producers == 0 || consumers == 0 || items == 0) {
            throw std::invalid_argument("Producers, consumers, and items must be positive");
        }
    }
    
    /**
     * @brief Run the benchmark and collect metrics
     * 
     * This method executes the complete benchmark:
     * 1. Warm-up phase to stabilize system state
     * 2. Producer and consumer thread creation
     * 3. Measurement phase
     * 4. Statistics collection and reporting
     * 
     * @performance The measurement phase is timed with high-resolution clock
     * @thread_safety Must be called from a single thread
     * @return Statistics structure containing enqueue and dequeue metrics
     */
    Statistics run() {
        std::cout << "\n========================================" << std::endl;
        std::cout << "Benchmark: " << demangle_type_name(typeid(Queue).name()) << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "Producers: " << num_producers_ 
                  << " | Consumers: " << num_consumers_
                  << " | Items/producer: " << items_per_producer_ << std::endl;
        
        // Reset statistics
        enqueue_stats_.reset();
        dequeue_stats_.reset();
        total_enqueued_ = 0;
        total_dequeued_ = 0;
        
        // Warm-up phase
        std::cout << "Warming up..." << std::endl;
        warmup();
        
        // Create threads
        std::vector<std::thread> producers;
        std::vector<std::thread> consumers;
        
        std::cout << "Running benchmark..." << std::endl;
        auto start_time = std::chrono::high_resolution_clock::now();
        
        // Start consumers
        for (size_t i = 0; i < num_consumers_; ++i) {
            consumers.emplace_back(&BenchmarkHarness::consumer_worker, this, i);
        }
        
        // Start producers
        for (size_t i = 0; i < num_producers_; ++i) {
            producers.emplace_back(&BenchmarkHarness::producer_worker, this, i);
        }
        
        // Wait for producers to finish
        for (auto& p : producers) {
            p.join();
        }
        
        // Signal consumers to stop and wait
        stop_ = true;
        for (auto& c : consumers) {
            c.join();
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time).count();
        
        // Sort latencies for percentile calculation
        enqueue_stats_.sort();
        dequeue_stats_.sort();
        
        // Print results
        print_results(duration_ns);
        
        return enqueue_stats_;
    }
    
private:
    /**
     * @brief Warm-up phase to stabilize system state
     * 
     * Runs a short benchmark to warm up CPU caches, branch predictors,
     * and thread scheduling.
     */
    void warmup() {
        constexpr size_t WARMUP_ITEMS = 10000;
        std::atomic<bool> warmup_stop{false};
        
        // Simple warm-up: single producer, single consumer
        std::thread producer([this, &warmup_stop]() {
            size_t produced = 0;
            while (produced < WARMUP_ITEMS && !warmup_stop) {
                if (queue_.enqueue(produced)) {
                    produced++;
                }
            }
        });
        
        std::thread consumer([this, &warmup_stop]() {
            uint64_t item;
            size_t consumed = 0;
            while (consumed < WARMUP_ITEMS && !warmup_stop) {
                if (queue_.dequeue(item)) {
                    consumed++;
                }
            }
        });
        
        producer.join();
        consumer.join();
        
        // Clear the queue
        uint64_t item;
        while (queue_.dequeue(item)) {}
        
        std::cout << "Warm-up complete." << std::endl;
    }
    
    /**
     * @brief Producer worker thread function
     * @param thread_id Unique identifier for this producer
     * 
     * This function:
     * - Enqueues items_per_producer_ items
     * - Records LTTng tracepoints with per-operation latency
     * - Updates statistics when queue is not full
     * - Spins with pause instruction when queue is full
     */
    void producer_worker(size_t thread_id) {
        uint64_t produced = 0;
        
        while (produced < items_per_producer_) {
            auto before = std::chrono::high_resolution_clock::now();
            
            if (queue_.enqueue(produced)) {
                auto after = std::chrono::high_resolution_clock::now();
                auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    after - before).count();
                
                // LTTng tracepoint
                const char* queue_name = demangle_type_name(typeid(Queue).name()).c_str();
                tracepoint(benchmark_mpmc, queue_operation,
                          queue_name, "enqueue", 
                          thread_id, latency_ns);
                
                // Record statistics
                std::lock_guard<std::mutex> lock(stats_mutex_);
                enqueue_stats_.add(latency_ns);
                
                produced++;
                total_enqueued_++;
            } else {
                // Queue full, pause to reduce contention
                __builtin_ia32_pause();
            }
        }
    }
    
    /**
     * @brief Consumer worker thread function
     * @param thread_id Unique identifier for this consumer
     * 
     * This function:
     * - Dequeues items until stop_ is signaled
     * - Records LTTng tracepoints with per-operation latency
     * - Updates statistics when queue is not empty
     * - Spins with pause instruction when queue is empty
     */
    void consumer_worker(size_t thread_id) {
        uint64_t item;
        size_t total_items_expected = items_per_producer_ * num_producers_;
        
        while (!stop_.load() || total_dequeued_.load() < total_items_expected) {
            auto before = std::chrono::high_resolution_clock::now();
            
            if (queue_.dequeue(item)) {
                auto after = std::chrono::high_resolution_clock::now();
                auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    after - before).count();
                
                // LTTng tracepoint
                const char* queue_name = demangle_type_name(typeid(Queue).name()).c_str();
                tracepoint(benchmark_mpmc, queue_operation,
                          queue_name, "dequeue", 
                          thread_id, latency_ns);
                
                // Record statistics
                std::lock_guard<std::mutex> lock(stats_mutex_);
                dequeue_stats_.add(latency_ns);
                
                total_dequeued_++;
            } else {
                // Queue empty, pause to reduce contention
                __builtin_ia32_pause();
            }
        }
    }
    
    /**
     * @brief Print benchmark results
     * @param duration_ns Total benchmark duration in nanoseconds
     */
    void print_results(uint64_t duration_ns) {
        uint64_t total_ops = enqueue_stats_.count;
        double throughput = total_ops * 1e9 / duration_ns;
        
        std::cout << "\n----------------------------------------" << std::endl;
        std::cout << "RESULTS" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Total operations: " << total_ops << std::endl;
        std::cout << "Duration: " << duration_ns / 1e9 << " seconds" << std::endl;
        std::cout << "Throughput: " << throughput << " ops/sec" << std::endl;
        
        std::cout << "\nEnqueue Latency (ns):" << std::endl;
        std::cout << "  Min:    " << enqueue_stats_.min << std::endl;
        std::cout << "  Max:    " << enqueue_stats_.max << std::endl;
        std::cout << "  Avg:    " << enqueue_stats_.avg() << std::endl;
        std::cout << "  P50:    " << enqueue_stats_.percentile(50) << std::endl;
        std::cout << "  P99:    " << enqueue_stats_.percentile(99) << std::endl;
        std::cout << "  P99.9:  " << enqueue_stats_.percentile(99.9) << std::endl;
        
        std::cout << "\nDequeue Latency (ns):" << std::endl;
        std::cout << "  Min:    " << dequeue_stats_.min << std::endl;
        std::cout << "  Max:    " << dequeue_stats_.max << std::endl;
        std::cout << "  Avg:    " << dequeue_stats_.avg() << std::endl;
        std::cout << "  P50:    " << dequeue_stats_.percentile(50) << std::endl;
        std::cout << "  P99:    " << dequeue_stats_.percentile(99) << std::endl;
        std::cout << "  P99.9:  " << dequeue_stats_.percentile(99.9) << std::endl;
        std::cout << "----------------------------------------\n" << std::endl;
    }
};
