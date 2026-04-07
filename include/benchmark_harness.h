#ifndef BENCHMARK_HARNESS_H
#define BENCHMARK_HARNESS_H

#include "benchmark_mpmc_tp.h"
#include <chrono>
#include <thread>
#include <vector>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <typeinfo>
#include <cxxabi.h>
#include <cstdint>
#include <mutex>

class LockFreeStatistics {
private:
    struct alignas(64) ThreadLocalStats {
        std::atomic<uint64_t> count{0};
        std::atomic<uint64_t> sum{0};
        std::atomic<uint64_t> min{UINT64_MAX};
        std::atomic<uint64_t> max{0};
        
        void reset() {
            count.store(0, std::memory_order_relaxed);
            sum.store(0, std::memory_order_relaxed);
            min.store(UINT64_MAX, std::memory_order_relaxed);
            max.store(0, std::memory_order_relaxed);
        }
    };
    
    std::vector<ThreadLocalStats> per_thread_stats_;
    std::atomic<uint64_t> total_count_{0};
    std::atomic<uint64_t> total_sum_{0};
    std::atomic<uint64_t> global_min_{UINT64_MAX};
    std::atomic<uint64_t> global_max_{0};
    
    std::mutex sample_mutex_;
    std::vector<uint64_t> latency_samples_;
    static constexpr size_t SAMPLE_RATE = 100;
    
public:
    explicit LockFreeStatistics(size_t max_threads = 256) 
        : per_thread_stats_(max_threads) {}
    
    void add(size_t thread_id, uint64_t latency_ns) {
        size_t idx = thread_id % per_thread_stats_.size();
        ThreadLocalStats& stats = per_thread_stats_[idx];
        
        stats.count.fetch_add(1, std::memory_order_relaxed);
        stats.sum.fetch_add(latency_ns, std::memory_order_relaxed);
        
        uint64_t old_min = stats.min.load(std::memory_order_relaxed);
        while (latency_ns < old_min && 
               !stats.min.compare_exchange_weak(old_min, latency_ns,
                                                std::memory_order_relaxed)) {}
        
        uint64_t old_max = stats.max.load(std::memory_order_relaxed);
        while (latency_ns > old_max && 
               !stats.max.compare_exchange_weak(old_max, latency_ns,
                                                std::memory_order_relaxed)) {}
        
        total_count_.fetch_add(1, std::memory_order_relaxed);
        total_sum_.fetch_add(latency_ns, std::memory_order_relaxed);
        
        old_min = global_min_.load(std::memory_order_relaxed);
        while (latency_ns < old_min && 
               !global_min_.compare_exchange_weak(old_min, latency_ns,
                                                  std::memory_order_relaxed)) {}
        
        old_max = global_max_.load(std::memory_order_relaxed);
        while (latency_ns > old_max && 
               !global_max_.compare_exchange_weak(old_max, latency_ns,
                                                  std::memory_order_relaxed)) {}
        
        if (total_count_.load(std::memory_order_relaxed) % SAMPLE_RATE == 0) {
            std::lock_guard<std::mutex> lock(sample_mutex_);
            latency_samples_.push_back(latency_ns);
        }
    }
    
    uint64_t count() const {
        return total_count_.load(std::memory_order_relaxed);
    }
    
    uint64_t sum() const {
        return total_sum_.load(std::memory_order_relaxed);
    }
    
    uint64_t min() const {
        return global_min_.load(std::memory_order_relaxed);
    }
    
    uint64_t max() const {
        return global_max_.load(std::memory_order_relaxed);
    }
    
    double avg() const {
        uint64_t cnt = count();
        return cnt > 0 ? static_cast<double>(sum()) / cnt : 0.0;
    }
    
    uint64_t percentile(double p) {
        std::lock_guard<std::mutex> lock(sample_mutex_);
        if (latency_samples_.empty()) return 0;
        std::sort(latency_samples_.begin(), latency_samples_.end());
        size_t idx = static_cast<size_t>(p / 100.0 * latency_samples_.size());
        idx = std::min(idx, latency_samples_.size() - 1);
        return latency_samples_[idx];
    }
    
    void prepare_percentiles() {
        std::lock_guard<std::mutex> lock(sample_mutex_);
        std::sort(latency_samples_.begin(), latency_samples_.end());
    }
    
    void reset() {
        total_count_.store(0, std::memory_order_relaxed);
        total_sum_.store(0, std::memory_order_relaxed);
        global_min_.store(UINT64_MAX, std::memory_order_relaxed);
        global_max_.store(0, std::memory_order_relaxed);
        
        for (auto& stats : per_thread_stats_) {
            stats.reset();
        }
        
        {
            std::lock_guard<std::mutex> lock(sample_mutex_);
            latency_samples_.clear();
        }
    }
};

template<typename Queue>
class BenchmarkHarness {
private:
    Queue& queue_;
    const size_t num_producers_;
    const size_t num_consumers_;
    const size_t items_per_producer_;
    
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> total_enqueued_{0};
    std::atomic<uint64_t> total_dequeued_{0};
    std::atomic<size_t> next_thread_id_{0};
    
    LockFreeStatistics enqueue_stats_;
    LockFreeStatistics dequeue_stats_;
    
    static std::string demangle_type_name(const char* mangled_name) {
        int status;
        char* demangled = abi::__cxa_demangle(mangled_name, nullptr, nullptr, &status);
        std::string result = (status == 0) ? demangled : mangled_name;
        free(demangled);
        return result;
    }
    
    void warmup() {
        constexpr size_t WARMUP_ITEMS = 10000;
        
        std::thread producer([this]() {
            size_t produced = 0;
            while (produced < WARMUP_ITEMS) {
                if (queue_.enqueue(produced)) {
                    produced++;
                }
            }
        });
        
        std::thread consumer([this]() {
            uint64_t item;
            size_t consumed = 0;
            while (consumed < WARMUP_ITEMS) {
                if (queue_.dequeue(item)) {
                    consumed++;
                }
            }
        });
        
        producer.join();
        consumer.join();
        
        uint64_t item;
        while (queue_.dequeue(item)) {}
    }
    
    void producer_worker() {
        size_t thread_id = next_thread_id_.fetch_add(1, std::memory_order_relaxed);
        uint64_t produced = 0;
        
        while (produced < items_per_producer_) {
            auto before = std::chrono::high_resolution_clock::now();
            
            if (queue_.enqueue(produced)) {
                auto after = std::chrono::high_resolution_clock::now();
                auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    after - before).count();
                
                const char* queue_name = demangle_type_name(typeid(Queue).name()).c_str();
                tracepoint(benchmark_mpmc, queue_operation,
                          queue_name, "enqueue", thread_id, latency_ns);
                
                enqueue_stats_.add(thread_id, latency_ns);
                produced++;
                total_enqueued_.fetch_add(1, std::memory_order_relaxed);
            } else {
                __builtin_ia32_pause();
            }
        }
    }
    
    void consumer_worker() {
        size_t thread_id = next_thread_id_.fetch_add(1, std::memory_order_relaxed);
        uint64_t item;
        size_t total_items_expected = items_per_producer_ * num_producers_;
        
        while (!stop_.load(std::memory_order_acquire) || 
               total_dequeued_.load(std::memory_order_relaxed) < total_items_expected) {
            auto before = std::chrono::high_resolution_clock::now();
            
            if (queue_.dequeue(item)) {
                auto after = std::chrono::high_resolution_clock::now();
                auto latency_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    after - before).count();
                
                const char* queue_name = demangle_type_name(typeid(Queue).name()).c_str();
                tracepoint(benchmark_mpmc, queue_operation,
                          queue_name, "dequeue", thread_id, latency_ns);
                
                dequeue_stats_.add(thread_id, latency_ns);
                total_dequeued_.fetch_add(1, std::memory_order_relaxed);
            } else {
                __builtin_ia32_pause();
            }
        }
    }
    
    void print_results(uint64_t duration_ns) {
        uint64_t total_ops = enqueue_stats_.count();
        double throughput_ops = total_ops * 1e9 / duration_ns;
        double throughput_mops = throughput_ops / 1e6;
        
        std::cout << "\n----------------------------------------" << std::endl;
        std::cout << "RESULTS" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Total operations: " << total_ops << std::endl;
        std::cout << "Duration: " << duration_ns / 1e9 << " seconds" << std::endl;
        std::cout << "Throughput: " << throughput_ops << " ops/sec" << std::endl;
        std::cout << "Throughput: " << throughput_mops << " M ops/sec" << std::endl;
        
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
    
public:
    BenchmarkHarness(Queue& q, size_t producers, size_t consumers, size_t items)
        : queue_(q)
        , num_producers_(producers)
        , num_consumers_(consumers)
        , items_per_producer_(items)
        , enqueue_stats_(producers + consumers)
        , dequeue_stats_(producers + consumers) {
        
        if (producers == 0 || consumers == 0 || items == 0) {
            throw std::invalid_argument("Producers, consumers, and items must be positive");
        }
    }
    
    void run() {
        std::string queue_name = demangle_type_name(typeid(Queue).name());
        
        std::cout << "\n========================================" << std::endl;
        std::cout << "Benchmark: " << queue_name << std::endl;
        std::cout << "========================================" << std::endl;
        std::cout << "Producers: " << num_producers_ 
                  << " | Consumers: " << num_consumers_
                  << " | Items/producer: " << items_per_producer_ << std::endl;
        
        enqueue_stats_.reset();
        dequeue_stats_.reset();
        total_enqueued_ = 0;
        total_dequeued_ = 0;
        stop_ = false;
        next_thread_id_ = 0;
        
        std::cout << "Warming up..." << std::endl;
        warmup();
        std::cout << "Warm-up complete." << std::endl;
        
        std::vector<std::thread> producers;
        std::vector<std::thread> consumers;
        
        std::cout << "Running benchmark..." << std::endl;
        auto start_time = std::chrono::high_resolution_clock::now();
        
        for (size_t i = 0; i < num_consumers_; ++i) {
            consumers.emplace_back(&BenchmarkHarness::consumer_worker, this);
        }
        
        for (size_t i = 0; i < num_producers_; ++i) {
            producers.emplace_back(&BenchmarkHarness::producer_worker, this);
        }
        
        for (auto& producer : producers) {
            producer.join();
        }
        
        stop_.store(true, std::memory_order_release);
        for (auto& consumer : consumers) {
            consumer.join();
        }
        
        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            end_time - start_time).count();
        
        enqueue_stats_.prepare_percentiles();
        dequeue_stats_.prepare_percentiles();
        
        print_results(duration_ns);
    }
    
    Queue& queue() { return queue_; }
    size_t producers() const { return num_producers_; }
    size_t consumers() const { return num_consumers_; }
    size_t items_per_producer() const { return items_per_producer_; }
};

#endif
