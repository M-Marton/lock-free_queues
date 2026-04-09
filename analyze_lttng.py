#!/usr/bin/env python3
"""
Streaming LTTng trace analyzer – reads summary.csv from trace directory.
"""

import sys
import argparse
import subprocess
import re
import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path
from collections import defaultdict

def stream_trace_events(trace_dir):
    """Generator yielding (vpid, op, latency_ns)."""
    try:
        proc = subprocess.Popen(
            ['babeltrace', str(trace_dir)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1
        )
        for line in proc.stdout:
            if 'mpmc_benchmark:queue_op' not in line:
                continue
            pid_match = re.search(r'vpid\s*=\s*(\d+)', line)
            if not pid_match:
                continue
            vpid = int(pid_match.group(1))
            match = re.search(r'operation = "([^"]+)".*latency_ns = (\d+)', line)
            if match:
                yield vpid, match.group(1), int(match.group(2))
        proc.terminate()
    except Exception as e:
        print(f"Error: {e}")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--trace-dir', required=True)
    parser.add_argument('--output-dir', default='./results/analysis')
    args = parser.parse_args()

    trace_dir = Path(args.trace_dir)
    summary_csv = trace_dir / 'summary.csv'
    if not summary_csv.exists():
        print(f"Error: summary.csv not found in {trace_dir}")
        sys.exit(1)

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    summary = pd.read_csv(summary_csv)
    pid_to_idx = {row['pid']: i for i, row in summary.iterrows() if row['pid'] != 0}
    print(f"Loaded {len(summary)} runs, {len(pid_to_idx)} with valid PID")

    enq_samples = defaultdict(list)
    deq_samples = defaultdict(list)

    print("Streaming trace events...")
    total = matched = 0
    for vpid, op, lat in stream_trace_events(trace_dir):
        total += 1
        if vpid in pid_to_idx:
            matched += 1
            idx = pid_to_idx[vpid]
            if op == 'enqueue':
                enq_samples[idx].append(lat)
            else:
                deq_samples[idx].append(lat)

    print(f"Trace events: {total}, matched: {matched}")

    results = []
    for i, row in summary.iterrows():
        enq = enq_samples.get(i, [])
        deq = deq_samples.get(i, [])
        if enq:
            results.append({
                'queue': row['queue'],
                'cores': row['cores'],
                'producers': row['producers'],
                'consumers': row['consumers'],
                'items': row['items'],
                'capacity': row['capacity'],
                'run': row['run'],
                'throughput_mops': row['throughput_mops'],
                'enqueue_p99_ns': np.percentile(enq, 99),
                'dequeue_p99_ns': np.percentile(deq, 99) if deq else 0,
            })

    if not results:
        print("No events matched. Check PID alignment.")
        sys.exit(1)

    df = pd.DataFrame(results)
    df.to_csv(output_dir / 'enriched.csv', index=False)
    print(f"Saved {len(df)} runs with latency data.")

    # Throughput comparison
    plt.figure()
    for q in df['queue'].unique():
        sub = df[df['queue'] == q].groupby('cores')['throughput_mops'].mean()
        if not sub.empty:
            plt.plot(sub.index, sub.values, marker='o', label=q.upper())
    plt.xlabel('CPU Cores')
    plt.ylabel('Throughput (M ops/sec)')
    plt.title('Throughput Comparison')
    plt.legend()
    plt.grid(True)
    plt.savefig(output_dir / 'throughput_comparison.png', dpi=150)
    plt.close()

    # Latency at max cores
    max_core = df['cores'].max()
    high_core = df[df['cores'] == max_core]
    if not high_core.empty:
        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
        enq_means = high_core.groupby('queue')['enqueue_p99_ns'].mean()
        deq_means = high_core.groupby('queue')['dequeue_p99_ns'].mean()
        ax1.bar(enq_means.index, enq_means.values/1000, color='steelblue')
        ax1.set_ylabel('P99 Enqueue Latency (μs)')
        ax1.set_title(f'Enqueue at {max_core} cores')
        ax2.bar(deq_means.index, deq_means.values/1000, color='coral')
        ax2.set_ylabel('P99 Dequeue Latency (μs)')
        ax2.set_title(f'Dequeue at {max_core} cores')
        plt.tight_layout()
        plt.savefig(output_dir / 'latency_comparison.png', dpi=150)
        plt.close()

    print(f"Plots saved in {output_dir}")

if __name__ == '__main__':
    main()
