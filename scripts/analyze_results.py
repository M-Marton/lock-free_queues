#!/usr/bin/env python3
"""
@file analyze_results.py
@brief Python script to analyze benchmark results and generate plots
@author MPMC Benchmark Project
@version 1.0.0

This script parses benchmark output files and generates:
- Throughput comparison charts
- Latency distribution plots
- Scalability analysis
"""

import os
import re
import sys
import argparse
from pathlib import Path
from typing import Dict, List, Tuple
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np

def parse_benchmark_output(filepath: Path) -> Dict:
    """
    Parse a benchmark output file and extract metrics.
    
    Args:
        filepath: Path to the benchmark output file
        
    Returns:
        Dictionary containing extracted metrics
    """
    with open(filepath, 'r') as f:
        content = f.read()
    
    metrics = {}
    
    # Extract throughput
    throughput_match = re.search(r'Throughput:\s+([\d.]+)\s+ops/sec', content)
    if throughput_match:
        metrics['throughput'] = float(throughput_match.group(1))
    
    # Extract enqueue latencies
    enqueue_match = re.search(r'Enqueue Latency \(ns\):.*?Min:\s+(\d+).*?Max:\s+(\d+).*?Avg:\s+([\d.]+).*?P99:\s+(\d+)', 
                               content, re.DOTALL)
    if enqueue_match:
        metrics['enqueue_min'] = int(enqueue_match.group(1))
        metrics['enqueue_max'] = int(enqueue_match.group(2))
        metrics['enqueue_avg'] = float(enqueue_match.group(3))
        metrics['enqueue_p99'] = int(enqueue_match.group(4))
    
    # Extract dequeue latencies
    dequeue_match = re.search(r'Dequeue Latency \(ns\):.*?Min:\s+(\d+).*?Max:\s+(\d+).*?Avg:\s+([\d.]+).*?P99:\s+(\d+)',
                               content, re.DOTALL)
    if dequeue_match:
        metrics['dequeue_min'] = int(dequeue_match.group(1))
        metrics['dequeue_max'] = int(dequeue_match.group(2))
        metrics['dequeue_avg'] = float(dequeue_match.group(3))
        metrics['dequeue_p99'] = int(dequeue_match.group(4))
    
    return metrics

def load_all_results(results_dir: Path) -> pd.DataFrame:
    """
    Load all benchmark results from the results directory.
    
    Args:
        results_dir: Directory containing benchmark output files
        
    Returns:
        DataFrame with all metrics
    """
    data = []
    
    for filepath in results_dir.glob("*.csv"):
        # Parse filename: queue_p#_c#.csv
        parts = filepath.stem.split('_')
        if len(parts) >= 2:
            queue = parts[0]
            producers = int(parts[1][1:])  # Remove 'p' prefix
            consumers = int(parts[2][1:])  # Remove 'c' prefix
        else:
            continue
        
        metrics = parse_benchmark_output(filepath)
        if metrics:
            metrics['queue'] = queue
            metrics['producers'] = producers
            metrics['consumers'] = consumers
            metrics['threads'] = producers + consumers
            data.append(metrics)
    
    return pd.DataFrame(data)

def plot_throughput_comparison(df: pd.DataFrame, output_dir: Path):
    """
    Create throughput comparison chart.
    
    Args:
        df: DataFrame with benchmark results
        output_dir: Directory to save plots
    """
    plt.figure(figsize=(12, 8))
    
    for queue in df['queue'].unique():
        queue_df = df[df['queue'] == queue]
        queue_df = queue_df.sort_values('threads')
        
        plt.plot(queue_df['threads'], queue_df['throughput'] / 1e6, 
                 marker='o', linewidth=2, label=queue.upper())
    
    plt.xlabel('Number of Threads (Producers + Consumers)', fontsize=12)
    plt.ylabel('Throughput (M ops/sec)', fontsize=12)
    plt.title('MPMC Queue Throughput Comparison', fontsize=14, fontweight='bold')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    
    output_path = output_dir / 'throughput_comparison.png'
    plt.savefig(output_path, dpi=150)
    plt.close()
    print(f"Saved: {output_path}")

def plot_latency_comparison(df: pd.DataFrame, output_dir: Path):
    """
    Create latency comparison chart.
    
    Args:
        df: DataFrame with benchmark results
        output_dir: Directory to save plots
    """
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))
    
    # P99 latency comparison
    for queue in df['queue'].unique():
        queue_df = df[df['queue'] == queue].sort_values('threads')
        axes[0].plot(queue_df['threads'], queue_df['enqueue_p99'] / 1000, 
                    marker='s', linewidth=2, label=queue.upper())
    
    axes[0].set_xlabel('Number of Threads', fontsize=12)
    axes[0].set_ylabel('P99 Enqueue Latency (μs)', fontsize=12)
    axes[0].set_title('P99 Enqueue Latency by Queue Type', fontsize=12, fontweight='bold')
    axes[0].legend()
    axes[0].grid(True, alpha=0.3)
    
    # Average latency comparison
    for queue in df['queue'].unique():
        queue_df = df[df['queue'] == queue].sort_values('threads')
        axes[1].plot(queue_df['threads'], queue_df['enqueue_avg'] / 1000,
                    marker='^', linewidth=2, label=queue.upper())
    
    axes[1].set_xlabel('Number of Threads', fontsize=12)
    axes[1].set_ylabel('Average Enqueue Latency (μs)', fontsize=12)
    axes[1].set_title('Average Enqueue Latency by Queue Type', fontsize=12, fontweight='bold')
    axes[1].legend()
    axes[1].grid(True, alpha=0.3)
    
    plt.tight_layout()
    output_path = output_dir / 'latency_comparison.png'
    plt.savefig(output_path, dpi=150)
    plt.close()
    print(f"Saved: {output_path}")

def plot_scalability(df: pd.DataFrame, output_dir: Path):
    """
    Create scalability analysis chart.
    
    Args:
        df: DataFrame with benchmark results
        output_dir: Directory to save plots
    """
    plt.figure(figsize=(12, 8))
    
    # Calculate ideal scaling (linear)
    baseline = df[df['threads'] == 2]['throughput'].mean()
    
    for queue in df['queue'].unique():
        queue_df = df[df['queue'] == queue].sort_values('threads')
        speedup = queue_df['throughput'] / baseline
        
        plt.plot(queue_df['threads'], speedup, 
                marker='o', linewidth=2, label=queue.upper())
    
    # Ideal scaling line
    max_threads = df['threads'].max()
    ideal = [t / 2 for t in range(2, max_threads + 1, 2)]
    plt.plot(range(2, max_threads + 1, 2), ideal, 
            'k--', linewidth=1, label='Ideal Scaling', alpha=0.7)
    
    plt.xlabel('Number of Threads', fontsize=12)
    plt.ylabel('Speedup (relative to 2 threads)', fontsize=12)
    plt.title('MPMC Queue Scalability Analysis', fontsize=14, fontweight='bold')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    
    output_path = output_dir / 'scalability_analysis.png'
    plt.savefig(output_path, dpi=150)
    plt.close()
    print(f"Saved: {output_path}")

def generate_summary_table(df: pd.DataFrame, output_dir: Path):
    """
    Generate a summary table of results.
    
    Args:
        df: DataFrame with benchmark results
        output_dir: Directory to save summary
    """
    # Pivot table for throughput at maximum threads
    max_threads = df[df['threads'] == df['threads'].max()]
    
    summary = max_threads.pivot_table(
        values='throughput',
        index='queue',
        columns='producers',
        aggfunc='first'
    )
    
    # Convert to millions for readability
    summary = summary / 1e6
    
    output_path = output_dir / 'summary_table.csv'
    summary.to_csv(output_path)
    print(f"Saved: {output_path}")
    
    # Print to console
    print("\n" + "="*60)
    print("SUMMARY: Throughput at Maximum Threads (M ops/sec)")
    print("="*60)
    print(summary.to_string())
    print("="*60 + "\n")

def main():
    parser = argparse.ArgumentParser(description='Analyze MPMC benchmark results')
    parser.add_argument('--results-dir', type=str, default='./results',
                        help='Directory containing benchmark results')
    parser.add_argument('--output-dir', type=str, default='./results/plots',
                        help='Directory to save plots')
    args = parser.parse_args()
    
    results_dir = Path(args.results_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    if not results_dir.exists():
        print(f"Error: Results directory {results_dir} not found")
        print("Run benchmarks first using scripts/run_benchmarks.sh")
        sys.exit(1)
    
    print("Loading benchmark results...")
    df = load_all_results(results_dir)
    
    if df.empty:
        print("No results found. Run benchmarks first.")
        sys.exit(1)
    
    print(f"Loaded {len(df)} benchmark configurations")
    
    print("\nGenerating plots...")
    plot_throughput_comparison(df, output_dir)
    plot_latency_comparison(df, output_dir)
    plot_scalability(df, output_dir)
    generate_summary_table(df, output_dir)
    
    print(f"\nAll analyses complete! Results saved in {output_dir}")

if __name__ == "__main__":
    main()
