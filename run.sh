#!/bin/bash
# MPMC Benchmark Runner with cgroup CPU limiting and LTTng support

set -u

# Get the original user who ran sudo
ORIGINAL_USER=${SUDO_USER:-$USER}
ORIGINAL_HOME=$(eval echo ~$ORIGINAL_USER)

RESULTS_DIR="./results"
DATA_DIR="$RESULTS_DIR/data"
TRACES_DIR="$RESULTS_DIR/traces"
mkdir -p "$DATA_DIR" "$TRACES_DIR"

# Fix permissions for the original user
chown -R $ORIGINAL_USER:$ORIGINAL_USER "$RESULTS_DIR" 2>/dev/null || true

# ============================================================================
# Configuration
# ============================================================================
QUEUES=("mutex" "ringbuffer" "bounded")
CORES=(1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20)
PRODUCERS=16
CONSUMERS=16
ITEMS=1000000
CAPACITY=100000
TIMEOUT=120
RETRIES=3

# LTTng configuration (set to 0 to disable)
LTTNG_ENABLED=${LTTNG_ENABLED:-0}  # Disabled by default since no lttng-ust

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Results file
RESULT_CSV="$DATA_DIR/results.csv"
echo "queue,cores,throughput_mops,enqueue_p99_ns,dequeue_p99_ns,retries,trace_file" > "$RESULT_CSV"

# ============================================================================
# LTTng setup (disabled)
# ============================================================================
setup_lttng() {
    return 1
}

stop_lttng() {
    return 0
}

# ============================================================================
# cgroup helpers (cgroups v2)
# ============================================================================
CGROUP_PATH="/sys/fs/cgroup/benchmark_$$"

setup_cgroup() {
    local cores=$1
    sudo mkdir -p "$CGROUP_PATH" 2>/dev/null || true
    
    local period=100000
    local quota=$((cores * period))
    echo "$quota $period" | sudo tee "$CGROUP_PATH/cpu.max" 2>/dev/null || true
}

cleanup_cgroup() {
    sudo rmdir "$CGROUP_PATH" 2>/dev/null || true
}

# ============================================================================
# Build
# ============================================================================
echo "Building benchmark..."

# Build without LTTng for simplicity
g++ -O3 -march=native -pthread -std=c++17 benchmark.cpp -o benchmark 2>/dev/null

if [ ! -f "./benchmark" ]; then
    echo "ERROR: Compilation failed"
    exit 1
fi

# ============================================================================
# Benchmark runner
# ============================================================================
run_bench() {
    local queue=$1
    local cores=$2
    
    printf "  %-12s cores=%-2d : " "$queue" "$cores"
    
    local retry=0
    local success=0
    local throughput=""
    local enq_p99=""
    local deq_p99=""
    local trace_file=""
    
    while [ $retry -lt $RETRIES ] && [ $success -eq 0 ]; do
        local session="bench_${queue}_c${cores}"
        trace_file="$TRACES_DIR/$session"
        local output="$DATA_DIR/${queue}_c${cores}_run${retry}.log"
        
        # Setup cgroup
        setup_cgroup $cores
        
        # Run in cgroup
        (
            echo $$ > "$CGROUP_PATH/cgroup.procs" 2>/dev/null || true
            timeout $TIMEOUT ./benchmark -t "$queue" -p $PRODUCERS -c $CONSUMERS -i $ITEMS -s $CAPACITY > "$output" 2>&1
        )
        local exit_code=$?
        
        cleanup_cgroup
        
        if [ $exit_code -eq 0 ] && [ -f "$output" ]; then
            # Extract throughput - look for number before "M ops/sec"
            throughput=$(grep "Throughput:" "$output" | sed 's/.*Throughput: //' | awk '{print $1}')
            enq_p99=$(grep "Enqueue.*p99=" "$output" | sed 's/.*p99=//' | awk '{print $1}')
            deq_p99=$(grep "Dequeue.*p99=" "$output" | sed 's/.*p99=//' | awk '{print $1}')
            
            # Validate throughput is a number
            if [[ "$throughput" =~ ^[0-9]+\.?[0-9]*$ ]] && [ -n "$throughput" ] && [ "$throughput" != "0" ]; then
                success=1
                echo -e "${GREEN}✓${NC} $throughput M ops/sec"
            else
                echo -e "${YELLOW}⚠${NC} Invalid throughput: '$throughput'"
            fi
        fi
        
        retry=$((retry + 1))
    done
    
    if [ $success -eq 1 ]; then
        echo "$queue,$cores,$throughput,$enq_p99,$deq_p99,$retry,$trace_file" >> "$RESULT_CSV"
    else
        echo -e "${RED}✗${NC} FAILED after $RETRIES attempts"
        echo "$queue,$cores,0,0,0,$RETRIES," >> "$RESULT_CSV"
    fi
}

# ============================================================================
# Main
# ============================================================================
echo -e "${GREEN}╔════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║     MPMC Queue Benchmark - Controlled CPU Scaling          ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "Configuration:"
echo "  Producers: $PRODUCERS | Consumers: $CONSUMERS"
echo "  Items per producer: $ITEMS"
echo "  Queue capacity: $CAPACITY"
echo "  CPU cores to test: ${CORES[@]}"
echo ""

for cores in "${CORES[@]}"; do
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Testing with $cores CPU core(s)${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    
    for queue in "${QUEUES[@]}"; do
        run_bench "$queue" "$cores"
    done
    echo ""
done

# Fix permissions for analysis
chown -R $ORIGINAL_USER:$ORIGINAL_USER "$RESULTS_DIR" 2>/dev/null || true

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Benchmark complete!${NC}"
echo "Results: $RESULT_CSV"
echo ""
echo "Run analysis: python3 analyze.py"
