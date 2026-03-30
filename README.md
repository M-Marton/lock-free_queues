# MPMC Queue Benchmarking Project

## Overview

This project implements and benchmarks various Multi-Producer Multi-Consumer (MPMC) queue implementations on Linux, with integrated LTTng tracing for detailed performance analysis.

## Implemented Queues

| Queue | Type | Locking | Characteristics |
|-------|------|---------|-----------------|
| `MutexMPMCQueue` | Baseline | Mutex + Condition Variables | Blocking, simple |
| `TwoLockMPMCQueue` | Lock-free | Michael-Scott algorithm | Unbounded, classic lock-free |
| `BoundedRingBufferMPMC` | Lock-free | Atomic indices with ring buffer | Bounded, high performance |

## Features

- ✅ Multiple MPMC queue implementations
- ✅ Configurable producer/consumer thread counts
- ✅ Detailed latency statistics (min, max, avg, p50, p99, p99.9)
- ✅ LTTng-UST tracepoints for performance analysis
- ✅ Doxygen documentation
- ✅ Benchmark automation scripts
- ✅ Result analysis with Python

## Prerequisites

### Ubuntu/Debian
```bash
sudo apt update
sudo apt install build-essential cmake git
sudo apt install lttng-tools lttng-ust liblttng-ust-dev
sudo apt install doxygen graphviz
sudo apt install python3 python3-pip
pip3 install pandas matplotlib numpy
