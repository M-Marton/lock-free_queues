#!/usr/bin/env python3
"""
MPMC Benchmark Analysis - Statistical results with error bars
"""

import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import numpy as np
from pathlib import Path
import warnings
warnings.filterwarnings('ignore')

sns.set_style("darkgrid")

# ============================================================================
# Data loading
# ============================================================================
def load_summary():
    df = pd.read_csv('results/data/summary_results.csv')
    # Convert to numeric
    numeric_cols = ['cores', 'producers', 'consumers', 'items', 'capacity',
                    'throughput_mean', 'throughput_std',
                    'enqueue_p99_mean', 'enqueue_p99_std',
                    'dequeue_p99_mean', 'dequeue_p99_std']
    for col in numeric_cols:
        if col in df.columns:
            df[col] = pd.to_numeric(df[col], errors='coerce')
    df = df.dropna(subset=['throughput_mean'])
    return df

def load_details():
    return pd.read_csv('results/data/detail_results.csv')

# ============================================================================
# 1. Throughput vs Cores with error bars
# ============================================================================
def plot_throughput_with_errorbars(df, output_dir):
    """Line plots with error bars (std deviation) for each configuration"""
    config_cols = ['queue', 'producers', 'consumers', 'items', 'capacity']
    grouped = df.groupby(config_cols)
    
    for (queue, prod, cons, items, cap), group in grouped:
        if len(group) < 2:
            continue
        group = group.sort_values('cores')
        plt.figure()
        plt.errorbar(group['cores'], group['throughput_mean'],
                     yerr=group['throughput_std'], capsize=5, marker='o', linewidth=2, markersize=8)
        plt.xlabel('CPU Cores (cgroup limit)')
        plt.ylabel('Throughput (M ops/sec)')
        plt.title(f'{queue.upper()} | P={prod} C={cons} | items={items} | cap={cap}\n(mean ± std, {group["runs"].iloc[0]} runs)')
        plt.grid(True, alpha=0.3)
        plt.tight_layout()
        fname = f"throughput_stats_{queue}_p{prod}c{cons}_i{items}_s{cap}.png"
        plt.savefig(output_dir / fname, dpi=150)
        plt.close()
    print(f"  Generated {len(grouped)} throughput plots with error bars")

# ============================================================================
# 2. Heatmaps (using means)
# ============================================================================
def plot_heatmaps(df, output_dir):
    """Heatmaps of mean throughput for fixed cores"""
    # Create a column for log throughput (better color scaling)
    df['log_throughput'] = np.log1p(df['throughput_mean'])
    
    config_cols = ['queue', 'cores', 'items', 'capacity']
    for (queue, cores, items, cap), group in df.groupby(config_cols):
        if len(group) < 2:
            continue
        pivot = group.pivot_table(index='producers', columns='consumers',
                                  values='throughput_mean', aggfunc='first')
        if pivot.empty or pivot.shape[0] < 2 or pivot.shape[1] < 2:
            continue
        plt.figure(figsize=(10, 8))
        sns.heatmap(pivot, annot=True, fmt='.2f', cmap='viridis',
                    linewidths=0.5, cbar_kws={'label': 'Throughput (M ops/sec)'})
        plt.title(f'{queue.upper()} | cores={cores} | items={items} | cap={cap}')
        plt.xlabel('Consumers')
        plt.ylabel('Producers')
        plt.tight_layout()
        fname = f"heatmap_stats_{queue}_c{cores}_i{items}_s{cap}.png"
        plt.savefig(output_dir / fname, dpi=150)
        plt.close()
    print(f"  Generated heatmaps")

# ============================================================================
# 3. Latency comparison with error bars
# ============================================================================
def plot_latency_with_errorbars(df, output_dir):
    """Grouped bar chart with error bars for enqueue/dequeue P99 latency"""
    high_core = df[df['cores'] == df['cores'].max()]
    sample_configs = [
        (4, 4, 500000, 100000),
        (10, 1, 500000, 100000),
        (1, 10, 500000, 100000),
        (16, 16, 50000, 1000),
    ]
    for prod, cons, items, cap in sample_configs:
        sub = high_core[(high_core['producers'] == prod) &
                        (high_core['consumers'] == cons) &
                        (high_core['items'] == items) &
                        (high_core['capacity'] == cap)]
        if sub.empty:
            continue
        queues = sub['queue'].values
        enq_mean = sub['enqueue_p99_mean'].values / 1000  # to μs
        enq_std = sub['enqueue_p99_std'].values / 1000
        deq_mean = sub['dequeue_p99_mean'].values / 1000
        deq_std = sub['dequeue_p99_std'].values / 1000
        
        x = np.arange(len(queues))
        width = 0.35
        fig, ax = plt.subplots()
        ax.bar(x - width/2, enq_mean, width, yerr=enq_std, capsize=5,
               label='Enqueue P99', color='steelblue', error_kw={'ecolor': 'black'})
        ax.bar(x + width/2, deq_mean, width, yerr=deq_std, capsize=5,
               label='Dequeue P99', color='coral', error_kw={'ecolor': 'black'})
        ax.set_xlabel('Queue')
        ax.set_ylabel('Latency (μs)')
        ax.set_title(f'P99 Latency | P={prod} C={cons} | items={items} | cap={cap} | cores={high_core["cores"].iloc[0]}')
        ax.set_xticks(x)
        ax.set_xticklabels(queues)
        ax.legend()
        ax.grid(True, alpha=0.3)
        plt.tight_layout()
        fname = f"latency_stats_p{prod}c{cons}_i{items}_s{cap}.png"
        plt.savefig(output_dir / fname, dpi=150)
        plt.close()
    print(f"  Generated latency plots with error bars")

# ============================================================================
# 4. Boxplot of throughput by queue (using all individual runs for better distribution)
# ============================================================================
def plot_boxplot_individual_runs(detail_df, output_dir):
    """Boxplot using all individual run data (not just means)"""
    plt.figure(figsize=(10, 6))
    sns.boxplot(data=detail_df, x='queue', y='throughput_mops', palette='Set2')
    plt.xlabel('Queue Type')
    plt.ylabel('Throughput (M ops/sec)')
    plt.title('Throughput Distribution by Queue (all individual runs)')
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_dir / 'boxplot_individual_runs.png', dpi=150)
    plt.close()
    print(f"  Saved boxplot of individual runs")

# ============================================================================
# 5. Coefficient of variation analysis
# ============================================================================
def plot_cv_analysis(df, output_dir):
    """Plot coefficient of variation (std/mean) to show stability"""
    df['cv_throughput'] = df['throughput_std'] / df['throughput_mean']
    # For each queue, plot CV vs cores
    plt.figure(figsize=(10, 6))
    for queue in df['queue'].unique():
        qdf = df[df['queue'] == queue].groupby('cores')['cv_throughput'].mean().reset_index()
        plt.plot(qdf['cores'], qdf['cv_throughput'], marker='o', label=queue.upper())
    plt.xlabel('CPU Cores')
    plt.ylabel('Coefficient of Variation (σ/μ)')
    plt.title('Measurement Stability (lower is better)')
    plt.legend()
    plt.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_dir / 'coefficient_of_variation.png', dpi=150)
    plt.close()
    print(f"  Saved CV analysis")

# ============================================================================
# Main
# ============================================================================
def main():
    output_dir = Path('results/analysis')
    output_dir.mkdir(parents=True, exist_ok=True)
    
    print("Loading data...")
    df_summary = load_summary()
    if df_summary.empty:
        print("No summary results found. Run ./run.sh first.")
        return
    print(f"Loaded {len(df_summary)} summary configurations.")
    
    try:
        df_detail = load_details()
        print(f"Loaded {len(df_detail)} individual run records.")
    except:
        df_detail = pd.DataFrame()
    
    print("Generating plots...")
    plot_throughput_with_errorbars(df_summary, output_dir)
    plot_heatmaps(df_summary, output_dir)
    plot_latency_with_errorbars(df_summary, output_dir)
    if not df_detail.empty:
        plot_boxplot_individual_runs(df_detail, output_dir)
    plot_cv_analysis(df_summary, output_dir)
    
    # Generate statistical summary report
    report = output_dir / 'statistical_report.txt'
    with open(report, 'w') as f:
        f.write("MPMC BENCHMARK STATISTICAL REPORT\n")
        f.write("=================================\n\n")
        f.write(f"Based on {df_summary['runs'].iloc[0] if not df_summary.empty else '?'} runs per configuration.\n\n")
        f.write("Best throughput (mean) per queue:\n")
        best = df_summary.loc[df_summary.groupby('queue')['throughput_mean'].idxmax()]
        for _, row in best.iterrows():
            f.write(f"  {row['queue'].upper()}: {row['throughput_mean']:.2f} ± {row['throughput_std']:.2f} M ops/sec "
                    f"(P={int(row['producers'])}, C={int(row['consumers'])}, cores={int(row['cores'])})\n")
        f.write("\nLowest latency (enqueue P99) per queue:\n")
        best_lat = df_summary.loc[df_summary.groupby('queue')['enqueue_p99_mean'].idxmin()]
        for _, row in best_lat.iterrows():
            f.write(f"  {row['queue'].upper()}: {row['enqueue_p99_mean']:.0f} ± {row['enqueue_p99_std']:.0f} ns\n")
    print(f"Saved report: {report}")
    print("Analysis complete.")

if __name__ == '__main__':
    main()
