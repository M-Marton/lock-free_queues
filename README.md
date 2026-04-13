# MPMC Queue Benchmark with LTTng Tracing

Performance measurement using LTTng-UST tracepoints.

- **run.sh** build and run benchmark
- build/**benchmark**
- **analyze_lttng.py** lttng ctf postprocessing and result visualization

## Implemented Queues

| Queue | Type | Description |
|-------|------|-------------|
| `mutex` | Blocking | Baseline with std::mutex |
| `ringbuffer` | Lock-free | CAS-based circular buffer |
| `TwoLockHazardPointer` | Lock-free | Michael-Scott unbounded MPMC queue with Hazard Pointers |

## Why Only These Three?

- **Mutex**: Baseline comparison (shows why lock-free matters)
- **RingBuffer**: Proven lock-free implementation, cache-friendly
- **TwoLockHazardPointers**: Unbounded, lock-free, solved ABA problem

## Dependencies

```
sudo apt install build-essential lttng-tools liblttng-ust-dev python3 python3-pip python3-pandas python3-matplotlib python3-numpy python3-seaborn parallel
```
## Benchmark parameters

```
Usage: ./build/benchmark [options]
  -t TYPE     mutex | ringbuffer | bounded | hazard
  -p NUM      Producers (default: 4)
  -c NUM      Consumers (default: 4)
  -i NUM      Items per producer (default: 1000000)
  -s NUM      Queue capacity (default: 100000, ignored for hazard)
  -f FILE     Write PID to FILE (optional)
  -h          Help

```
## run.sh parameters

```
#Set up config matrix for benchmark automation
CONFIGS="producers consumers items capacity"

#Increasing statistic accuracy
RUNS_PER_CONFIG=10

```


## Build & Run

```bash
# Run full benchmark
sudo ./run.sh

# Analyze results
sudo python3 sudo ./analyze_lttng.py --trace-dir=./results/traces/*
