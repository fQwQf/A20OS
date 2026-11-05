# Archived Development & Profiling Artifacts

This directory contains historical diagnostic patches, discarded optimization experiments, one-off migration verification harnesses, and micro-benchmark drivers preserved for provenance and experimental reproducibility without cluttering the primary `tools/` root namespace.

## Contents

### 1. Memory & COW Profiling Experiments
- `bottleneck-*.patch`, `ptshare-experiment.patch`: Early copy-on-write, page-table sharing, and futex patches referenced in `docs/status/history/perf-worklog-2026-10-22.md`.
- `bottleneck-*.c`, `bottleneck-sysbench-fileio.sh`: C drivers and sysbench runner for COW bottleneck isolation.

### 2. Historical Subsystem Migration & Integration Harnesses
- `check-mutex-state-integration.py`, `check-park-wait-integration.py`, `check-process-integration.py`, `check-rw-mutex-integration.py`, `check-wait-queue-integration.py`, `check-wait-timer-integration.py`: Isolated RV64 SMP integration verifiers used during kernel synchronization/memory state migration to Rust.
- `smoke_bind_records.py`: Boot input binding runner used during early Rust signal/ELF/FS integration.
- `run-idl-service-interop-guest.py`, `run-native-idl-rv32.py`: Verification scripts for early native C IDL and RV32 client/server interop.
- `run_instance_tcg.py`: Specialized TCG runner forcing accelerator bypass for preemption measurement.
- `test-bsd-inbound-kvm.py`: Early inbound socket probe for the BSD networking stack.

### 3. One-Off Benchmarking & Development Scripts
- `bench_console.py`: Terminal transport throughput comparison driver.
- `bench-apk-kvm.py`: KVM-based APK workload comparison runner.
- `bench-net-inbound.py`: Isolated TCP inbound hostfwd benchmark driver.
- `fs_guest_run.sh`: Early standalone filesystem guest runner.
- `net-check.sh`: Syntax-only concurrent compilation check script for the networking completion units.

### 4. Standalone Runtime & Third-Party Porting Recipes
- `build-jamvm-classlib.py`, `build-jamvm-gc.py`, `build-jamvm-jni.py`, `build-jamvm-native.py`: GNU Classpath 0.99 and JamVM 2.0.0 standalone port compilation, JNI linking, and cooperative GC safepoint patch builders.
- `check-jamvm-native.py`: Standalone VM launcher and JNI probe runner in A20OS guests.
- `build-mlibc-native.py`, `build-mlibc-software.py`: Isolated full Native mlibc sysroot builder and test application (Lua/SQLite) compilation drivers.

### 5. Multi-Node & Shared Device Experiment Audits
- `audit-shared-minecraft.py`: Analysis and screenshot audit script for retained Minecraft execution evidence from cluster stage 61.
- `check-shared-device-call.py`: Concurrent multi-architecture host shim harness for shared device mailbox routing from cluster stage 56.
- `shared-device-test/`: Test fixtures and host shims used by `check-shared-device-call.py`.
- `shared-fabric-test/`: Standalone software driver and fake bus simulator for shared fabric semantics.
