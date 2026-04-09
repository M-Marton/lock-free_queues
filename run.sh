#!/bin/bash
# MPMC Benchmark - Full matrix, PID file via -f argument

set -u

RESULTS_DIR="./results"
TRACES_DIR="$RESULTS_DIR/traces"
mkdir -p "$TRACES_DIR"

# Build
echo "Building benchmark with LTTng..."
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..

# Configuration matrix
QUEUES=("mutex" "ringbuffer" "bounded" "hazard")
CORES=(1 2 4 8 15)
RATIOS=(
    "1 1" "1 4" "1 10"
    "4 1" "10 1"
    "4 4" "8 8"
    "16 16"
)
ITEMS_LIST=(100000)
CAPACITY_LIST=(1000 100000)

TIMEOUT=180
RUNS_PER_CONFIG=3

# Create unique session
SESSION_NAME="mpmc_sequential_$$"
TRACE_PATH="$TRACES_DIR/$SESSION_NAME"
mkdir -p "$TRACE_PATH"

# CSV inside trace directory
SUMMARY_CSV="$TRACE_PATH/summary.csv"
echo "queue,cores,producers,consumers,items,capacity,run,pid,throughput_mops" > "$SUMMARY_CSV"

# Pre‑compute total test cases
total_configs=0
for queue in "${QUEUES[@]}"; do
    for cores in "${CORES[@]}"; do
        for ratio in "${RATIOS[@]}"; do
            for items in "${ITEMS_LIST[@]}"; do
                for cap in "${CAPACITY_LIST[@]}"; do
                    for run in $(seq 1 $RUNS_PER_CONFIG); do
                        total_configs=$((total_configs + 1))
                    done
                done
            done
        done
    done
done

echo "Total test cases: $total_configs"
echo ""

# Setup LTTng session
lttng create "$SESSION_NAME" --output="$TRACE_PATH" > /dev/null 2>&1
lttng enable-event --userspace mpmc_benchmark:queue_op > /dev/null 2>&1
lttng add-context --userspace -t vtid -t vpid -t procname > /dev/null 2>&1
lttng start > /dev/null 2>&1

# Run sequentially
current=0
start_global=$(date +%s)

for queue in "${QUEUES[@]}"; do
    for cores in "${CORES[@]}"; do
        for ratio in "${RATIOS[@]}"; do
            prod=$(echo $ratio | cut -d' ' -f1)
            cons=$(echo $ratio | cut -d' ' -f2)
            for items in "${ITEMS_LIST[@]}"; do
                for cap in "${CAPACITY_LIST[@]}"; do
                    for run in $(seq 1 $RUNS_PER_CONFIG); do
                        current=$((current + 1))
                        echo "[$current/$total_configs] $queue cores=$cores P=$prod C=$cons items=$items cap=$cap run=$run"

                        # Create cgroup
                        cgroup_name="mpmc_$$_${queue}_c${cores}_r${run}"
                        sudo mkdir -p "/sys/fs/cgroup/$cgroup_name" 2>/dev/null
                        echo "$((cores * 100000)) 100000" | sudo tee "/sys/fs/cgroup/$cgroup_name/cpu.max" > /dev/null

                        output="$TRACE_PATH/${queue}_p${prod}c${cons}_i${items}_s${cap}_c${cores}_run${run}.log"
                        pidfile="$TRACE_PATH/${queue}_p${prod}c${cons}_i${items}_s${cap}_c${cores}_run${run}.pid"

                        # Run benchmark with PID file argument
                        sudo cgexec -g cpu:"$cgroup_name" timeout $TIMEOUT ./build/benchmark \
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

                        wait $bench_pid 2>/dev/null

                        throughput=$(grep "Throughput:" "$output" | head -1 | awk '{print $2}')
                        if [[ "$throughput" =~ ^[0-9]+\.?[0-9]*$ ]]; then
                            if [ -n "$real_pid" ]; then
                                echo "$queue,$cores,$prod,$cons,$items,$cap,$run,$real_pid,$throughput" >> "$SUMMARY_CSV"
                                echo "  ✓ PID: $real_pid, throughput: $throughput M ops/sec"
                            else
                                echo "  ⚠ WARNING: No PID, throughput: $throughput"
                                echo "$queue,$cores,$prod,$cons,$items,$cap,$run,0,$throughput" >> "$SUMMARY_CSV"
                            fi
                        else
                            echo "  ✗ FAILED"
                        fi

                        # Cleanup
                        rm -f "$pidfile"
                        sudo rmdir "/sys/fs/cgroup/$cgroup_name" 2>/dev/null
                    done
                done
            done
        done
    done
done

# Stop LTTng session
lttng stop "$SESSION_NAME" > /dev/null 2>&1
lttng destroy "$SESSION_NAME" > /dev/null 2>&1

echo ""
echo "Benchmark finished."
echo "Trace saved in: $TRACE_PATH"
echo "Summary CSV: $SUMMARY_CSV"
