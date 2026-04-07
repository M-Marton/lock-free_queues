#!/bin/bash

# No "set -e" - we want to continue even after failures
set -u  # Only fail on undefined variables, not on command failures

BENCHMARK_BIN="./build/benchmark"
RESULTS_DIR="./results"
DATA_DIR="$RESULTS_DIR/data"
TRACES_DIR="$RESULTS_DIR/traces"
LOGS_DIR="$RESULTS_DIR/logs"

MAX_RETRIES=10
TIMEOUT_SECONDS=120
LTTNG_ENABLED=${LTTNG_ENABLED:-1}

mkdir -p "$DATA_DIR" "$TRACES_DIR" "$LOGS_DIR"
#scq removed
QUEUES=("mutex" "twolock" "twolock_hazard" "ringbuffer" "scq" "disruptor")

PRODUCER_CONFIGS=(1 2 4 8 12 16)
CONSUMER_CONFIGS=(1 2 4 8 12 16)

ITEMS=5000
CAPACITY=1024

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

RESULTS_CSV="$DATA_DIR/summary.csv"
echo "queue,producers,consumers,throughput,enqueue_p99,dequeue_p99,retries,deadlocks,trace_file,success" > "$RESULTS_CSV"

# Global counters for final summary
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
CRASHED_TESTS=0

run_benchmark() {
    local queue=$1
    local producers=$2
    local consumers=$3
    local retry=0
    local deadlocks=0
    local success=0
    local trace_file=""
    local crash_detected=0
    
    while [ $retry -lt $MAX_RETRIES ] && [ $success -eq 0 ]; do
        local run_id="${queue}_p${producers}_c${consumers}_run${retry}"
        local output_file="$DATA_DIR/${run_id}.csv"
        local log_file="$LOGS_DIR/${run_id}.log"
        
        # Initialize trace session only on first attempt
        if [ $LTTNG_ENABLED -eq 1 ] && [ $retry -eq 0 ]; then
            local session_name="bench_${queue}_p${producers}_c${consumers}"
            trace_file="$TRACES_DIR/${session_name}"
            
            # Clean up any existing session with same name
            lttng destroy "$session_name" 2>/dev/null || true
            
            lttng create "$session_name" --output="$trace_file" > /dev/null 2>&1
            if [ $? -eq 0 ]; then
                lttng enable-event --userspace benchmark_mpmc:queue_operation > /dev/null 2>&1
                lttng add-context --userspace -t vtid -t vpid -t procname > /dev/null 2>&1
                lttng start > /dev/null 2>&1
            else
                trace_file=""
            fi
        fi
        
        # Run benchmark with timeout
        set +e  # Temporarily disable exit on error
        timeout $TIMEOUT_SECONDS $BENCHMARK_BIN \
            --type="$queue" \
            --producers="$producers" \
            --consumers="$consumers" \
            --items="$ITEMS" \
            --size="$CAPACITY" \
            > "$output_file" 2> "$log_file"
        
        local exit_code=$?
        set -e  # Re-enable exit on error
        
        # Stop tracing if it was started
        if [ $LTTNG_ENABLED -eq 1 ] && [ -n "$trace_file" ]; then
            lttng stop 2>/dev/null || true
            lttng destroy 2>/dev/null || true
        fi
        
        # Analyze result
        if [ $exit_code -eq 0 ] && grep -q "Throughput:" "$output_file" 2>/dev/null; then
            success=1
            echo -e "${GREEN}✓${NC} Success after $retry retries"
        elif [ $exit_code -eq 124 ]; then
            echo -e "${RED}✗${NC} Timeout after ${TIMEOUT_SECONDS}s (attempt $((retry+1))/$MAX_RETRIES)"
            deadlocks=$((deadlocks + 1))
            crash_detected=1
        else
            echo -e "${RED}✗${NC} Crash with exit code $exit_code (attempt $((retry+1))/$MAX_RETRIES)"
            deadlocks=$((deadlocks + 1))
            crash_detected=1
        fi
        
        retry=$((retry + 1))
    done
    
    # Record result even if failed
    if [ $success -eq 1 ]; then
        local throughput=$(grep "Throughput:" "$output_file" | head -1 | awk '{print $2}')
        local enqueue_p99=$(grep -A10 "Enqueue Latency" "$output_file" | grep "P99:" | head -1 | awk '{print $2}')
        local dequeue_p99=$(grep -A10 "Dequeue Latency" "$output_file" | grep "P99:" | head -1 | awk '{print $2}')
        
        # Handle missing values
        enqueue_p99=${enqueue_p99:-0}
        dequeue_p99=${dequeue_p99:-0}
        
        echo "$queue,$producers,$consumers,$throughput,$enqueue_p99,$dequeue_p99,$retry,$deadlocks,$trace_file,1" >> "$RESULTS_CSV"
        
        printf "  Throughput: %'.0f ops/sec | P99: %s ns | Retries: %d | Deadlocks: %d\n" \
               "$throughput" "$enqueue_p99" "$retry" "$deadlocks"
        
        PASSED_TESTS=$((PASSED_TESTS + 1))
    else
        echo "$queue,$producers,$consumers,0,0,0,$MAX_RETRIES,$deadlocks,$trace_file,0" >> "$RESULTS_CSV"
        echo -e "${RED}  FAILED after $MAX_RETRIES attempts ($deadlocks deadlocks/crashes)${NC}"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        if [ $crash_detected -eq 1 ]; then
            CRASHED_TESTS=$((CRASHED_TESTS + 1))
        fi
    fi
    
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    echo ""
}

print_header() {
    local queue=$1
    echo -e "\n${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
    echo -e "${BLUE}Testing: $queue${NC}"
    echo -e "${BLUE}━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━${NC}"
}

print_final_summary() {
    echo -e "\n${GREEN}════════════════════════════════════════════════════════════════${NC}"
    echo -e "${GREEN}                    FINAL SUMMARY${NC}"
    echo -e "${GREEN}════════════════════════════════════════════════════════════════${NC}"
    echo ""
    echo "Total configurations tested: $TOTAL_TESTS"
    echo -e "${GREEN}Passed: $PASSED_TESTS${NC}"
    echo -e "${RED}Failed: $FAILED_TESTS${NC}"
    echo -e "${RED}Crashes/Deadlocks: $CRASHED_TESTS${NC}"
    echo ""
    
    # Show per-queue success rate
    echo "Per-queue success rate:"
    echo "----------------------------------------"
    
    for queue in "${QUEUES[@]}"; do
        local queue_total=0
        local queue_passed=0
        
        while IFS=',' read -r q p c t ep dp r d tf s; do
            if [ "$q" = "$queue" ]; then
                queue_total=$((queue_total + 1))
                if [ "$s" = "1" ]; then
                    queue_passed=$((queue_passed + 1))
                fi
            fi
        done < "$RESULTS_CSV"
        
        if [ $queue_total -gt 0 ]; then
            local success_rate=$((queue_passed * 100 / queue_total))
            if [ $success_rate -eq 100 ]; then
                echo -e "  ${GREEN}$queue: $queue_passed/$queue_total (${success_rate}%)${NC}"
            elif [ $success_rate -ge 50 ]; then
                echo -e "  ${YELLOW}$queue: $queue_passed/$queue_total (${success_rate}%)${NC}"
            else
                echo -e "  ${RED}$queue: $queue_passed/$queue_total (${success_rate}%)${NC}"
            fi
        fi
    done
    
    echo "----------------------------------------"
}

# Main execution
echo -e "${GREEN}╔════════════════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║         MPMC Queue Benchmark Suite with LTTng                  ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════════════════════════════╝${NC}"
echo ""
echo "Configuration:"
echo "  Max retries per config: $MAX_RETRIES"
echo "  Timeout: ${TIMEOUT_SECONDS}s"
echo "  LTTng tracing: $([ $LTTNG_ENABLED -eq 1 ] && echo "Enabled" || echo "Disabled")"
echo "  Items per producer: $ITEMS"
echo "  Queue capacity: $CAPACITY"
echo ""

for queue in "${QUEUES[@]}"; do
    print_header "$queue"
    
    for producers in "${PRODUCER_CONFIGS[@]}"; do
        for consumers in "${CONSUMER_CONFIGS[@]}"; do
            printf "P=%-2d C=%-2d : " "$producers" "$consumers"
            run_benchmark "$queue" "$producers" "$consumers"
        done
    done
done

print_final_summary

echo -e "\n${GREEN}Benchmark Complete!${NC}"
echo "Results saved in:"
echo "  Data: $DATA_DIR/"
echo "  Traces: $TRACES_DIR/"
echo "  Logs: $LOGS_DIR/"
echo ""
echo "Run analysis: python3 scripts/analyze_results.py"
echo "Analyze LTTng traces: python3 scripts/analyze_lttng.py --trace-dir $TRACES_DIR"

# Exit with appropriate code
if [ $FAILED_TESTS -gt 0 ]; then
    exit 1
else
    exit 0
fi
