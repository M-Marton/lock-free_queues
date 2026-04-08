#!/usr/bin/env python3
"""
Analyze LTTng traces from the MPMC benchmark.
Computes throughput, latency distributions, and generates plots.
"""

import os
import sys
import subprocess
import json
import re
import numpy as np
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from pathlib import Path
from collections import defaultdict

def parse_trace(trace_dir):
    """Run babeltrace and extract events."""
    events = []
    try:
        result = subprocess.run(
            ['babeltrace', str(trace_dir)],
            capture_output=True, text=True, timeout=60
        )
        if result.returncode != 0:
            print(f"Error running babeltrace on {trace_dir}: {result.stderr}")
            return events
        for line in result.stdout.split('\n'):
            if 'mpmc_benchmark:queue_op' in line:
                # Extract fields using regex
                match = re.search(r'queue_name = "([^"]+)".*operation = "([^"]+)".*thread_id = (\d+).*latency_ns = (\d+)', line)
                if match:
                    events.append({
                        'queue': match.group(1),
                        'op': match.group(2),
                        'thread': int(match.group(3)),
                        'latency_ns': int(match.group(4))
                    })
    except subprocess.TimeoutExpired:
        print(f"Timeout processing {trace_dir}")
    except FileNotFoundError:
        print("babeltrace not found. Install lttng-tools package.")
    return events

def process_all_traces(traces_root):
    """Walk through all trace directories and compute per‑configuration statistics."""
    traces_root = Path(traces_root)
    results = []
    for trace_dir in traces_root.iterdir():
        if not trace_dir.is_dir():
            continue
        # Parse directory name: bench_{queue}_p{prod}c{cons}_i{items}_s{cap}_c{cores}_run{run}
        parts = trace_dir.name.split('_')
        if len(parts) < 10:
            continue
        try:
            queue = parts[1]
            prod = int(parts[2][1:])
            cons = int(parts[3][1:])
            items = int(parts[4][1:])
            cap = int(parts[5][1:])
            cores = int(parts[7][1:])
            run = int(parts[9][1:]) if parts[9].startswith('run') else 0
        except (IndexError, ValueError):
            continue

        events = parse_trace(trace_dir)
        if not events:
            continue

        enq_lat = [e['latency_ns'] for e in events if e['op'] == 'enqueue']
        deq_lat = [e['latency_ns'] for e in events if e['op'] == 'dequeue']

        if enq_lat:
            results.append({
                'queue': queue,
                'cores': cores,
                'producers': prod,
                'consumers': cons,
                'items': items,
                'capacity': cap,
                'run': run,
                'throughput_mops': len(enq_lat) / (max(1, (max(e.get('timestamp',0) for e in events) - min(e.get('timestamp',0) for e in events)) / 1e9) if events else 0,
                'enqueue_p99_ns': np.percentile(enq_lat, 99),
                'dequeue_p99_ns': np.percentile(deq_lat, 99),
                'enqueue_mean_ns': np.mean(enq_lat),
                'dequeue_mean_ns': np.mean(deq_lat),
            })
    return pd.DataFrame(results)

def plot_throughput_vs_cores(df, out_dir):
    plt.figure(figsize=(10, 6))
    for queue in df['queue'].unique():
        qdf = df[df['queue'] == queue].groupby('cores')['throughput_mops'].mean().reset_index()
        plt.plot(qdf['cores'], qdf['throughput_mops'], marker='o', label=queue.upper())
    plt.xlabel('CPU cores')
    plt.ylabel('Throughput (M ops/sec)')
    plt.title('Throughput vs cores (averaged over runs)')
    plt.legend()
    plt.grid(True)
    plt.savefig(out_dir / 'throughput_vs_cores.png', dpi=150)
    plt.close()

def plot_latency_heatmap(df, out_dir):
    # For a fixed core count (e.g., 15), show latency as heatmap over producers/consumers
    high_core = df[df['cores'] == df['cores'].max()]
    pivot_enq = high_core.pivot_table(index='producers', columns='consumers', values='enqueue_p99_ns', aggfunc='mean')
    if not pivot_enq.empty:
        plt.figure(figsize=(10, 8))
        sns.heatmap(pivot_enq, annot=True, fmt='.0f', cmap='Reds')
        plt.title('P99 Enqueue Latency (ns) at max cores')
        plt.savefig(out_dir / 'latency_heatmap_enqueue.png', dpi=150)
        plt.close()

def generate_report(df, out_dir):
    # Summary statistics per queue
    summary = df.groupby('queue').agg({
        'throughput_mops': ['mean', 'std'],
        'enqueue_p99_ns': ['mean', 'std'],
        'dequeue_p99_ns': ['mean', 'std']
    }).round(2)
    summary.to_csv(out_dir / 'summary_lttng.csv')
    with open(out_dir / 'report.txt', 'w') as f:
        f.write("MPMC Benchmark Results (from LTTng traces)\n")
        f.write("==========================================\n\n")
        f.write(summary.to_string())
    print(f"Report saved to {out_dir}/report.txt")

def main():
    if len(sys.argv) > 1:
        traces_root = sys.argv[1]
    else:
        traces_root = './results/traces'
    out_dir = Path('./results/analysis')
    out_dir.mkdir(parents=True, exist_ok=True)

    print("Processing LTTng traces...")
    df = process_all_traces(traces_root)
    if df.empty:
        print("No trace events found. Run the benchmark first.")
        return

    print(f"Collected {len(df)} configuration runs.")
    plot_throughput_vs_cores(df, out_dir)
    plot_latency_heatmap(df, out_dir)
    generate_report(df, out_dir)
    print(f"Plots and report saved to {out_dir}")

if __name__ == '__main__':
    main()
