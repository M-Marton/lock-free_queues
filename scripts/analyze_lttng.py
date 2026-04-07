#!/usr/bin/env python3

import os
import sys
import subprocess
import json
import re
import argparse
from pathlib import Path
from collections import defaultdict
import statistics

def parse_babeltrace_output(trace_path):
    """Parse babeltrace output to extract events"""
    events = []
    
    try:
        result = subprocess.run(
            ['babeltrace', str(trace_path)],
            capture_output=True,
            text=True,
            timeout=60
        )
        
        if result.returncode != 0:
            print(f"Error running babeltrace: {result.stderr}")
            return events
        
        for line in result.stdout.split('\n'):
            if 'benchmark_mpmc:queue_operation' in line:
                event = parse_event_line(line)
                if event:
                    events.append(event)
                    
    except subprocess.TimeoutExpired:
        print(f"Timeout processing trace: {trace_path}")
    except FileNotFoundError:
        print("babeltrace not found. Install lttng-tools package.")
    
    return events

def parse_event_line(line):
    """Parse a single babeltrace event line"""
    event = {}
    
    patterns = {
        'queue_type': r'queue_type\s*=\s*"([^"]+)"',
        'operation': r'operation\s*=\s*"([^"]+)"',
        'thread_id': r'thread_id\s*=\s*(\d+)',
        'latency_ns': r'latency_ns\s*=\s*(\d+)'
    }
    
    for key, pattern in patterns.items():
        match = re.search(pattern, line)
        if match:
            event[key] = match.group(1) if key in ['queue_type', 'operation'] else int(match.group(1))
    
    if event:
        if 'timestamp' in line:
            ts_match = re.search(r'\[(\d+\.\d+)\]', line)
            if ts_match:
                event['timestamp'] = float(ts_match.group(1))
    
    return event

def analyze_trace(trace_path):
    """Analyze a single trace directory"""
    print(f"\nAnalyzing: {trace_path}")
    
    events = parse_babeltrace_output(trace_path)
    
    if not events:
        print("  No events found in trace")
        return None
    
    enqueue_latencies = [e['latency_ns'] for e in events if e.get('operation') == 'enqueue']
    dequeue_latencies = [e['latency_ns'] for e in events if e.get('operation') == 'dequeue']
    
    thread_stats = defaultdict(lambda: {'enqueue_count': 0, 'dequeue_count': 0, 'total_latency': 0})
    for event in events:
        tid = event.get('thread_id', 0)
        if event.get('operation') == 'enqueue':
            thread_stats[tid]['enqueue_count'] += 1
            thread_stats[tid]['total_latency'] += event.get('latency_ns', 0)
        else:
            thread_stats[tid]['dequeue_count'] += 1
    
    results = {
        'total_events': len(events),
        'enqueue_count': len(enqueue_latencies),
        'dequeue_count': len(dequeue_latencies),
        'enqueue_latency': {
            'min': min(enqueue_latencies) if enqueue_latencies else 0,
            'max': max(enqueue_latencies) if enqueue_latencies else 0,
            'avg': statistics.mean(enqueue_latencies) if enqueue_latencies else 0,
            'p50': statistics.median(enqueue_latencies) if enqueue_latencies else 0,
            'p99': percentile(enqueue_latencies, 99) if enqueue_latencies else 0,
        },
        'dequeue_latency': {
            'min': min(dequeue_latencies) if dequeue_latencies else 0,
            'max': max(dequeue_latencies) if dequeue_latencies else 0,
            'avg': statistics.mean(dequeue_latencies) if dequeue_latencies else 0,
            'p50': statistics.median(dequeue_latencies) if dequeue_latencies else 0,
            'p99': percentile(dequeue_latencies, 99) if dequeue_latencies else 0,
        },
        'thread_stats': dict(thread_stats)
    }
    
    print(f"  Total events: {results['total_events']}")
    print(f"  Enqueues: {results['enqueue_count']} | Dequeues: {results['dequeue_count']}")
    print(f"  Enqueue P99 latency: {results['enqueue_latency']['p99']:.0f} ns")
    print(f"  Dequeue P99 latency: {results['dequeue_latency']['p99']:.0f} ns")
    
    return results

def percentile(data, p):
    """Calculate percentile of a list"""
    if not data:
        return 0
    sorted_data = sorted(data)
    idx = int(len(sorted_data) * p / 100)
    return sorted_data[min(idx, len(sorted_data) - 1)]

def find_trace_directories(trace_dir):
    """Find all trace directories"""
    traces = []
    
    if not trace_dir.exists():
        return traces
    
    for item in trace_dir.iterdir():
        if item.is_dir() and (item / 'ust').exists():
            traces.append(item)
        elif item.is_dir() and any((item / p).exists() for p in ['channel0_0', 'metadata']):
            traces.append(item)
    
    return traces

def generate_report(results, output_dir):
    """Generate JSON report and summary CSV"""
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    json_path = output_dir / 'lttng_analysis.json'
    with open(json_path, 'w') as f:
        json.dump(results, f, indent=2, default=str)
    print(f"\nJSON report saved: {json_path}")
    
    if results:
        csv_path = output_dir / 'lttng_summary.csv'
        with open(csv_path, 'w') as f:
            f.write("trace,enqueue_count,dequeue_count,enqueue_p99_ns,dequeue_p99_ns\n")
            for trace_name, data in results.items():
                f.write(f"{trace_name},{data['enqueue_count']},{data['dequeue_count']},"
                       f"{data['enqueue_latency']['p99']:.0f},{data['dequeue_latency']['p99']:.0f}\n")
        print(f"CSV summary saved: {csv_path}")

def main():
    parser = argparse.ArgumentParser(description='Analyze LTTng traces from MPMC benchmarks')
    parser.add_argument('--trace-dir', type=str, default='./results/traces',
                        help='Directory containing LTTng traces')
    parser.add_argument('--output-dir', type=str, default='./results/analysis',
                        help='Output directory for analysis results')
    parser.add_argument('--trace-name', type=str, default=None,
                        help='Analyze specific trace by name')
    args = parser.parse_args()
    
    trace_dir = Path(args.trace_dir)
    output_dir = Path(args.output_dir)
    
    if not trace_dir.exists():
        print(f"Trace directory not found: {trace_dir}")
        sys.exit(1)
    
    print("=" * 60)
    print("LTTng Trace Analysis Tool")
    print("=" * 60)
    
    if args.trace_name:
        trace_path = trace_dir / args.trace_name
        if trace_path.exists():
            results = {args.trace_name: analyze_trace(trace_path)}
        else:
            print(f"Trace not found: {trace_path}")
            sys.exit(1)
    else:
        traces = find_trace_directories(trace_dir)
        
        if not traces:
            print("No trace directories found.")
            sys.exit(0)
        
        print(f"\nFound {len(traces)} trace(s)")
        
        results = {}
        for trace in traces:
            trace_name = trace.name
            results[trace_name] = analyze_trace(trace)
    
    generate_report(results, output_dir)
    
    print("\n" + "=" * 60)
    print("Analysis Complete")
    print("=" * 60)

if __name__ == "__main__":
    main()
