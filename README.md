# MPMC Queue Benchmark with LTTng Tracing

Full performance measurement using LTTng-UST tracepoints.

## Implemented Queues

| Queue | Type | Description |
|-------|------|-------------|
| `mutex` | Blocking | Baseline with std::mutex |
| `ringbuffer` | Lock-free | CAS-based circular buffer |
| `bounded` | Lock-free | Fetch-and-add based MPMC queue |

## Why Only These Three?

- **Mutex**: Baseline comparison (shows why lock-free matters)
- **RingBuffer**: Proven lock-free implementation, cache-friendly
- **BoundedMPMC**: Simple, deadlock-free, good scalability

## Dependencies

```
sudo apt install build-essential git lttng-tools liblttng-ust-dev python3 python3-pip python3-pandas python3-matplotlib python3-numpy python3-seaborn parallel
```

## Build & Run

```bash
# Run full benchmark (tests 1,2,4,8,12 cores)
sudo ./run.sh

# Analyze results
python3 analyze.py
