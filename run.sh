#!/bin/bash
# run.sh v2.0 - MPMC Benchmark Runner with LTTng cleanup and prevention
# Changes: Added trap for cleanup, LTTng session protection, timeout handling

set -u

# ============================================================================
# Cleanup function - ensures LTTng sessions are always destroyed
# ============================================================================
cleanup() {
    echo ""
    echo "Cleaning up LTTng sessions and processes..."
    sudo lttng destroy --all 2>/dev/null
    sudo pkill -9 lttng-consumerd 2>/dev/null
    sudo pkill -9 lttng-sessiond 2>/dev/null
    echo "Cleanup complete."
    exit
}

# Set trap for various exit signals
trap cleanup EXIT INT TERM HUP

# ============================================================================
# Start fresh LTTng daemon
# ============================================================================
echo "Starting fresh LTTng daemon..."
sudo lttng destroy --all 2>/dev/null
sudo pkill -9 lttng-consumerd 2>/dev/null
sudo pkill -9 lttng-sessiond 2>/dev/null
sleep 1
sudo lttng-sessiond --daemonize
sleep 1
echo "LTTng daemon ready."

# ============================================================================
# Configuration
# ============================================================================

RESULTS_DIR="./results"
TRACES_DIR="$RESULTS_DIR/traces"
DATA_DIR="$RESULTS_DIR/data"
mkdir -p "$TRACES_DIR" "$DATA_DIR"

# Build
echo "Building benchmark with LTTng..."
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..

# Test configurations: producers, consumers, items, capacity
# Simplified matrix for meaningful results
CONFIGS=(
    # Balanced tests with tight capacity
    "8 8 1000000 10"
    "8 8 1000000 50"
    "8 8 1000000 100"
    "8 8 1000000 500"
    "8 8 1000000 1000"
    
    # Unbalanced tests (tight capacity)
    "14 2 1000000 50"
    "2 14 1000000 50"
    "15 1 1000000 50"
    "1 15 1000000 50"
)

# Queue types
QUEUES=("mutex" "ringbuffer" "hazard")

TIMEOUT=600  # 10 minutes timeout per test (for 10M items)
RUNS_PER_CONFIG=10

# Use all available cores (no cgroup limiting)
# Just run on the system with full hardware capability
CORES_SETTING="full"

# Pre‑compute total test cases
total_configs=0
for queue in "${QUEUES[@]}"; do
    for config in "${CONFIGS[@]}"; do
        for run in $(seq 1 $RUNS_PER_CONFIG); do
            total_configs=$((total_configs + 1))
        done
    done
done

echo "Total test cases: $total_configs"
echo "CPU cores: all available (no cgroup limiting)"
echo "Timeout per test: ${TIMEOUT}s"
echo ""

# ============================================================================
# Setup LTTng session (single session for all runs)
# ============================================================================
SESSION_NAME="mpmc_sequential_$$"
TRACE_PATH="$TRACES_DIR/$SESSION_NAME"
mkdir -p "$TRACE_PATH"

SUMMARY_CSV="$TRACE_PATH/summary.csv"
echo "queue,cores,producers,consumers,items,capacity,run,pid,throughput_mops" > "$SUMMARY_CSV"


echo "Creating LTTng session: $SESSION_NAME (output: $TRACE_PATH)"
lttng create "$SESSION_NAME" --output="$TRACE_PATH" > /dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "ERROR: Failed to create LTTng session"
    exit 1
fi

lttng enable-event --userspace mpmc_benchmark:queue_op > /dev/null 2>&1
lttng add-context --userspace -t vtid -t vpid -t procname > /dev/null 2>&1
lttng start > /dev/null 2>&1
echo "LTTng tracing started."

# ============================================================================
# Run benchmarks sequentially
# ============================================================================
current=0
start_global=$(date +%s)

for queue in "${QUEUES[@]}"; do
    for config in "${CONFIGS[@]}"; do
        read -r prod cons items cap <<< "$config"
        for run in $(seq 1 $RUNS_PER_CONFIG); do
            current=$((current + 1))
            
            echo "[$current/$total_configs] $queue P=$prod C=$cons items=$items cap=$cap run=$run"

            output="$DATA_DIR/${queue}_p${prod}c${cons}_i${items}_s${cap}_run${run}.log"
            pidfile="$DATA_DIR/${queue}_p${prod}c${cons}_i${items}_s${cap}_run${run}.pid"

            # Remove old PID file
            rm -f "$pidfile"

            # Run benchmark with timeout (no cgroups, uses all cores)
            timeout $TIMEOUT ./build/benchmark \
                -t "$queue" -p "$prod" -c "$cons" -i "$items" -s "$cap" -f "$pidfile" \
                > "$output" 2>&1 &
            bench_pid=$!

            # Wait for PID file (max 5 seconds)
            real_pid=""
            for i in {1..50}; do
                if [ -f "$pidfile" ]; then
                    real_pid=$(cat "$pidfile")
                    break
                fi
                sleep 0.1
            done

            # Wait for benchmark to finish (or timeout)
            wait $bench_pid 2>/dev/null
            exit_code=$?

            # Extract throughput from output
            throughput=$(grep "Throughput:" "$output" | head -1 | awk '{print $2}')
            
            if [[ "$throughput" =~ ^[0-9]+\.?[0-9]*$ ]]; then
                if [ -n "$real_pid" ]; then
                    echo "$queue,$CORES_SETTING,$prod,$cons,$items,$cap,$run,$real_pid,$throughput" >> "$SUMMARY_CSV"
                    echo "  ✓ PID: $real_pid, throughput: $throughput M ops/sec, exit_code: $exit_code"
                else
                    echo "  ⚠ WARNING: No PID, throughput: $throughput"
                    echo "$queue,$CORES_SETTING,$prod,$cons,$items,$cap,$run,0,$throughput" >> "$SUMMARY_CSV"
                fi
            else
                echo "  ✗ FAILED (exit_code: $exit_code)"
                # Check if it was a timeout
                if [ $exit_code -eq 124 ]; then
                    echo "    → Timeout after ${TIMEOUT}s"
                fi
            fi

            # Cleanup PID file
            rm -f "$pidfile"
        done
    done
done

# ============================================================================
# Stop LTTng session and cleanup
# ============================================================================
echo ""
echo "Stopping LTTng session..."
lttng stop "$SESSION_NAME" > /dev/null 2>&1
lttng destroy "$SESSION_NAME" > /dev/null 2>&1

# Summary statistics
echo ""
echo "========================================="
echo "Summary statistics (averaged over runs):"
echo "========================================="

awk -F',' '
NR>1 {
    key = $1","$3","$4","$5","$6
    sum_throughput[key] += $9
    count[key]++
}
END {
    for (key in sum_throughput) {
        avg = sum_throughput[key] / count[key]
        printf "%-50s avg throughput = %.4f M ops/sec (based on %d runs)\n", key, avg, count[key]
    }
}' "$SUMMARY_CSV" | sort

end_global=$(date +%s)
total_duration=$((end_global - start_global))
echo ""
echo "Benchmark finished in $((total_duration/60)) minutes and $((total_duration%60)) seconds."
echo "Trace saved in: $TRACE_PATH"
echo "Summary CSV: $SUMMARY_CSV"

# ============================================================================
# Cleanup is automatically called by trap
# ============================================================================
echo ""
echo "Run completed successfully."

# ============================================================================
# Run analyze_lttng.py
# ===========================================================================

python3 ./analyze_lttng.py --trace-dir=$TRACE_PATH --output-dir=./results/analysis_$SESSION_NAME
