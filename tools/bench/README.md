# Benchmark & Performance Analysis Tools

This directory contains benchmarking harnesses, trace analysis tools, microbenchmark drivers, and performance diff utilities.

## Contents

### 1. Workload Comparison & Difference Analysis
- `a20-perf-diff.py`: Comprehensive `/proc/a20/perf` snapshot and diagnostic manifest diff tool.
- `analyze-apk-bottlenecks.py`: Multi-workload runner capturing A20OS hardware/software counters and lock contention traces.
- `summarize-benchmark-pairs.py`: Statistical reduction tool for paired benchmark samples.

### 2. Microbenchmarks & Subsystem Harnesses
- `bench-apk-linux.py`: Alpine sysbench / microbenchmark suite comparing native Linux and A20OS KVM guests.
- `bench-mmap-selector.py`: Interleaved C/Rust mapping-creation and unmap selector timing harness.
- `bench-net-kvm.py`: VirtIO TCP/UDP echo microbenchmark runner.
- `bench-net-scale-kvm.py`: Connection scalability probe under event loops and fresh guest boots.
- `bench-signal-core-host.sh`: Paired host microbenchmark for signal delivery decisions.
- `keyring_selector_bench.sh`: Keyring search/read/link latency comparison script.
- `lock_bench.py`: Multi-vCPU lock contention benchmark measuring per-lock totals and call-site samples.
- `net-microbench.c`: Portable C client/server microbenchmark compiled as a static Linux ELF for host and guest network evaluation.
