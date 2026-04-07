#!/usr/bin/env python3

import os
import sys
import subprocess
import argparse
from pathlib import Path
import pandas as pd
import matplotlib.pyplot as plt
import numpy as np
import seaborn as sns
import re

def load_results(summary_csv):
    """Load results from summary CSV, filtering out failed tests"""
    if not Path(summary_csv).exists():
        return pd.DataFrame()
    
    df = pd.read_csv(summary_csv)
    
    # Filter to successful tests only for performance metrics
    df_success = df[df['success'] == 1].copy()
    
    # Keep failed tests for deadlock analysis
    df_failed = df[df['success'] == 0].copy()
    
    return df_success, df_failed

def plot_throughput_comparison(df, output_dir):
    """Create throughput comparison chart (successful tests only)"""
    if df.empty:
        print("No successful tests for throughput comparison")
        return
    
    plt.figure(figsize=(14, 8))
    
    for queue in df['queue'].unique():
        queue_df = df[df['queue'] == queue]
        queue_df = queue_df[queue_df['producers'] == queue_df['consumers']]
        queue_df = queue_df.sort_values('producers')
        
        if not queue_df.empty and queue_df['throughput'].max() > 0:
            plt.plot(queue_df['producers'], queue_df['throughput'] / 1e6, 
                    marker='o', linewidth=2, markersize=8, label=queue.upper())
    
    plt.xlabel('Threads (P=C)')
    plt.ylabel('Throughput (M ops/sec)')
    plt.title('MPMC Queue Throughput Comparison (Successful Runs)')
    plt.legend(loc='upper right')
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    
    output_path = output_dir / 'throughput_comparison.png'
    plt.savefig(output_path, dpi=150)
    plt.close()
    print(f"Saved: {output_path}")

def plot_deadlock_analysis(df_failed, output_dir):
    """Analyze and plot deadlock patterns from failed tests"""
    if df_failed.empty:
        print("No failures to analyze")
        return
    
    # Deadlock heatmap
    pivot = df_failed.pivot_table(
        values='deadlocks',
        index='queue',
        columns='producers',
        aggfunc='sum',
        fill_value=0
    )
    
    if not pivot.empty:
        plt.figure(figsize=(12, 6))
        sns.heatmap(pivot, annot=True, fmt='d', cmap='Reds', 
                    xticklabels=True, yticklabels=True)
        plt.title('Deadlocks/Crashes by Queue and Producer Count')
        plt.xlabel('Producer Threads')
        plt.ylabel('Queue Type')
        plt.tight_layout()
        
        output_path = output_dir / 'deadlock_heatmap.png'
        plt.savefig(output_path, dpi=150)
        plt.close()
        print(f"Saved: {output_path}")
    
    # Failure rate by queue
    total_by_queue = df_failed.groupby('queue').size()
    deadlocks_by_queue = df_failed.groupby('queue')['deadlocks'].sum()
    
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
    
    total_by_queue.plot(kind='bar', ax=ax1, color='red', alpha=0.7)
    ax1.set_xlabel('Queue Type')
    ax1.set_ylabel('Number of Failed Configurations')
    ax1.set_title('Failed Test Configurations by Queue')
    ax1.set_xticklabels(ax1.get_xticklabels(), rotation=45)
    
    deadlocks_by_queue.plot(kind='bar', ax=ax2, color='orange', alpha=0.7)
    ax2.set_xlabel('Queue Type')
    ax2.set_ylabel('Total Deadlocks/Crashes')
    ax2.set_title('Total Deadlocks/Crashes by Queue')
    ax2.set_xticklabels(ax2.get_xticklabels(), rotation=45)
    
    plt.tight_layout()
    output_path = output_dir / 'failure_analysis.png'
    plt.savefig(output_path, dpi=150)
    plt.close()
    print(f"Saved: {output_path}")

def plot_success_rate(df_success, df_failed, output_dir):
    """Plot success rate per queue"""
    success_counts = df_success.groupby('queue').size()
    failed_counts = df_failed.groupby('queue').size()
    
    all_queues = set(success_counts.index) | set(failed_counts.index)
    
    success_rates = []
    queues_list = []
    
    for queue in sorted(all_queues):
        total = success_counts.get(queue, 0) + failed_counts.get(queue, 0)
        if total > 0:
            success_rate = (success_counts.get(queue, 0) / total) * 100
            success_rates.append(success_rate)
            queues_list.append(queue)
    
    plt.figure(figsize=(12, 6))
    bars = plt.bar(queues_list, success_rates, alpha=0.7)
    
    # Color bars based on success rate
    for bar, rate in zip(bars, success_rates):
        if rate == 100:
            bar.set_color('green')
        elif rate >= 70:
            bar.set_color('yellowgreen')
        elif rate >= 50:
            bar.set_color('yellow')
        else:
            bar.set_color('red')
    
    plt.xlabel('Queue Type')
    plt.ylabel('Success Rate (%)')
    plt.title('Test Success Rate by Queue Type')
    plt.ylim(0, 105)
    plt.axhline(y=100, color='green', linestyle='--', alpha=0.5, label='Perfect')
    plt.axhline(y=80, color='orange', linestyle='--', alpha=0.5, label='Acceptable')
    plt.legend()
    plt.tight_layout()
    
    output_path = output_dir / 'success_rate.png'
    plt.savefig(output_path, dpi=150)
    plt.close()
    print(f"Saved: {output_path}")

def generate_report(df_success, df_failed, output_dir):
    """Generate summary report"""
    report_path = output_dir / 'analysis_report.txt'
    
    with open(report_path, 'w') as f:
        f.write("=" * 80 + "\n")
        f.write("MPMC QUEUE BENCHMARK ANALYSIS REPORT\n")
        f.write("=" * 80 + "\n\n")
        
        f.write("TEST SUMMARY\n")
        f.write("-" * 40 + "\n")
        f.write(f"Successful configurations: {len(df_success)}\n")
        f.write(f"Failed configurations: {len(df_failed)}\n")
        f.write(f"Overall success rate: {len(df_success) * 100 / (len(df_success) + len(df_failed)):.1f}%\n\n")
        
        f.write("PER-QUEUE SUCCESS RATE\n")
        f.write("-" * 40 + "\n")
        
        for queue in sorted(set(df_success['queue'].unique()) | set(df_failed['queue'].unique())):
            success_count = len(df_success[df_success['queue'] == queue])
            failed_count = len(df_failed[df_failed['queue'] == queue])
            total = success_count + failed_count
            if total > 0:
                rate = success_count * 100 / total
                f.write(f"{queue:20} : {success_count:3}/{total:3} ({rate:5.1f}%)\n")
        
        if not df_success.empty:
            f.write("\nPERFORMANCE SUMMARY (Successful Runs)\n")
            f.write("-" * 40 + "\n")
            
            best_throughput = df_success.loc[df_success['throughput'].idxmax()]
            f.write(f"Best throughput: {best_throughput['queue'].upper()} ")
            f.write(f"with {best_throughput['producers']}P/{best_throughput['consumers']}C ")
            f.write(f"@ {best_throughput['throughput']/1e6:.2f} M ops/sec\n")
            
            best_latency = df_success.loc[df_success['enqueue_p99'].idxmin()]
            f.write(f"Best P99 latency: {best_latency['queue'].upper()} ")
            f.write(f"with {best_latency['producers']}P/{best_latency['consumers']}C ")
            f.write(f"@ {best_latency['enqueue_p99']:.0f} ns\n")
        
        if not df_failed.empty:
            f.write("\nFAILURE ANALYSIS\n")
            f.write("-" * 40 + "\n")
            
            most_deadlocks = df_failed.loc[df_failed['deadlocks'].idxmax()]
            f.write(f"Most deadlocks: {most_deadlocks['queue'].upper()} ")
            f.write(f"with {most_deadlocks['deadlocks']} deadlocks ")
            f.write(f"(P={most_deadlocks['producers']}, C={most_deadlocks['consumers']})\n")
    
    print(f"Saved: {report_path}")

def main():
    parser = argparse.ArgumentParser(description='Analyze MPMC benchmark results')
    parser.add_argument('--data-dir', type=str, default='./results/data',
                        help='Directory containing benchmark results')
    parser.add_argument('--output-dir', type=str, default='./results/analysis',
                        help='Directory to save plots and analysis')
    parser.add_argument('--skip-lttng', action='store_true',
                        help='Skip LTTng trace analysis')
    args = parser.parse_args()
    
    data_dir = Path(args.data_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    summary_csv = data_dir / 'summary.csv'
    
    if not summary_csv.exists():
        print(f"Error: Summary file not found: {summary_csv}")
        print("Run benchmarks first using scripts/run_benchmarks.sh")
        sys.exit(1)
    
    print("Loading benchmark results...")
    df_success, df_failed = load_results(summary_csv)
    
    print(f"Loaded {len(df_success)} successful configurations")
    print(f"Loaded {len(df_failed)} failed configurations")
    
    print("\nGenerating plots...")
    plot_throughput_comparison(df_success, output_dir)
    plot_deadlock_analysis(df_failed, output_dir)
    plot_success_rate(df_success, df_failed, output_dir)
    generate_report(df_success, df_failed, output_dir)
    
    print(f"\nAnalysis complete! Results saved in {output_dir}")

if __name__ == "__main__":
    main()
