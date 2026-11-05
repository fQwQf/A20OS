# Memory Management Verification & Stress Gates (`tools/mm/`)

This directory contains standalone gates and host verification harnesses testing memory allocator invariants, page cache writeback, and fault injection.

## Files

| File | Type | Make Target | Description |
|------|------|-------------|-------------|
| `check-slab-cpu-window.py` | Python / ASan | `make check-slab-cpu-window` | Concurrency and race boundary assertions for per-CPU slab allocator windows |
| `mm-alloc-fail-gate.py` | Python | `smoke-mm-alloc-fail` | Deterministic kernel allocation failure injection gate across multiple stages |
| `mm-shared-write-gate.py` | Python | `smoke-mm-shared-write` | Two-boot gate verifying `MAP_SHARED` dirty-page flush without explicit `msync` |
| `mmtest_gate.py` | Python | `smoke-mm-software` | High-stress memory workload verification harness (git, vim, gcc, python, nodejs) |
