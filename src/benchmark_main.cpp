/**
 * @file benchmark_main.cpp
 * @brief Main benchmark driver for MPMC queue implementations
 * @author MPMC Benchmark Project
 * @version 1.0.0
 * 
 * This program benchmarks multiple MPMC queue implementations with configurable
 * thread counts and test parameters. Results are printed to stdout and can be
 * captured for analysis.
 * 
 * @section usage Usage
 * @code
 * ./benchmark --type=ringbuffer --producers=4 --consumers=4 --items=1000000
 * 
 * Options:
 *   -t, --type      Queue type (mutex|twolock|ringbuffer)
 *   -p, --producers Number of producer threads
 *   -c, --consumers Number of consumer threads
 *   -i, --items     Items per producer
 *   -s, --size      Queue capacity (for bounded queues)
 *   -h, --help      Show help message
 * @endcode
 */

#include "queues/mutex_queue.h"
#include "queues/two_lock_queue.h"
#include "queues/ring_buffer_queue.h"
#include "benchmark_harness.h"
#include <iostream>
#include <getopt.h>
#include <memory>
#include <cstdlib>

/**
 * @struct Config
 * @brief Benchmark configuration parameters
 */
struct Config {
    std::string queue_type = "ringbuffer";      ///< Queue implementation to test
    size_t producers = 4;                       ///< Number of producer threads
    size_t consumers = 4;                       ///< Number of consumer threads
    size_t items = 1000000;                     ///< Items per producer
    size_t capacity = 10000;                    ///< Queue capacity
    bool help = false;                          ///< Show help
};

/**
 * @brief Print usage information
 */
void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "\nOptions:\n"
              << "  -t, --type TYPE     Queue type: mutex, twolock, ringbuffer (default: ringbuffer)\n"
              << "  -p, --producers N   Number of producer threads (default: 4)\n"
              << "  -c, --consumers N   Number of consumer threads (default: 4)\n"
              << "  -i, --items N       Items per producer (default: 1000000)\n"
              << "  -s, --size N        Queue capacity for bounded queues (default: 10000)\n"
              << "  -h, --help          Show this help message\n"
              << "\nExample:\n"
              << "  " << program_name << " --type=ringbuffer --producers=8 --consumers=8 --items=5000000\n"
              << "\nQueue Types:\n"
              << "  mutex       - Baseline mutex-based queue (blocking)\n"
              << "  twolock     - Lock-free Michael-Scott queue (unbounded)\n"
              << "  ringbuffer  - High-performance bounded ring buffer (lock-free)\n"
              << "\nLTTng Tracing:\n"
              << "  To enable tracing, run:\n"
              << "    lttng create session\n"
              << "    lttng enable-event --userspace benchmark_mpmc:queue_operation\n"
              << "    lttng start\n"
              << "    " << program_name << " ...\n"
              << "    lttng stop\n"
              << "    babeltrace ~/lttng-traces/session-*/\n";
}

/**
 * @brief Parse command line arguments
 * @param argc Argument count
 * @param argv Argument vector
 * @return Parsed configuration
 */
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
                if (cfg.producers == 0) {
                    std::cerr << "Error: Producers must be > 0\n";
                    exit(1);
                }
                break;
            case 'c':
                cfg.consumers = std::stoul(optarg);
                if (cfg.consumers == 0) {
                    std::cerr << "Error: Consumers must be > 0\n";
                    exit(1);
                }
                break;
            case 'i':
                cfg.items = std::stoul(optarg);
                if (cfg.items == 0) {
                    std::cerr << "Error: Items must be > 0\n";
                    exit(1);
                }
                break;
            case 's':
                cfg.capacity = std::stoul(optarg);
                if (cfg.capacity == 0) {
                    std::cerr << "Error: Capacity must be > 0\n";
                    exit(1);
                }
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

/**
 * @brief Main entry point
 */
int main(int argc, char* argv[]) {
    Config cfg = parse_args(argc, argv);
    
    if (cfg.help) {
        print_usage(argv[0]);
        return 0;
    }
    
    // Validate configuration
    if (cfg.producers == 0 || cfg.consumers == 0 || cfg.items == 0) {
        std::cerr << "Error: Producers, consumers, and items must be positive\n";
        return 1;
    }
    
    std::cout << "\n╔════════════════════════════════════════════════════════════╗\n";
    std::cout << "║         MPMC Queue Benchmark Suite                         ║\n";
    std::cout << "╚════════════════════════════════════════════════════════════╝\n";
    
    // Run the appropriate benchmark
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
    else if (cfg.queue_type == "ringbuffer") {
        BoundedRingBufferMPMC<uint64_t> queue(cfg.capacity);
        BenchmarkHarness<BoundedRingBufferMPMC<uint64_t>> harness(
            queue, cfg.producers, cfg.consumers, cfg.items);
        harness.run();
    }
    else {
        std::cerr << "Error: Unknown queue type '" << cfg.queue_type << "'\n";
        std::cerr << "Valid types: mutex, twolock, ringbuffer\n";
        return 1;
    }
    
    return 0;
}
