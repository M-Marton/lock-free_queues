#include "queues.h"
#include "benchmark_tp.h"
#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <string>
#include <getopt.h>
#include <algorithm>
#include <unistd.h>
#include <fstream>
#include <sys/stat.h>

using namespace std::chrono;

// ============================================================================
// Stats class
// ============================================================================
class Stats {
private:
    struct alignas(64) ThreadData {
        std::atomic<uint64_t> count{0};
        std::atomic<uint64_t> sum{0};
        std::atomic<uint64_t> min{UINT64_MAX};
        std::atomic<uint64_t> max{0};
        void reset() {
            count = 0; sum = 0; min = UINT64_MAX; max = 0;
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
        auto& d = data[idx];
        d.count.fetch_add(1, std::memory_order_relaxed);
        d.sum.fetch_add(ns, std::memory_order_relaxed);
        uint64_t old = d.min.load(std::memory_order_relaxed);
        while (ns < old && !d.min.compare_exchange_weak(old, ns)) {}
        old = d.max.load(std::memory_order_relaxed);
        while (ns > old && !d.max.compare_exchange_weak(old, ns)) {}
        total_cnt.fetch_add(1, std::memory_order_relaxed);
        total_sum.fetch_add(ns, std::memory_order_relaxed);
        old = global_min.load(std::memory_order_relaxed);
        while (ns < old && !global_min.compare_exchange_weak(old, ns)) {}
        old = global_max.load(std::memory_order_relaxed);
        while (ns > old && !global_max.compare_exchange_weak(old, ns)) {}
        if (total_cnt.load() % 1000 == 0) {
            std::lock_guard<std::mutex> lock(sample_mtx);
            samples.push_back(ns);
            if (samples.size() > 10000) samples.erase(samples.begin());
        }
    }

    uint64_t count() const { return total_cnt.load(); }
    uint64_t sum() const { return total_sum.load(); }
    uint64_t min() const { return global_min.load(); }
    uint64_t max() const { return global_max.load(); }
    double avg() const { return count() ? (double)sum() / count() : 0; }
    uint64_t p99() {
        std::lock_guard<std::mutex> lock(sample_mtx);
        if (samples.empty()) return 0;
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() * 99 / 100];
    }
    void reset() {
        total_cnt = 0; total_sum = 0; global_min = UINT64_MAX; global_max = 0;
        for (auto& d : data) d.reset();
        std::lock_guard<std::mutex> lock(sample_mtx);
        samples.clear();
    }
};

// ============================================================================
// Benchmark harness
// ============================================================================
template<typename Q>
class Benchmark {
private:
    Q& queue;
    size_t producers, consumers, items;
    std::string queue_name;
    std::atomic<bool> stop{false};
    std::atomic<size_t> done_enq{0};
    std::atomic<size_t> done_deq{0};
    std::atomic<size_t> next_id{0};
    Stats enq_stats, deq_stats;

public:
    Benchmark(Q& q, size_t p, size_t c, size_t i, const std::string& name)
        : queue(q), producers(p), consumers(c), items(i), queue_name(name) {}

    void run() {
        enq_stats.reset();
        deq_stats.reset();
        done_enq = 0;
        done_deq = 0;
        next_id = 0;
        stop = false;

        std::vector<std::thread> producers_th, consumers_th;
        auto start = high_resolution_clock::now();

        for (size_t i = 0; i < consumers; ++i) {
            consumers_th.emplace_back([this]() {
                size_t tid = next_id.fetch_add(1);
                uint64_t dummy;
                size_t total = producers * items;
                while (done_deq.load() < total) {
                    auto before = high_resolution_clock::now();
                    if (queue.dequeue(dummy)) {
                        auto ns = duration_cast<nanoseconds>(high_resolution_clock::now() - before).count();
                        tracepoint(mpmc_benchmark, queue_op, queue_name.c_str(), "dequeue", tid, ns);
                        deq_stats.add(tid, ns);
                        done_deq.fetch_add(1);
                    } else {
                        __builtin_ia32_pause();
                    }
                }
            });
        }

        for (size_t i = 0; i < producers; ++i) {
            producers_th.emplace_back([this]() {
                size_t tid = next_id.fetch_add(1);
                for (size_t i = 0; i < items; ++i) {
                    auto before = high_resolution_clock::now();
                    while (!queue.enqueue(i)) {
                        __builtin_ia32_pause();
                    }
                    auto ns = duration_cast<nanoseconds>(high_resolution_clock::now() - before).count();
                    tracepoint(mpmc_benchmark, queue_op, queue_name.c_str(), "enqueue", tid, ns);
                    enq_stats.add(tid, ns);
                    done_enq.fetch_add(1);
                }
            });
        }

        for (auto& t : producers_th) t.join();
        stop = true;
        for (auto& t : consumers_th) t.join();

        auto ns = duration_cast<nanoseconds>(high_resolution_clock::now() - start).count();
        uint64_t total = enq_stats.count();
        double throughput = total * 1e9 / ns;

        std::cout << std::fixed << std::setprecision(4);
        std::cout << "\n========================================\n";
        std::cout << "RESULTS\n";
        std::cout << "========================================\n";
        std::cout << "Throughput: " << throughput / 1e6 << "\n";
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
    std::string pidfile = "";
    bool help = false;
};

void usage(const char* prog) {
    std::cout << "Usage: " << prog << " [options]\n"
              << "  -t TYPE     mutex | ringbuffer | bounded | hazard\n"
              << "  -p NUM      Producers (default: 4)\n"
              << "  -c NUM      Consumers (default: 4)\n"
              << "  -i NUM      Items per producer (default: 1000000)\n"
              << "  -s NUM      Queue capacity (default: 100000, ignored for hazard)\n"
              << "  -f FILE     Write PID to FILE (optional)\n"
              << "  -h          Help\n";
}

Config parse_args(int argc, char** argv) {
    Config cfg;
    int opt;
    while ((opt = getopt(argc, argv, "t:p:c:i:s:f:h")) != -1) {
        switch (opt) {
            case 't': cfg.type = optarg; break;
            case 'p': cfg.producers = std::stoul(optarg); break;
            case 'c': cfg.consumers = std::stoul(optarg); break;
            case 'i': cfg.items = std::stoul(optarg); break;
            case 's': cfg.capacity = std::stoul(optarg); break;
            case 'f': cfg.pidfile = optarg; break;
            case 'h': cfg.help = true; break;
        }
    }
    return cfg;
}

int main(int argc, char** argv) {
    Config cfg = parse_args(argc, argv);
    if (cfg.help) { usage(argv[0]); return 0; }

    // Write PID to file if requested
    if (!cfg.pidfile.empty()) {
        std::ofstream pf(cfg.pidfile);
        if (pf.is_open()) {
            pf << getpid();
            pf.close();
            chmod(cfg.pidfile.c_str(), 0644);
        }
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
        else if (cfg.type == "hazard") {
            TwoLockHazardQueue<uint64_t> q;
            Benchmark<TwoLockHazardQueue<uint64_t>> bench(q, cfg.producers, cfg.consumers, cfg.items, "hazard");
            bench.run();
        }
        else {
            std::cerr << "Unknown type: " << cfg.type << "\n";
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    // Remove PID file
    if (!cfg.pidfile.empty()) {
        unlink(cfg.pidfile.c_str());
    }
    return 0;
}
