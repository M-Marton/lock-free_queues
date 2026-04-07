#include "queues.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <string>
#include <cstring>
#include <getopt.h>
#include <algorithm>
#include <random>

// LTTng support (optional)
#ifdef LTTNG_ENABLED
#include <lttng/tracepoint.h>
#include <lttng/ust-tracepoint-event.h>

// Define tracepoint provider
#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER mpmc_benchmark

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "benchmark_tp.h"

#if !defined(_BENCHMARK_TP_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _BENCHMARK_TP_H

#include <lttng/tracepoint.h>

TRACEPOINT_EVENT(
    mpmc_benchmark,
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

#endif /* _BENCHMARK_TP_H */

#include <lttng/tracepoint-event.h>

// Provider registration
#define TRACEPOINT_CREATE_PROBES
#include <lttng/tracepoint.h>

#else
// Dummy tracepoint macros when LTTng is disabled
#define tracepoint(provider, name, ...) ((void)0)
#endif

using namespace std::chrono;

// ============================================================================
// Statistics (Lock-free, per-thread)
// ============================================================================
class Stats {
private:
    struct alignas(64) ThreadData {
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
    
    std::vector<ThreadData> data;
    std::atomic<uint64_t> total_cnt{0};
    std::atomic<uint64_t> total_sum{0};
    std::atomic<uint64_t> global_min{UINT64_MAX};
    std::atomic<uint64_t> global_max{0};
    std::vector<uint64_t> samples;
    std::mutex sample_mtx;
    
public:
    Stats(size_t threads = 256) : data(threads) {}
    
    void add(size_t tid, uint64_t ns) {
        size_t idx = tid % data.size();
        ThreadData& d = data[idx];
        
        d.count.fetch_add(1, std::memory_order_relaxed);
        d.sum.fetch_add(ns, std::memory_order_relaxed);
        
        uint64_t old_min = d.min.load(std::memory_order_relaxed);
        while (ns < old_min && !d.min.compare_exchange_weak(old_min, ns, 
                std::memory_order_relaxed, std::memory_order_relaxed)) {}
        
        uint64_t old_max = d.max.load(std::memory_order_relaxed);
        while (ns > old_max && !d.max.compare_exchange_weak(old_max, ns,
                std::memory_order_relaxed, std::memory_order_relaxed)) {}
        
        total_cnt.fetch_add(1, std::memory_order_relaxed);
        total_sum.fetch_add(ns, std::memory_order_relaxed);
        
        old_min = global_min.load(std::memory_order_relaxed);
        while (ns < old_min && !global_min.compare_exchange_weak(old_min, ns,
                std::memory_order_relaxed, std::memory_order_relaxed)) {}
        
        old_max = global_max.load(std::memory_order_relaxed);
        while (ns > old_max && !global_max.compare_exchange_weak(old_max, ns,
                std::memory_order_relaxed, std::memory_order_relaxed)) {}
        
        if (total_cnt.load(std::memory_order_relaxed) % 100 == 0) {
            std::lock_guard<std::mutex> lock(sample_mtx);
            samples.push_back(ns);
            if (samples.size() > 10000) samples.erase(samples.begin());
        }
    }
    
    uint64_t count() const { return total_cnt.load(std::memory_order_relaxed); }
    uint64_t sum() const { return total_sum.load(std::memory_order_relaxed); }
    uint64_t min() const { return global_min.load(std::memory_order_relaxed); }
    uint64_t max() const { return global_max.load(std::memory_order_relaxed); }
    
    double avg() const { 
        uint64_t c = count();
        return c > 0 ? static_cast<double>(sum()) / c : 0.0; 
    }
    
    uint64_t p99() {
        std::lock_guard<std::mutex> lock(sample_mtx);
        if (samples.empty()) return 0;
        std::sort(samples.begin(), samples.end());
        size_t idx = samples.size() * 99 / 100;
        return samples[std::min(idx, samples.size() - 1)];
    }
    
    void reset() {
        total_cnt.store(0, std::memory_order_relaxed);
        total_sum.store(0, std::memory_order_relaxed);
        global_min.store(UINT64_MAX, std::memory_order_relaxed);
        global_max.store(0, std::memory_order_relaxed);
        for (auto& d : data) d.reset();
        {
            std::lock_guard<std::mutex> lock(sample_mtx);
            samples.clear();
        }
    }
};

// ============================================================================
// Benchmark Harness
// ============================================================================
template<typename Q>
class Benchmark {
private:
    Q& queue;
    size_t producers;
    size_t consumers;
    size_t items;
    std::string queue_name;
    std::atomic<bool> stop{false};
    std::atomic<size_t> done_enq{0};
    std::atomic<size_t> done_deq{0};
    std::atomic<size_t> next_id{0};
    Stats enq_stats;
    Stats deq_stats;
    
    static std::string demangle(const char* name) {
        return name;
    }
    
    void producer_worker() {
        size_t tid = next_id.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < items; i++) {
            auto start = high_resolution_clock::now();
            while (!queue.enqueue(i)) {
                if (stop.load(std::memory_order_relaxed)) return;
                __builtin_ia32_pause();
            }
            auto ns = duration_cast<nanoseconds>(high_resolution_clock::now() - start).count();
            
            // LTTng tracepoint
            tracepoint(mpmc_benchmark, queue_operation, 
                      queue_name.c_str(), "enqueue", tid, ns);
            
            enq_stats.add(tid, ns);
            done_enq.fetch_add(1, std::memory_order_relaxed);
        }
    }
    
    void consumer_worker() {
        size_t tid = next_id.fetch_add(1, std::memory_order_relaxed);
        uint64_t dummy;
        size_t total = producers * items;
        while (done_deq.load(std::memory_order_relaxed) < total) {
            auto start = high_resolution_clock::now();
            if (queue.dequeue(dummy)) {
                auto ns = duration_cast<nanoseconds>(high_resolution_clock::now() - start).count();
                
                // LTTng tracepoint
                tracepoint(mpmc_benchmark, queue_operation,
                          queue_name.c_str(), "dequeue", tid, ns);
                
                deq_stats.add(tid, ns);
                done_deq.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (stop.load(std::memory_order_relaxed)) return;
                __builtin_ia32_pause();
            }
        }
    }
    
public:
    Benchmark(Q& q, size_t p, size_t c, size_t i, const std::string& name) 
        : queue(q), producers(p), consumers(c), items(i), queue_name(name) {}
    
    void run() {
        enq_stats.reset();
        deq_stats.reset();
        done_enq.store(0, std::memory_order_relaxed);
        done_deq.store(0, std::memory_order_relaxed);
        next_id.store(0, std::memory_order_relaxed);
        stop.store(false, std::memory_order_relaxed);
        
        std::vector<std::thread> producers_th;
        std::vector<std::thread> consumers_th;
        
        auto start = high_resolution_clock::now();
        
        for (size_t i = 0; i < consumers; i++) {
            consumers_th.emplace_back(&Benchmark::consumer_worker, this);
        }
        for (size_t i = 0; i < producers; i++) {
            producers_th.emplace_back(&Benchmark::producer_worker, this);
        }
        
        for (auto& t : producers_th) t.join();
        stop.store(true, std::memory_order_relaxed);
        for (auto& t : consumers_th) t.join();
        
        auto ns = duration_cast<nanoseconds>(high_resolution_clock::now() - start).count();
        
        uint64_t total = enq_stats.count();
        double throughput = total * 1e9 / ns;
        
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "\n========================================\n";
        std::cout << "RESULTS\n";
        std::cout << "========================================\n";
        std::cout << "Throughput: " << throughput / 1e6 << " M ops/sec\n";
        std::cout << "Total ops: " << total << "\n";
        std::cout << "Duration: " << ns / 1e9 << " s\n\n";
        
        std::cout << "Enqueue (ns): min=" << enq_stats.min() 
                  << " max=" << enq_stats.max()
                  << " avg=" << (int)enq_stats.avg()
                  << " p99=" << enq_stats.p99() << "\n";
        std::cout << "Dequeue (ns): min=" << deq_stats.min() 
                  << " max=" << deq_stats.max()
                  << " avg=" << (int)deq_stats.avg()
                  << " p99=" << deq_stats.p99() << "\n";
        std::cout << "========================================\n";
    }
};

// ============================================================================
// Main
// ============================================================================
struct Config {
    std::string type = "ringbuffer";
    size_t producers = 4;
    size_t consumers = 4;
    size_t items = 1000000;
    size_t capacity = 100000;
    bool help = false;
};

void usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  -t TYPE     mutex | ringbuffer | bounded\n"
              << "  -p NUM      Producers (default: 4)\n"
              << "  -c NUM      Consumers (default: 4)\n"
              << "  -i NUM      Items per producer (default: 1000000)\n"
              << "  -s NUM      Queue capacity (default: 100000)\n"
              << "  -h          Show help\n";
}

Config parse_args(int argc, char** argv) {
    Config cfg;
    int opt;
    while ((opt = getopt(argc, argv, "t:p:c:i:s:h")) != -1) {
        switch (opt) {
            case 't': cfg.type = optarg; break;
            case 'p': cfg.producers = std::stoul(optarg); break;
            case 'c': cfg.consumers = std::stoul(optarg); break;
            case 'i': cfg.items = std::stoul(optarg); break;
            case 's': cfg.capacity = std::stoul(optarg); break;
            case 'h': cfg.help = true; break;
            default: cfg.help = true; break;
        }
    }
    return cfg;
}

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    if (cfg.help) { 
        usage(argv[0]); 
        return 0; 
    }
    
    std::cout << "\nBenchmark: " << cfg.type 
              << " | P=" << cfg.producers << " C=" << cfg.consumers 
              << " | Items=" << cfg.items << "\n";
    
    try {
        if (cfg.type == "mutex") {
            MutexQueue<uint64_t> q(cfg.capacity);
            Benchmark<MutexQueue<uint64_t>> bench(q, cfg.producers, cfg.consumers, cfg.items, "mutex");
            bench.run();
        }
        else if (cfg.type == "ringbuffer") {
            RingBufferQueue<uint64_t> q(cfg.capacity);
            Benchmark<RingBufferQueue<uint64_t>> bench(q, cfg.producers, cfg.consumers, cfg.items, "ringbuffer");
            bench.run();
        }
        else if (cfg.type == "bounded") {
            BoundedMPMCQueue<uint64_t> q(cfg.capacity);
            Benchmark<BoundedMPMCQueue<uint64_t>> bench(q, cfg.producers, cfg.consumers, cfg.items, "bounded");
            bench.run();
        }
        else {
            std::cerr << "Unknown type: " << cfg.type << "\n";
            std::cerr << "Valid types: mutex, ringbuffer, bounded\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}
