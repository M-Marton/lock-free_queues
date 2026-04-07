#!/usr/bin/env python3

import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
from pathlib import Path
import os
import sys
import csv

def ensure_dir(path):
    """Create directory with proper permissions"""
    path = Path(path)
    if not path.exists():
        path.mkdir(parents=True, exist_ok=True)
    return path

def debug_print_csv_content(filepath):
    """Print first few lines of CSV for debugging"""
    print(f"\nDebug: Reading {filepath}")
    with open(filepath, 'r') as f:
        lines = f.readlines()
        print(f"  Total lines: {len(lines)}")
        print(f"  Header: {lines[0].strip()}")
        if len(lines) > 1:
            print(f"  First data row: {lines[1].strip()}")
        if len(lines) > 2:
            print(f"  Second data row: {lines[2].strip()}")

def load_data():
    """Load results from CSV with proper type handling and debugging"""
    results_file = Path('results/data/results.csv')
    
    if not results_file.exists():
        print(f"Error: Results file not found: {results_file}")
        print("Run sudo ./run.sh first")
        return pd.DataFrame()
    
    # Debug: Show file content
    debug_print_csv_content(results_file)
    
    # Try reading with standard pandas
    try:
        df = pd.read_csv(results_file)
        print(f"\n  Initial read: {len(df)} rows, columns: {list(df.columns)}")
    except Exception as e:
        print(f"  Error reading with pandas: {e}")
        return pd.DataFrame()
    
    # Check if DataFrame is empty
    if df.empty:
        print("  DataFrame is empty")
        return pd.DataFrame()
    
    # Print column dtypes before conversion
    print(f"  Column types before conversion:")
    for col in df.columns:
        print(f"    {col}: {df[col].dtype}")
    
    # Convert columns to numeric where appropriate
    numeric_cols = ['throughput_mops', 'enqueue_p99_ns', 'dequeue_p99_ns', 'retries', 'cores']
    for col in numeric_cols:
        if col in df.columns:
            original = df[col].copy()
            df[col] = pd.to_numeric(df[col], errors='coerce')
            na_count = df[col].isna().sum()
            if na_count > 0:
                print(f"  Converted {col}: {na_count} NA values (from {len(original)} total)")
    
    # Filter out invalid rows
    original_len = len(df)
    df = df[df['throughput_mops'] > 0]
    df = df[df['cores'].notna()]
    print(f"  Filtered: {original_len} -> {len(df)} rows")
    
    if df.empty:
        print("  No valid rows after filtering")
        return pd.DataFrame()
    
    # Print sample of valid data
    print(f"\n  Sample of valid data:")
    print(df[['queue', 'cores', 'throughput_mops']].head())
    
    return df

def plot_throughput(df, output_dir):
    """Create throughput comparison chart"""
    plt.figure(figsize=(10, 6))
    
    for queue in df['queue'].unique():
        qdf = df[df['queue'] == queue].copy()
        qdf = qdf.sort_values('cores')
        
        cores = qdf['cores'].values
        throughput = qdf['throughput_mops'].values
        
        if len(cores) > 0:
            plt.plot(cores, throughput, 
                    marker='o', linewidth=2, markersize=8, label=queue.upper())
            print(f"  Plotted {queue}: cores={cores}, throughput={throughput}")
    
    plt.xlabel('CPU Cores (cgroup limit)')
    plt.ylabel('Throughput (M ops/sec)')
    plt.title('MPMC Queue Throughput vs CPU Cores (P=16, C=16)')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_dir / 'throughput.png', dpi=150)
    plt.close()
    print(f"  Saved: throughput.png")

def plot_latency(df, output_dir):
    """Create latency comparison chart"""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))
    
    has_data = False
    
    for queue in df['queue'].unique():
        qdf = df[df['queue'] == queue].copy()
        qdf = qdf.sort_values('cores')
        
        cores = qdf['cores'].values
        enq_lat = qdf['enqueue_p99_ns'].values
        deq_lat = qdf['dequeue_p99_ns'].values
        
        # Filter out NaN values
        valid_mask = ~np.isnan(enq_lat) & ~np.isnan(deq_lat)
        
        if len(cores) > 0 and valid_mask.any():
            has_data = True
            ax1.plot(cores[valid_mask], enq_lat[valid_mask] / 1000, 
                    marker='s', linewidth=2, label=queue.upper())
            ax2.plot(cores[valid_mask], deq_lat[valid_mask] / 1000, 
                    marker='^', linewidth=2, label=queue.upper())
    
    if not has_data:
        print("  Warning: No latency data available")
        plt.close()
        return
    
    ax1.set_xlabel('CPU Cores')
    ax1.set_ylabel('P99 Enqueue Latency (μs)')
    ax1.set_title('Enqueue P99 Latency')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    
    ax2.set_xlabel('CPU Cores')
    ax2.set_ylabel('P99 Dequeue Latency (μs)')
    ax2.set_title('Dequeue P99 Latency')
    ax2.legend()
    ax2.grid(True, alpha=0.3)
    
    plt.tight_layout()
    plt.savefig(output_dir / 'latency.png', dpi=150)
    plt.close()
    print(f"  Saved: latency.png")

def plot_scaling(df, output_dir):
    """Create scalability analysis chart"""
    plt.figure(figsize=(10, 6))
    
    has_data = False
    
    for queue in df['queue'].unique():
        qdf = df[df['queue'] == queue].copy()
        qdf = qdf.sort_values('cores')
        
        cores = qdf['cores'].values
        throughput = qdf['throughput_mops'].values
        
        # Find baseline at 1 core
        baseline_mask = cores == 1
        if baseline_mask.any() and len(throughput) > 0:
            baseline = throughput[baseline_mask][0]
            if baseline > 0:
                scaling = throughput / baseline
                plt.plot(cores, scaling, marker='o', linewidth=2, label=queue.upper())
                has_data = True
    
    if not has_data:
        print("  Warning: No scaling data available")
        plt.close()
        return
    
    max_core = df['cores'].max()
    plt.plot([1, max_core], [1, max_core], 'k--', alpha=0.5, label='Ideal')
    plt.xlabel('CPU Cores')
    plt.ylabel('Speedup (relative to 1 core)')
    plt.title('Scalability Analysis')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_dir / 'scaling.png', dpi=150)
    plt.close()
    print(f"  Saved: scaling.png")

def generate_report(df, output_dir):
    """Generate text report"""
    report_path = output_dir / 'report.txt'
    
    with open(report_path, 'w') as f:
        f.write("=" * 60 + "\n")
        f.write("MPMC QUEUE BENCHMARK REPORT\n")
        f.write("=" * 60 + "\n\n")
        
        max_cores = df['cores'].max()
        
        f.write(f"Best throughput at {int(max_cores)} cores:\n")
        best = df[df['cores'] == max_cores].sort_values('throughput_mops', ascending=False)
        for _, row in best.iterrows():
            f.write(f"  {row['queue'].upper()}: {row['throughput_mops']:.2f} M ops/sec\n")
        
        f.write(f"\nBest latency at {int(max_cores)} cores:\n")
        best_lat = df[df['cores'] == max_cores].sort_values('enqueue_p99_ns')
        for _, row in best_lat.iterrows():
            if pd.notna(row['enqueue_p99_ns']):
                f.write(f"  {row['queue'].upper()}: {row['enqueue_p99_ns']:.0f} ns\n")
        
        f.write("\nScalability (max-core / 1-core):\n")
        for queue in df['queue'].unique():
            qdf = df[df['queue'] == queue]
            if len(qdf) >= 2:
                c1 = qdf[qdf['cores'] == 1]['throughput_mops'].values
                cmax = qdf[qdf['cores'] == max_cores]['throughput_mops'].values
                if len(c1) > 0 and len(cmax) > 0 and c1[0] > 0:
                    speedup = cmax[0] / c1[0]
                    f.write(f"  {queue.upper()}: {speedup:.2f}x\n")
    
    print(f"  Saved: report.txt")

def print_summary(df):
    """Print summary to console"""
    print("\n" + "=" * 60)
    print("QUICK SUMMARY")
    print("=" * 60)
    
    max_cores = int(df['cores'].max())
    
    print(f"\nAt {max_cores} cores:")
    for queue in df['queue'].unique():
        qdf = df[(df['queue'] == queue) & (df['cores'] == max_cores)]
        if not qdf.empty:
            throughput = qdf['throughput_mops'].values[0]
            enq_lat = qdf['enqueue_p99_ns'].values[0] if not pd.isna(qdf['enqueue_p99_ns'].values[0]) else 0
            print(f"  {queue.upper()}: {throughput:.2f} M ops/sec, {enq_lat:.0f} ns P99")
    
    # Find best overall
    best = df[df['cores'] == max_cores].sort_values('throughput_mops', ascending=False).iloc[0]
    print(f"\nBest overall: {best['queue'].upper()} with {best['throughput_mops']:.2f} M ops/sec")

def main():
    print("=" * 60)
    print("MPMC Queue Benchmark Analysis")
    print("=" * 60)
    
    # Create output directory
    output_dir = ensure_dir('results/analysis')
    
    # Load data
    df = load_data()
    if df.empty:
        print("\nNo valid results found.")
        print("\nPossible issues:")
        print("  1. Benchmark didn't complete successfully")
        print("  2. CSV format is incorrect")
        print("  3. No successful test runs")
        print("\nCheck results/data/results.csv manually:")
        print("  cat results/data/results.csv")
        sys.exit(1)
    
    print(f"\nProcessing {len(df)} successful results")
    print(f"Queues: {', '.join(df['queue'].unique())}")
    print(f"Cores tested: {sorted(df['cores'].unique())}")
    
    print("\nGenerating plots...")
    plot_throughput(df, output_dir)
    plot_latency(df, output_dir)
    plot_scaling(df, output_dir)
    generate_report(df, output_dir)
    print_summary(df)
    
    print(f"\nAnalysis complete! Results saved in {output_dir}/")
    print("\nTo view results:")
    print("  cat results/analysis/report.txt")
    print("  firefox results/analysis/throughput.png")

if __name__ == '__main__':
    main()
