# mlibc Runtime & Thread Coordination Tools (`tools/mlibc/`)

This directory contains standalone QEMU verification drivers executing native programs linked against the mlibc C library and POSIX threads implementation.

## Files

| File | Description |
|------|-------------|
| `check-mlibc-runtime.py` | Runs native libc boundary and application tests in an isolated RV64/AArch64 VM |
| `check-mlibc-signals.py` | Boots A20OS and exercises libc pthread signal masks, suspension, and joining |
| `check-mlibc-threads.py` | Boots A20OS and runs full-libc pthread mutex, condvar, and TLS workloads |
