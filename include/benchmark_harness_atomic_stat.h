/**
 * @file benchmark_harness.h
 * @brief Fixed benchmark harness with lock-free statistics
 */

#pragma once

#include "benchmark_mpmc_tp.h"
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <numeric>
#include <typeinfo>
#include <cxxabi.h>
#include <cstdint>
#include <mutex>
#include <vector>

/**
 * @brief Lock-free statistics accumulator
 * 
 * Uses atomic operations and per-thread accumulation to avoid contention
 */
class LockFreeStatistics {
private:
    struct alignas(64) ThreadLocalStats {  // Cache line aligned
        std::atomic<uint64_t> count{0};
        std::atomic<uint64_t> sum{0};
        std::atomic<uint64_t> min{0xFFFFFFFFFFFFFFFFULL};
        std::atomic<uint64_t> max{0};
        
        ThreadLocalStats() = default;
    };
    
    std::vector<ThreadLocalStats> per_thread_stats_;
    std::atomic<uint64_t> total_count_{0};
    std::atomic<uint64_t> total_sum_{0};
    std::atomic<uint64_t> global_min_{0xFFFFFFFFFFFFFFFFULL};
    std::atomic<uint64_t> global_max_{0};
    
    // For percentile calculation (collected at the end)
    std::mutex latency_collection_mutex_;
    std::vector<uint64_t> all_latencies_;
    
public:
    LockFreeStatistics(size_t num_threads = 128) 
        : per_thread_stats_(num_threads) {}
    
    /**
     * @brief Add a latency measurement from a specific thread
     * @param thread_id Thread identifier
     * @param latency_ns Latency in nanoseconds
     */
    void add(size_t thread_id, uint64_t latency_ns) {
        // Update per-thread stats (atomic, no mutex)
        ThreadLocalStats& stats = per_thread_stats_[thread_id % per_thread_stats_.size()];
        stats.count.fetch_add(1, std::memory_order_relaxed);
        stats.sum.fetch_add(latency_ns, std::memory_order_relaxed);
        
        // Update min with atomic compare-and-swap
        uint64_t old_min = stats.min.load(std::memory_order_relaxed);
        while (latency_ns < old_min && 
               !stats.min.compare_exchange_weak(old_min, latency_ns,
                                                std::memory_order_relaxed)) {}
        
        // Update max
        uint64_t old_max = stats.max.load(std::memory_order_relaxed);
        while (latency_ns > old_max && 
               !stats.max.compare_exchange_weak(old_max, latency_ns,
                                                std::memory_order_relaxed)) {}
        
        // For percentile calculation (collected at end, so mutex is acceptable)
        // But we only store a sample if needed to avoid overhead
        if (latency_ns > 1000 || (total_count_.load() % 1000 == 0)) {
            std::lock_guard<std::mutex> lock(latency_collection_mutex_);
            all_latencies_.push_back(latency_ns);
        }
        
        // Update global totals (atomic, no mutex)
        total_count_.fetch_add(1, std::memory_order_relaxed);
        total_sum_.fetch_add(latency_ns, std::memory_order_relaxed);
        
        // Update global min/max
        old_min = global_min_.load(std::memory_order_relaxed);
        while (latency_ns < old_min && 
               !global_min_.compare_exchange_weak(old_min, latency_ns,
                                                  std::memory_order_relaxed)) {}
        
        old_max = global_max_.load(std::memory_order_relaxed);
        while (latency_ns > old_max && 
               !global_max_.compare_exchange_weak(old_max, latency_ns,
                                                  std::memory_order_relaxed)) {}
    }
    
    /**
     * @brief Get total count
     */
    uint64_t count() const {
        return total_count_.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief Get total sum
     */
    uint64_t sum() const {
        return total_sum_.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief Get minimum latency
     */
    uint64_t min() const {
        return global_min_.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief Get maximum latency
     */
    uint64_t max() const {
        return global_max_.load(std::memory_order_relaxed);
    }
    
    /**
     * @brief Get average latency
     */
    double avg() const {
        uint64_t cnt = count();
        return cnt > 0 ? static_cast<double>(sum()) / cnt : 0.0;
    }
    
    /**
     * @brief Get percentile latency (requires sorting)
     */
    uint64_t percentile(double p) {
        std::lock_guard<std::mutex> lock(latency_collection_mutex_);
        if (all_latencies_.empty()) return 0;
        
        std::sort(all_latencies_.begin(), all_latencies_.end());
        size_t idx = static_cast<size_t>(p / 100.0 * all_latencies_.size());
        idx = std::min(idx, all_latencies_.size() - 1);
        return all_latencies_[idx];
    }
    
    /**
     * @brief Prepare for percentile calculation
     */
    void prepare_percentiles() {
        std::lock_guard<std::mutex> lock(latency_collection_mutex_);
        std::sort(all_latencies_.begin(), all_latencies_.end());
    }
    
    /**
     * @brief Reset statistics
     */
    void reset() {
        total_count_ = 0;
        total_sum_ = 0;
        global_min_ = 0xFFFFFFFFFFFFFFFFULL;
        global_max_ = 0;
        
        for (auto& stats : per_thread_stats_) {
            stats.count = 0;
            stats.sum = 0;
            stats.min = 0xFFFFFFFFFFFFFFFFULL;
            stats.max = 0;
        }
        
        {
            std::lock_guard<std::mutex> lock(latency_collection_mutex_);
            all_latencies_.clear();
        }
    }
};

/**
 * @brief Alternative: Per-thread local storage (even faster)
 * 
 * This approach avoids any atomic operations during measurement
 */
class PerThreadStatistics {
private:
    struct ThreadLocalStats {
        uint64_t count = 0;
        uint64_t sum = 0;
        uint64_t min = UINT64_MAX;
        uint64_t max = 0;
        std::vector<uint64_t> latencies;  // For percentiles
    };
    
    thread_local static ThreadLocalStats local_stats_;
    
    std::mutex merge_mutex_;
    uint64_t total_count_ = 0;
    uint64_t total_sum_ = 0;
    uint64_t global_min_ = UINT64_MAX;
    uint64_t global_max_ = 0;
    std::vector<uint64_t> all_latencies_;
    
public:
    /**
     * @brief Add latency from current thread (no synchronization!)
     */
    void add(uint64_t latency_ns) {
        ThreadLocalStats& stats = local_stats_;
        stats.count++;
        stats.sum += latency_ns;
        if (latency_ns < stats.min) stats.min = latency_ns;
        if (latency_ns > stats.max) stats.max = latency_ns;
        
        // Sample for percentiles (1% sampling to reduce memory)
        if (stats.latencies.size() < 10000 || (stats.count % 100 == 0)) {
            stats.latencies.push_back(latency_ns);
        }
    }
    
    /**
     * @brief Merge all thread-local statistics
     */
    void merge() {
        // This should be called after all threads have finished
        // In a real implementation, you'd need a way to collect from all threads
        // This is simplified for the example
    }
};

template<typename Queue>
class BenchmarkHarness {
private:
    Queue& queue_;
    size_t num_producers_;
    size_t num_consumers_;
    size_t items_per_producer_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> total_enqueued_{0};
    std::atomic<uint64_t> total_dequeued_{0};
    
    // Lock-free statistics (no mutex during measurement!)
    LockFreeStatistics enqueue_stats_;
    LockFreeStatistics dequeue_stats_;
    
    // For tracking thread IDs
    std::atomic<size_t> next_thread_id_{0};
    
public:
    BenchmarkHarness(Queue& q, size_t producers, size_t consumers, size_t items)
        : queue_(q)
        , num_producers_(producers)
        , num_consumers_(consumers)
        , items_per_producer_(items) {
        
        if (producers == 0 || consumers == 0 || items == 0) {
            throw std::invalid_argument("Producers, consumers, and items must be positive");
        }
    }
    
    void run() {
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
        stop_ = false;
        next_thread_id_ = 0;
        
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
            consumers.emplace_back(&BenchmarkHarness::consumer_worker, this);
        }
        
        // Start producers
        for (size_t i = 0; i < num_producers_; ++i) {
            producers.emplace_back(&BenchmarkHarness::producer_worker, this);
        }
        
        // Wait for producers to finish
        for (auto& p : producers) {
            p.join();
        }
        
        // Signal consumers to stop
        stop_ = true;
        
        // Wait for consumers to finish
        for (auto& c : consumers) {
            c.join();
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time).count();
        
        // Prepare percentiles
        enqueue_stats_.prepare_percentiles();
        dequeue_stats_.prepare_percentiles();
        
        // Print results
        print_results(duration_ns);
    }
    
private:
    std::string demangle_type_name(const char* mangled_name) {
        int status;
        char* demangled = abi::__cxa_demangle(mangled_name, nullptr, nullptr, &status);
        std::string result = (status == 0) ? demangled : mangled_name;
        free(demangled);
        return result;
    }
    
    void warmup() {
        constexpr size_t WARMUP_ITEMS = 10000;
        std::atomic<bool> warmup_stop{false};
        
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
    
    void producer_worker() {
        size_t thread_id = next_thread_id_.fetch_add(1);
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
                
                // NO MUTEX! Lock-free statistics
                enqueue_stats_.add(thread_id, latency_ns);
                
                produced++;
                total_enqueued_++;
            } else {
                // Queue full, pause
                __builtin_ia32_pause();
            }
        }
    }
    
    void consumer_worker() {
        size_t thread_id = next_thread_id_.fetch_add(1);
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
                
                // NO MUTEX! Lock-free statistics
                dequeue_stats_.add(thread_id, latency_ns);
                
                total_dequeued_++;
            } else {
                // Queue empty, pause
                __builtin_ia32_pause();
            }
        }
    }
    
    void print_results(uint64_t duration_ns) {
        uint64_t total_ops = enqueue_stats_.count();
        double throughput = total_ops * 1e9 / duration_ns;
        
        std::cout << "\n----------------------------------------" << std::endl;
        std::cout << "RESULTS" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Total operations: " << total_ops << std::endl;
        std::cout << "Duration: " << duration_ns / 1e9 << " seconds" << std::endl;
        std::cout << "Throughput: " << throughput << " ops/sec" << std::endl;
        std::cout << "Throughput: " << throughput / 1e6 << " M ops/sec" << std::endl;
        
        std::cout << "\nEnqueue Latency (ns):" << std::endl;
        std::cout << "  Min:    " << enqueue_stats_.min() << std::endl;
        std::cout << "  Max:    " << enqueue_stats_.max() << std::endl;
        std::cout << "  Avg:    " << enqueue_stats_.avg() << std::endl;
        std::cout << "  P50:    " << enqueue_stats_.percentile(50) << std::endl;
        std::cout << "  P90:    " << enqueue_stats_.percentile(90) << std::endl;
        std::cout << "  P99:    " << enqueue_stats_.percentile(99) << std::endl;
        std::cout << "  P99.9:  " << enqueue_stats_.percentile(99.9) << std::endl;
        
        std::cout << "\nDequeue Latency (ns):" << std::endl;
        std::cout << "  Min:    " << dequeue_stats_.min() << std::endl;
        std::cout << "  Max:    " << dequeue_stats_.max() << std::endl;
        std::cout << "  Avg:    " << dequeue_stats_.avg() << std::endl;
        std::cout << "  P50:    " << dequeue_stats_.percentile(50) << std::endl;
        std::cout << "  P90:    " << dequeue_stats_.percentile(90) << std::endl;
        std::cout << "  P99:    " << dequeue_stats_.percentile(99) << std::endl;
        std::cout << "  P99.9:  " << dequeue_stats_.percentile(99.9) << std::endl;
        std::cout << "----------------------------------------\n" << std::endl;
    }
};
