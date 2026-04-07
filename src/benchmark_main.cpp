#include "queues/mutex_queue.h"
#include "queues/two_lock_queue.h"
#include "queues/two_lock_queue_hazard.h"
#include "queues/ring_buffer_queue.h"
#include "queues/scq_queue.h"
#include "queues/disruptor_queue.h"
#include "benchmark_harness.h"
#include <iostream>
#include <getopt.h>
#include <memory>
#include <cstdlib>
#include <string>

struct Config {
    std::string queue_type = "ringbuffer";
    size_t producers = 4;
    size_t consumers = 4;
    size_t items = 1000000;
    size_t capacity = 100000;
    bool help = false;
};

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "\nOptions:\n"
              << "  -t, --type TYPE     Queue type:\n"
              << "                      mutex, twolock, twolock_hazard,\n"
              << "                      ringbuffer, scq, disruptor\n"
              << "  -p, --producers N   Number of producer threads (default: 4)\n"
              << "  -c, --consumers N   Number of consumer threads (default: 4)\n"
              << "  -i, --items N       Items per producer (default: 1000000)\n"
              << "  -s, --size N        Queue capacity (default: 100000)\n"
              << "  -h, --help          Show this help\n"
              << "\nQueue Types:\n"
              << "  mutex        - Baseline mutex-based queue\n"
              << "  twolock      - Michael-Scott lock-free queue\n"
              << "  twolock_hazard - Michael-Scott with hazard pointers\n"
              << "  ringbuffer   - Bounded ring buffer (lock-free)\n"
              << "  scq          - Scalable Circular Queue (fetch-and-add)\n"
              << "  disruptor    - LMAX Disruptor pattern\n";
}

Config parse_args(int argc, char* argv[]) {
    Config cfg;
    
    static struct option long_options[] = {
        {"type", required_argument, 0, 't'},
        {"producers", required_argument, 0, 'p'},
        {"consumers", required_argument, 0, 'c'},
        {"items", required_argument, 0, 'i'},
        {"size", required_argument, 0, 's'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "t:p:c:i:s:h", long_options, nullptr)) != -1) {
        switch (opt) {
            case 't':
                cfg.queue_type = optarg;
                break;
            case 'p':
                cfg.producers = std::stoul(optarg);
                break;
            case 'c':
                cfg.consumers = std::stoul(optarg);
                break;
            case 'i':
                cfg.items = std::stoul(optarg);
                break;
            case 's':
                cfg.capacity = std::stoul(optarg);
                break;
            case 'h':
                cfg.help = true;
                break;
            default:
                cfg.help = true;
                break;
        }
    }
    
    return cfg;
}

int main(int argc, char* argv[]) {
    Config cfg = parse_args(argc, argv);
    
    if (cfg.help) {
        print_usage(argv[0]);
        return 0;
    }
    
    std::cout << "\nMPMC Queue Benchmark\n";
    std::cout << "====================\n";
    std::cout << "Type: " << cfg.queue_type << "\n";
    std::cout << "Producers: " << cfg.producers << "\n";
    std::cout << "Consumers: " << cfg.consumers << "\n";
    std::cout << "Items per producer: " << cfg.items << "\n";
    std::cout << "Capacity: " << cfg.capacity << "\n";
    
    if (cfg.queue_type == "mutex") {
        MutexMPMCQueue<uint64_t> queue(cfg.capacity);
        BenchmarkHarness<MutexMPMCQueue<uint64_t>> harness(
            queue, cfg.producers, cfg.consumers, cfg.items);
        harness.run();
    }
    else if (cfg.queue_type == "twolock") {
        TwoLockMPMCQueue<uint64_t> queue;
        BenchmarkHarness<TwoLockMPMCQueue<uint64_t>> harness(
            queue, cfg.producers, cfg.consumers, cfg.items);
        harness.run();
    }
    else if (cfg.queue_type == "twolock_hazard") {
        TwoLockMPMCQueueHazard<uint64_t> queue;
        BenchmarkHarness<TwoLockMPMCQueueHazard<uint64_t>> harness(
            queue, cfg.producers, cfg.consumers, cfg.items);
        harness.run();
    }
    else if (cfg.queue_type == "ringbuffer") {
        BoundedRingBufferMPMC<uint64_t> queue(cfg.capacity);
        BenchmarkHarness<BoundedRingBufferMPMC<uint64_t>> harness(
            queue, cfg.producers, cfg.consumers, cfg.items);
        harness.run();
    }
    else if (cfg.queue_type == "scq") {
        SCQQueue<uint64_t> queue(cfg.capacity);
        BenchmarkHarness<SCQQueue<uint64_t>> harness(
            queue, cfg.producers, cfg.consumers, cfg.items);
        harness.run();
    }
    else if (cfg.queue_type == "disruptor") {
        DisruptorQueue<uint64_t> queue(cfg.capacity);
        BenchmarkHarness<DisruptorQueue<uint64_t>> harness(
            queue, cfg.producers, cfg.consumers, cfg.items);
        harness.run();
    }
    else {
        std::cerr << "Unknown queue type: " << cfg.queue_type << "\n";
        return 1;
    }
    
    return 0;
}
