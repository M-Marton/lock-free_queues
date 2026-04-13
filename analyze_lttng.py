#!/usr/bin/env python3
"""
MPMC Queue Benchmark Analysis Script v2.3
Memory-efficient: aggregates percentiles on the fly without storing all events.
Uses streaming approximation for P99/P95/P90/P50 latencies.
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
import heapq

class StreamingPercentile:
    """Reservoir sampling based percentile estimator (memory efficient)."""
    
    def __init__(self, max_samples=10000):
        self.max_samples = max_samples
        self.samples = []
        self.count = 0
        self.rng = np.random.RandomState(42)
    
    def add(self, value):
        self.count += 1
        if len(self.samples) < self.max_samples:
            self.samples.append(value)
        else:
            # Reservoir sampling: replace random element with probability max_samples/count
            idx = self.rng.randint(0, self.count)
            if idx < self.max_samples:
                self.samples[idx] = value
    
    def percentile(self, p):
        if not self.samples:
            return 0
        sorted_samples = sorted(self.samples)
        idx = int(len(sorted_samples) * p / 100)
        return sorted_samples[min(idx, len(sorted_samples) - 1)]
    
    def clear(self):
        self.samples = []
        self.count = 0

def detect_delimiter(filepath):
    """Detect the delimiter of a CSV file."""
    with open(filepath, 'r') as f:
        first_line = f.readline()
        if '\t' in first_line:
            return '\t'
        elif ',' in first_line:
            return ','
        else:
            return None

def stream_trace_events(trace_dir):
    """Generator yielding (vpid, queue, op, latency_ns) - no timestamp to save memory."""
    try:
        proc = subprocess.Popen(
            ['babeltrace', str(trace_dir)],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            bufsize=65536
        )
        
        buffer = ""
        while True:
            chunk = proc.stdout.read(65536)
            if not chunk:
                break
            
            try:
                text = chunk.decode('utf-8', errors='replace')
            except UnicodeDecodeError:
                text = chunk.decode('latin-1')
            
            buffer += text
            lines = buffer.split('\n')
            buffer = lines[-1]
            
            for line in lines[:-1]:
                if 'mpmc_benchmark:queue_op' not in line:
                    continue
                
                pid_match = re.search(r'vpid\s*=\s*(\d+)', line)
                vpid = int(pid_match.group(1)) if pid_match else 0
                
                match = re.search(r'queue_name = "([^"]+)".*operation = "([^"]+)".*latency_ns = (\d+)', line)
                if match:
                    yield vpid, match.group(1), match.group(2), int(match.group(3))
        
        proc.terminate()
    except Exception as e:
        print(f"Error streaming trace: {e}")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--trace-dir', required=True, help='LTTng trace directory (contains summary.csv)')
    parser.add_argument('--output-dir', default='./results/analysis')
    args = parser.parse_args()

    trace_dir = Path(args.trace_dir)
    summary_csv = trace_dir / 'summary.csv'
    if not summary_csv.exists():
        print(f"Error: summary.csv not found in {trace_dir}")
        sys.exit(1)

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # =========================================================================
    # Load summary CSV
    # =========================================================================
    delimiter = detect_delimiter(summary_csv)
    if delimiter is None:
        print("Error: Could not detect delimiter in summary.csv")
        sys.exit(1)
    
    summary = pd.read_csv(summary_csv, sep=delimiter)
    summary.columns = summary.columns.str.strip()
    
    print(f"Loaded {len(summary)} runs from {summary_csv}")
    
    # Convert numeric columns
    summary['items'] = pd.to_numeric(summary['items'], errors='coerce')
    summary['capacity'] = pd.to_numeric(summary['capacity'], errors='coerce')
    summary['throughput_mops'] = pd.to_numeric(summary['throughput_mops'], errors='coerce')
    
    # Fix items to 1M
    target_items = 1000000
    summary = summary[summary['items'] == target_items]
    print(f"Filtered to {target_items:,} items: {len(summary)} runs")
    
    # Classify balanced vs unbalanced
    summary['type'] = summary.apply(
        lambda row: 'balanced' if row['producers'] == row['consumers'] else 'unbalanced', axis=1)
    
    # Create PID to run mapping with placeholder for aggregators
    pid_to_key = {}
    for _, row in summary.iterrows():
        pid = int(row['pid']) if pd.notna(row['pid']) and row['pid'] != 0 else 0
        if pid > 0:
            key = f"{row['queue']}_{row['producers']}_{row['consumers']}_{row['capacity']}_{row['run']}"
            pid_to_key[pid] = key
    
    print(f"Loaded {len(pid_to_key)} PID mappings")

    # =========================================================================
    # Stream trace and aggregate on the fly (no event storage)
    # =========================================================================
    # Use dictionaries with StreamingPercentile objects
    enq_percentiles = defaultdict(lambda: defaultdict(StreamingPercentile))
    deq_percentiles = defaultdict(lambda: defaultdict(StreamingPercentile))
    
    print("\nStreaming trace events and aggregating percentiles...")
    total_events = 0
    matched_events = 0
    
    for vpid, queue, op, latency_ns in stream_trace_events(trace_dir):
        total_events += 1
        if total_events % 1000000 == 0:
            print(f"  Processed {total_events:,} events...")
        
        if vpid in pid_to_key:
            matched_events += 1
            key = pid_to_key[vpid]
            
            if op == 'enqueue':
                enq_percentiles[key]['p99'].add(latency_ns)
                enq_percentiles[key]['p95'].add(latency_ns)
                enq_percentiles[key]['p90'].add(latency_ns)
                enq_percentiles[key]['p50'].add(latency_ns)
            else:
                deq_percentiles[key]['p99'].add(latency_ns)
    
    print(f"Total events: {total_events:,}, matched to runs: {matched_events:,}")
    print(f"Unique runs with data: {len(enq_percentiles)}")

    # =========================================================================
    # Build results dataframe from aggregated percentiles
    # =========================================================================
    results = []
    for key in enq_percentiles.keys():
        parts = key.split('_')
        try:
            results.append({
                'queue': parts[0],
                'producers': int(parts[1]),
                'consumers': int(parts[2]),
                'capacity': int(parts[3]),
                'run': int(parts[4]),
                'enqueue_p99_ns': enq_percentiles[key]['p99'].percentile(99),
                'enqueue_p95_ns': enq_percentiles[key]['p95'].percentile(95),
                'enqueue_p90_ns': enq_percentiles[key]['p90'].percentile(90),
                'enqueue_p50_ns': enq_percentiles[key]['p50'].percentile(50),
                'dequeue_p99_ns': deq_percentiles[key]['p99'].percentile(99) if key in deq_percentiles else 0,
            })
        except (ValueError, IndexError) as e:
            print(f"Warning: Could not parse key '{key}': {e}")
            continue
    
    latency_df = pd.DataFrame(results)
    
    # Merge latency data with throughput data
    merged_df = pd.merge(
        summary, 
        latency_df, 
        on=['queue', 'producers', 'consumers', 'capacity', 'run'],
        how='left'
    )
    
    # Add type column
    merged_df['type'] = merged_df.apply(
        lambda row: 'balanced' if row['producers'] == row['consumers'] else 'unbalanced', axis=1)
    
    print(f"Merged data: {len(merged_df)} rows")

    # =========================================================================
    # PLOT 1: Throughput vs Capacity (Balanced)
    # =========================================================================
    balanced = merged_df[merged_df['type'] == 'balanced']
    
    plt.figure(figsize=(10, 6))
    for queue in balanced['queue'].unique():
        qdf = balanced[balanced['queue'] == queue]
        grouped = qdf.groupby('capacity')['throughput_mops'].agg(['mean', 'std']).reset_index()
        grouped = grouped.sort_values('capacity')
        plt.plot(grouped['capacity'], grouped['mean'], marker='o', linewidth=2, markersize=8, label=queue.upper())
        plt.fill_between(grouped['capacity'], grouped['mean'] - grouped['std'], grouped['mean'] + grouped['std'], alpha=0.2)
    plt.xscale('log')
    plt.xlabel('Queue Capacity (log scale)')
    plt.ylabel('Throughput (M ops/sec)')
    plt.title('Throughput vs Capacity (Balanced P=C, 1M items)')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_dir / 'throughput_vs_capacity_balanced.png', dpi=150)
    plt.close()
    print("Saved: throughput_vs_capacity_balanced.png")

    # =========================================================================
    # PLOT 2: Latency vs Capacity (Balanced) - Enqueue
    # =========================================================================
    plt.figure(figsize=(10, 6))
    for queue in balanced['queue'].unique():
        qdf = balanced[balanced['queue'] == queue]
        grouped = qdf.groupby('capacity')['enqueue_p99_ns'].agg(['mean', 'std']).reset_index()
        grouped = grouped.sort_values('capacity')
        plt.plot(grouped['capacity'], grouped['mean'] / 1000, marker='o', linewidth=2, markersize=8, label=queue.upper())
        plt.fill_between(grouped['capacity'], (grouped['mean'] - grouped['std']) / 1000, (grouped['mean'] + grouped['std']) / 1000, alpha=0.2)
    plt.xscale('log')
    plt.xlabel('Queue Capacity (log scale)')
    plt.ylabel('P99 Enqueue Latency (μs)')
    plt.title('Enqueue Latency vs Capacity (Balanced P=C, 1M items)')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_dir / 'enqueue_latency_vs_capacity_balanced.png', dpi=150)
    plt.close()
    print("Saved: enqueue_latency_vs_capacity_balanced.png")

    # =========================================================================
    # PLOT 3: Latency vs Capacity (Balanced) - Dequeue
    # =========================================================================
    plt.figure(figsize=(10, 6))
    for queue in balanced['queue'].unique():
        qdf = balanced[balanced['queue'] == queue]
        grouped = qdf.groupby('capacity')['dequeue_p99_ns'].agg(['mean', 'std']).reset_index()
        grouped = grouped.sort_values('capacity')
        plt.plot(grouped['capacity'], grouped['mean'] / 1000, marker='o', linewidth=2, markersize=8, label=queue.upper())
        plt.fill_between(grouped['capacity'], (grouped['mean'] - grouped['std']) / 1000, (grouped['mean'] + grouped['std']) / 1000, alpha=0.2)
    plt.xscale('log')
    plt.xlabel('Queue Capacity (log scale)')
    plt.ylabel('P99 Dequeue Latency (μs)')
    plt.title('Dequeue Latency vs Capacity (Balanced P=C, 1M items)')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_dir / 'dequeue_latency_vs_capacity_balanced.png', dpi=150)
    plt.close()
    print("Saved: dequeue_latency_vs_capacity_balanced.png")

    # =========================================================================
    # PLOT 4: Unbalanced Comparison
    # =========================================================================
    unbalanced = merged_df[merged_df['type'] == 'unbalanced']
    
    if not unbalanced.empty:
        unbalanced_summary = unbalanced.groupby(['queue', 'producers', 'consumers']).agg({
            'throughput_mops': ['mean', 'std']
        }).reset_index()
        unbalanced_summary.columns = ['queue', 'producers', 'consumers', 'throughput_mean', 'throughput_std']
        
        plt.figure(figsize=(12, 6))
        x = np.arange(len(unbalanced_summary))
        width = 0.6
        colors = {'mutex': '#1f77b4', 'ringbuffer': '#2ca02c', 'hazard': '#d62728'}
        
        plt.bar(x, unbalanced_summary['throughput_mean'], yerr=unbalanced_summary['throughput_std'], 
                capsize=5, width=width, alpha=0.7,
                color=[colors.get(q, 'gray') for q in unbalanced_summary['queue']])
        
        labels = [f"{row['queue'].upper()}\nP={int(row['producers'])}/C={int(row['consumers'])}" 
                  for _, row in unbalanced_summary.iterrows()]
        plt.xticks(x, labels)
        plt.ylabel('Throughput (M ops/sec)')
        plt.title('Unbalanced Configuration Comparison (1M items, capacity=50)')
        plt.grid(True, alpha=0.3, axis='y')
        plt.tight_layout()
        plt.savefig(output_dir / 'unbalanced_comparison.png', dpi=150)
        plt.close()
        print("Saved: unbalanced_comparison.png")

    # =========================================================================
    # PLOT 5: Latency Distribution Boxplot
    # =========================================================================
    target_capacity = 50
    capacity_data = merged_df[merged_df['capacity'] == target_capacity]
    
    if not capacity_data.empty:
        fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
        
        enq_data = []
        deq_data = []
        labels = []
        for queue in capacity_data['queue'].unique():
            qdf = capacity_data[capacity_data['queue'] == queue]
            if not qdf.empty and not qdf['enqueue_p99_ns'].isna().all():
                enq_data.append(qdf['enqueue_p99_ns'].dropna().values / 1000)
                deq_data.append(qdf['dequeue_p99_ns'].dropna().values / 1000)
                labels.append(queue.upper())
        
        if enq_data:
            ax1.boxplot(enq_data, tick_labels=labels)
            ax1.set_ylabel('P99 Enqueue Latency (μs)')
            ax1.set_title(f'Enqueue Latency Distribution (capacity={target_capacity}, 1M items)')
            ax1.grid(True, alpha=0.3)
        
        if deq_data:
            ax2.boxplot(deq_data, tick_labels=labels)
            ax2.set_ylabel('P99 Dequeue Latency (μs)')
            ax2.set_title(f'Dequeue Latency Distribution (capacity={target_capacity}, 1M items)')
            ax2.grid(True, alpha=0.3)
        
        plt.suptitle(f'Latency Distribution at Capacity {target_capacity}', fontsize=14)
        plt.tight_layout()
        plt.savefig(output_dir / f'latency_distribution_capacity_{target_capacity}.png', dpi=150)
        plt.close()
        print(f"Saved: latency_distribution_capacity_{target_capacity}.png")

    # =========================================================================
    # Print summary
    # =========================================================================
    print("\n" + "="*60)
    print("SUMMARY")
    print("="*60)
    
    cap_50_balanced = balanced[balanced['capacity'] == 50]
    if not cap_50_balanced.empty:
        print(f"\nThroughput at capacity=50 (balanced P=C, 1M items):")
        for queue in cap_50_balanced['queue'].unique():
            qdf = cap_50_balanced[cap_50_balanced['queue'] == queue]
            mean_tp = qdf['throughput_mops'].mean()
            std_tp = qdf['throughput_mops'].std()
            print(f"  {queue.upper()}: {mean_tp:.4f} ± {std_tp:.4f} M ops/sec")
    
    print(f"\nAll plots saved in {output_dir}/")

if __name__ == '__main__':
    main()
