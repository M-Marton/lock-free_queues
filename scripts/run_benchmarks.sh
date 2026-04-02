#!/bin/bash
# @file run_benchmarks.sh
# @brief Script to run all benchmarks with various configurations
# @author MPMC Benchmark Project
# @version 1.0.0

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configuration
BENCHMARK_BIN="./build/benchmark"
RESULTS_DIR="./results"
TRACE_DIR="/tmp/lttng-traces"

# Create results directory
mkdir -p "$RESULTS_DIR"

# Queue types to test
QUEUES=("mutex" "twolock" "ringbuffer")

# Thread configurations (producers, consumers)
THREAD_CONFIGS=(

    "32 1"
    "32 2"
    "32 4"
    "32 8"
    "32 16"
    "32 32"
)

# Items per producer
ITEMS=10000

echo -e "${GREEN}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║         MPMC Queue Benchmark Suite                         ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════════════════════════╝${NC}"

# Check if benchmark exists
if [ ! -f "$BENCHMARK_BIN" ]; then
    echo -e "${RED}Error: Benchmark executable not found at $BENCHMARK_BIN${NC}"
    echo "Please build the project first:"
    echo "  mkdir build && cd build"
    echo "  cmake .. && make"
    exit 1
fi

# Run benchmarks without tracing first
echo -e "\n${YELLOW}Running benchmarks without tracing...${NC}\n"

for queue in "${QUEUES[@]}"; do
    for config in "${THREAD_CONFIGS[@]}"; do
        producers=$(echo $config | cut -d' ' -f1)
        consumers=$(echo $config | cut -d' ' -f2)
        
        output_file="$RESULTS_DIR/${queue}_p${producers}_c${consumers}.csv"
        
        echo "Testing $queue with $producers producers, $consumers consumers..."
        
        $BENCHMARK_BIN \
            --type="$queue" \
            --producers="$producers" \
            --consumers="$consumers" \
            --items="$ITEMS" \
            --size=100000 \
            > "$output_file"
        
        echo "Results saved to $output_file"
    done
done

# Run with LTTng tracing if enabled
if command -v lttng &> /dev/null; then
    echo -e "\n${YELLOW}Running benchmarks with LTTng tracing...${NC}\n"
    
    for queue in "${QUEUES[@]}"; do
        for config in "${THREAD_CONFIGS[@]}"; do
            producers=$(echo $config | cut -d' ' -f1)
            consumers=$(echo $config | cut -d' ' -f2)
            
            session_name="mpmc_${queue}_p${producers}_c${consumers}"
            trace_output="$RESULTS_DIR/traces_${queue}_p${producers}_c${consumers}"
            
            echo "Tracing $queue with $producers producers, $consumers consumers..."
            
            # Create LTTng session
            lttng create "$session_name" --output="$trace_output" > /dev/null 2>&1
            
            # Enable tracepoints
            lttng enable-event --userspace benchmark_mpmc:queue_operation > /dev/null 2>&1
            
            # Add context
            lttng add-context --userspace -t vtid -t vpid -t procname > /dev/null 2>&1
            
            # Start tracing
            lttng start > /dev/null 2>&1
            
            # Run benchmark
            $BENCHMARK_BIN \
                --type="$queue" \
                --producers="$producers" \
                --consumers="$consumers" \
                --items="$ITEMS" \
                --size=100000 \
                > /dev/null
            
            # Stop and destroy session
            lttng stop > /dev/null 2>&1
            lttng destroy > /dev/null 2>&1
            
            echo "Trace saved to $trace_output"
        done
    done
else
    echo -e "${YELLOW}LTTng not found. Install lttng-tools and lttng-ust for tracing support.${NC}"
fi

# Generate summary
echo -e "\n${GREEN}========================================${NC}"
echo -e "${GREEN}Benchmark Summary${NC}"
echo -e "${GREEN}========================================${NC}"

for queue in "${QUEUES[@]}"; do
    echo -e "\n${YELLOW}$queue Queue Results:${NC}"
    for config in "${THREAD_CONFIGS[@]}"; do
        producers=$(echo $config | cut -d' ' -f1)
        consumers=$(echo $config | cut -d' ' -f2)
        output_file="$RESULTS_DIR/${queue}_p${producers}_c${consumers}.csv"
        
        if [ -f "$output_file" ]; then
            # Extract throughput from results
            throughput=$(grep "Throughput:" "$output_file" | awk '{print $2}')
            echo "  P$producers C$consumers: $throughput ops/sec"
        fi
    done
done

echo -e "\n${GREEN}All benchmarks complete!${NC}"
echo "Results saved in $RESULTS_DIR/"

