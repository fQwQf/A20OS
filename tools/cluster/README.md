# Cluster Verification and Simulation Tools (`tools/cluster/`)

This directory contains the verification harnesses, protocol fuzzers, integration test runners, and multi-node network simulation utilities for the A20 cluster subsystem.

## Overview

The cluster subsystem supports distributed RPC, reliable datagrams (`SEND`), multi-node mesh discovery, leaf node bridging (over UART/SLIP), and Single System Image (SSI) primitives. The tools in this directory exercise these components across three execution tiers:

1. **Host Unit Tests & Fuzzers**: Native C binaries compiled with ASan/UBSan against the production frame codec and session state machines.
2. **Dual- & Multi-Node TCG/QEMU Emulation**: Automated multi-instance boots running independent A20 kernels communicating over UDP sockets or virtual serial interfaces.
3. **Load, Hardening & Fault Injection**: Deterministic packet loss, delay, corruption, peer restart, and partition testing.

## Files

### Unit Tests & Frame Fuzzers

| File | Description | Make Target |
|------|-------------|-------------|
| `check-cluster-unit.py` | Runs host C unit tests (`test_clx_*`), reference selftest, and peer table CRUD | `make check-cluster-unit` |
| `check-cluster-fuzz.py` | Mutation fuzzer under ASan/UBSan testing wire framing, headers, and payload boundaries | `make check-cluster-fuzz`, `make check-cluster-fuzz-smoke` |
| `check-cluster-wakeup.py` | Concurrency and race boundary checks on RX wakeup paths | `make check-cluster-wakeup` |
| `check-cluster-udp-peers.py` | Peer table allocation, churn, heartbeat, and timeout harness | Invoked by `check-cluster-unit` |

### Multi-Node Integration & Routing

| File | Description | Make Target |
|------|-------------|-------------|
| `check-cluster-loopback.py` | Single-kernel loopback RPC and route cancellation regression | `make check-cluster-loopback` |
| `check-cluster-udp.py` | 2- and 3-node UDP cluster test with packet loss, fragmentation, and restart | `make check-cluster-udp`, `make check-cluster-reliable`, `make check-cluster-send`, `make check-cluster-smp`, `make check-cluster-hardening` |
| `check-cluster-uart.py` | Native COM2 UART transport and leaf node pairing | `make check-cluster-uart` |
| `check-cluster-gateway.py` | Routed gateway sessions (RISC-V Native → UDP → x86 COM2 → leaf) | `make check-cluster-gateway` |
| `check-cluster-mesh.py` | 3-node Native cluster mesh with indirect probing and member reboot | `make check-cluster-mesh` |
| `check-cluster-wordcount.py` | 16-shard distributed word count pipeline across Native & leaf nodes | `make demo-wordcount` |

### Application & Resource Management

| File | Description | Make Target |
|------|-------------|-------------|
| `check-cluster-apps.py` | Multi-architecture application scheduling and failover | `make check-cluster-apps` |
| `check-cluster-instance.py` | Instance admission, foreign node rejection, and daemon restart | `make check-cluster-instance` |
| `check-cluster-objects.py` | Global cluster object lookup, leasing, and revocation | `make check-cluster-objects` |
| `check-cluster-memory.py` | Distributed memory state unit verification | `make check-cluster-memory` |
| `check-cluster-memory-network.py` | Multi-node remote paging and memory network verification | `make check-cluster-memory-network` |
| `check-cluster-ssi.py` | Single System Image local state checks | `make check-cluster-ssi` |
| `check-cluster-ssi-network.py` | Multi-node SSI thread slice migration tests | `make check-cluster-ssi-network` |

### Performance, Utilities & Network Bringup

| File | Description | Usage / Target |
|------|-------------|----------------|
| `check-cluster-load.py` | Sustained 64B SEND throughput and zero-loss contract verification | `make check-cluster-load` |
| `check-cluster-performance.py` | RPC latency and throughput benchmarks (local, loopback, UDP) | `make check-cluster-performance` |
| `compare-cluster-performance.py` | Compares raw per-boot measurement files from performance runs | CLI |
| `cluster-net-up.sh` | Shell script launching dual QEMU cluster instances (`qemu-riscv64-cluster-a/b`) | Manual / CI bringup |
| `cluster-qmp.py` | QMP control helper (screenshots, keystrokes) for cluster VMs | Diagnostic |

## Running Tests

Most tests can be invoked directly through make:

```bash
# Run unit tests and reference codec selftests
make check-cluster-unit

# Run fuzz testing
make check-cluster-fuzz

# Run dual-node UDP cluster integration test
make check-cluster-udp
```
