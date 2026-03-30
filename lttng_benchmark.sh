# Create LTTng session
lttng create mpmc-benchmark
lttng enable-event --userspace benchmark_mpmc:queue_operation
lttng add-context --userspace -t vtid -t vpid -t procname
lttng start

# Run benchmark
./build/benchmark --type=ringbuffer --producers=4 --consumers=4 --items=1000000

# Stop tracing
lttng stop

# View trace
babeltrace ~/lttng-traces/mpmc-benchmark-*/

# Or use Trace Compass for visualization
tracecompass ~/lttng-traces/mpmc-benchmark-*/
