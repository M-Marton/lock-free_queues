# MPMC Queue Benchmark Suite

Comprehensive benchmarking of MPMC queue implementations with LTTng tracing, deadlock detection, and automatic retry.

## Implemented Queues

| Queue | Type | Description |
|-------|------|-------------|
| mutex | Blocking | Baseline with std::mutex |
| twolock | Lock-free | Michael-Scott algorithm |
| twolock_hazard | Lock-free | Michael-Scott with hazard pointers |
| ringbuffer | Lock-free | Bounded ring buffer |
| scq | Lock-free | Scalable Circular Queue (fetch-and-add) |
| disruptor | Lock-free | LMAX Disruptor pattern |

## Dependencies

sudo apt install build-essential cmake git lttng-tools liblttng-ust-dev doxygen graphviz python3 python3-pip python3-pandas python3-matplotlib python3-numpy python3-seaborn



## Build

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
