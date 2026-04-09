#!/bin/bash
# MPMC Benchmark - Weighted parallel execution with CPU core budget

set -u

ORIGINAL_USER=${SUDO_USER:-$USER}
RESULTS_DIR="./results"
TRACES_DIR="$RESULTS_DIR/traces"
DATA_DIR="$RESULTS_DIR/data"
mkdir -p "$TRACES_DIR" "$DATA_DIR"

# Check LTTng tools
if ! command -v lttng &> /dev/null; then
    echo "ERROR: lttng not installed. Please install lttng-tools and lttng-ust."
    exit 1
fi

# Build
echo "Building benchmark with LTTng..."
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
cd ..

# Configuration matrix
QUEUES=("mutex" "ringbuffer" "bounded" "hazard")
CORES=(1 2 4 8 12 15)
RATIOS=(
    "1 1" "1 4" "1 10"
    "4 1" "4 4" "4 10"
    "10 1" "10 4" "8 8"
    "16 16"
)
ITEMS_LIST=(50000 500000)
CAPACITY_LIST=(1000 100000)

TIMEOUT=180
RUNS_PER_CONFIG=5
CPU_BUDGET=13          # Maximum total cores used concurrently

# Summary CSV
SUMMARY_CSV="$DATA_DIR/summary_lttng.csv"
echo "queue,cores,producers,consumers,items,capacity,run,throughput_mops,enqueue_p99_ns,dequeue_p99_ns" > "$SUMMARY_CSV"

# Temp directory for per-run results
TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

# Function to run a single configuration (background)
run_config() {
    local queue=$1
    local cores=$2
    local prod=$3
    local cons=$4
    local items=$5
    local cap=$6
    local run=$7
    local tmp_out=$8
    local job_id=$9

    local session="bench_${queue}_p${prod}c${cons}_i${items}_s${cap}_c${cores}_run${run}"
    local trace_dir="$TRACES_DIR/$session"
    local output="$DATA_DIR/${session}.log"

    # Create LTTng session
    lttng create "$session" --output="$trace_dir" > /dev/null 2>&1
    lttng enable-event --userspace mpmc_benchmark:queue_op > /dev/null 2>&1
    lttng add-context --userspace -t vtid -t vpid -t procname > /dev/null 2>&1
    lttng start > /dev/null 2>&1

    # Run with cgroup CPU limit
    local cgroup_name="mpmc_$$_${queue}_c${cores}_r${run}"
    sudo mkdir -p "/sys/fs/cgroup/$cgroup_name" 2>/dev/null
    echo "$((cores * 100000)) 100000" | sudo tee "/sys/fs/cgroup/$cgroup_name/cpu.max" > /dev/null

    if [ "$queue" == "hazard" ]; then
        sudo cgexec -g cpu:"$cgroup_name" timeout $TIMEOUT ./build/benchmark \
            -t "$queue" -p "$prod" -c "$cons" -i "$items" -s 0 \
            > "$output" 2>&1
    else
        sudo cgexec -g cpu:"$cgroup_name" timeout $TIMEOUT ./build/benchmark \
            -t "$queue" -p "$prod" -c "$cons" -i "$items" -s "$cap" \
            > "$output" 2>&1
    fi

    local exit_code=$?
    sudo rmdir "/sys/fs/cgroup/$cgroup_name" 2>/dev/null

    lttng stop > /dev/null 2>&1
    lttng destroy > /dev/null 2>&1

    if [ $exit_code -eq 0 ]; then
        throughput=$(grep "Throughput:" "$output" | head -1 | awk '{print $2}')
        if [[ "$throughput" =~ ^[0-9]+\.?[0-9]*$ ]]; then
            echo "$queue,$cores,$prod,$cons,$items,$cap,$run,$throughput,0,0" >> "$tmp_out"
            echo "OK"
        else
            echo "FAIL"
        fi
    else
        echo "FAIL"
    fi
}

export -f run_config
export TIMEOUT CPU_BUDGET TRACES_DIR DATA_DIR

# Generate all configurations as an array of strings
configs=()
for queue in "${QUEUES[@]}"; do
    for cores in "${CORES[@]}"; do
        for ratio in "${RATIOS[@]}"; do
            prod=$(echo $ratio | cut -d' ' -f1)
            cons=$(echo $ratio | cut -d' ' -f2)
            for items in "${ITEMS_LIST[@]}"; do
                for cap in "${CAPACITY_LIST[@]}"; do
                    for run in $(seq 1 $RUNS_PER_CONFIG); do
                        configs+=("$queue|$cores|$prod|$cons|$items|$cap|$run")
                    done
                done
            done
        done
    done
done

total=${#configs[@]}
echo "Total test cases: $total"
echo "CPU budget: $CPU_BUDGET cores (sum of per-benchmark core limits)"
echo ""

# Weighted job scheduler
running_pids=()
running_cores=()
tmp_results="$TMP_DIR/results.tmp"
touch "$tmp_results"

completed=0
next_config_index=0

# Function to start a new job if budget allows
start_job() {
    local config="${configs[$next_config_index]}"
    IFS='|' read -r queue cores prod cons items cap run <<< "$config"
    # Calculate current total cores
    local total_cores=0
    for c in "${running_cores[@]}"; do
        total_cores=$((total_cores + c))
    done
    # Check if adding this job would exceed budget (unless it's the only job and its cores > budget)
    if [ $((total_cores + cores)) -le $CPU_BUDGET ] || [ ${#running_pids[@]} -eq 0 ]; then
        # Start job
        local tmp_out="$TMP_DIR/out_${queue}_${cores}_${prod}_${cons}_${items}_${cap}_${run}"
        run_config "$queue" "$cores" "$prod" "$cons" "$items" "$cap" "$run" "$tmp_results" "$next_config_index" &
        local pid=$!
        running_pids+=($pid)
        running_cores+=($cores)
        echo "Started: $queue cores=$cores P=$prod C=$cons (budget: $((total_cores + cores))/$CPU_BUDGET)"
        next_config_index=$((next_config_index + 1))
        return 0
    else
        return 1
    fi
}

# Main scheduling loop
while [ $completed -lt $total ]; do
    # Try to start new jobs
    while [ $next_config_index -lt $total ]; do
        if start_job; then
            continue
        else
            break
        fi
    done

    # Wait for any job to finish
    if [ ${#running_pids[@]} -gt 0 ]; then
        wait -n
        # Remove finished jobs from arrays (no local, just reassign)
        new_pids=()
        new_cores=()
        for i in "${!running_pids[@]}"; do
            if kill -0 "${running_pids[$i]}" 2>/dev/null; then
                new_pids+=("${running_pids[$i]}")
                new_cores+=("${running_cores[$i]}")
            else
                completed=$((completed + 1))
                echo "Completed: $completed / $total"
            fi
        done
        running_pids=("${new_pids[@]}")
        running_cores=("${new_cores[@]}")
    else
        # No running jobs, but still configs left? Should not happen
        break
    fi
done

# Combine results into summary CSV
cat "$tmp_results" >> "$SUMMARY_CSV"

# Compute and display summary statistics
echo ""
echo "========================================="
echo "Summary statistics (averaged over runs):"
echo "========================================="

awk -F',' '
NR>1 {
    key = $1","$2","$3","$4","$5","$6
    sum_throughput[key] += $8
    count[key]++
}
END {
    for (key in sum_throughput) {
        avg = sum_throughput[key] / count[key]
        printf "%-40s avg throughput = %.2f M ops/sec (based on %d runs)\n", key, avg, count[key]
    }
}' "$SUMMARY_CSV" | sort

echo ""
echo "Relative performance (vs ringbuffer) at max cores (15):"
awk -F',' '
NR>1 && $2==15 && $1!="ringbuffer" {
    key = $1","$3","$4","$5","$6
    sum[$1] += $8; cnt[$1]++
}
END {
    for (q in sum) {
        avg[q] = sum[q]/cnt[q]
    }
    baseline = avg["ringbuffer"]
    for (q in avg) {
        if (q != "ringbuffer") printf "%s: %.2fx\n", q, avg[q]/baseline
    }
}' "$SUMMARY_CSV"

echo ""
echo "Benchmark finished. Traces saved in $TRACES_DIR"
echo "Run python3 analyze_lttng.py for detailed plots and statistics."
