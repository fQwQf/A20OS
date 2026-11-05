# Host Unit Test Harnesses

This directory contains standalone, host-side unit test drivers (`test-*-host.sh`, companion `test-*-host.c` stubs, and Python host verification scripts) that exercise kernel subsystems without requiring a full target emulator (QEMU) boot.

## Subsystems Covered
- **VFS / File Systems**: Page cache, block cache, UXFS RPC/lifecycle, mount namespace, file descriptor table state.
- **Networking & Sockets**: Netfilter NAT rewrite, lwIP TCP CUBIC conformance, AF_NETLINK, routing tables, lwIP software TX checksums (`test-lwip-software-tx.py`), BSD loopback/SYSINIT bootstrap (`test-bsd-bootstrap-host.py`), and BSD memory/task-TLS runtime under ASan/UBSan (`test-bsd-runtime-host.py`).
- **Memory Management**: COW faults, clone policies, madvise/mprotect/mseal, OOM and VMAR policies.
- **IPC & Synchronization**: Signals, mutexes, park-wait queues, and channel receive transactions.

## Execution
All test harnesses can be run individually or collectively via:
```bash
sh tools/run-host-scripts.sh
```
Targeted Make rules also invoke specific high-speed gates:
- `make test-ckpt-image`
- `make test-nat-rewrite`
- `make test-signal-core`
