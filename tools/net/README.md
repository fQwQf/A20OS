# Network Subsystem Analysis & Conformance Tools (`tools/net/`)

This directory contains network packet capture analyzers, concurrency wiring checkers, virtio completion unit verifiers, and multi-lane performance reporters.

## Files

| File | Type | Make Target / Usage | Description |
|------|------|---------------------|-------------|
| `check-net-pcap.py` | Python | CLI / Net Tests | Ethernet/IP/TCP/UDP packet capture verification and header integrity checker |
| `check_lwip_concurrency_wiring.py` | Python | Gate | Static assertion verifying lane lock sequencing and IRQ staging rules |
| `check-virtio-net-completion.py` | Python / ASan | `make check-virtio-net-completion` | Unit test exercising production virtio-net TX completion ring under ASan/UBSan |
| `check_net_lane_report.py` | Python | Smoke helper | Validates per-lane connection distribution and lock budget reports |
| `check_net_lane_parallel_report.py`| Python | Smoke helper | Multi-lane parallel scalability and contention report validator |
| `lwip-sync.sh` | Shell | Maintenance | Helper to synchronize vendored lwIP tree components |

## Subdirectories

- `tests/`: Multi-node KVM and QEMU integration test runners (BSD bootstrap, desktop labwc browser, netlink control plane, and 3-node router topology).
- `tcp-cubic-test/`: Host testing fixtures and lwIP port override shims for RFC 8312 CUBIC congestion control verification (`test-tcp-cubic-host.sh`).
