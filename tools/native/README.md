# Native Runtime & Personality Verification Suite (`tools/native/`)

This directory contains the verification harnesses, admission gates, and ABI layout auditors for the A20 Native personality and runtime infrastructure.

## Subsystems Covered

- **Syscall & Feature Admission**: Syscall availability, argument translation, admission bounds, and ABI compatibility (`check-native-exec-features.py`, `check-execution.py`, `check-flat-user.py`).
- **Binary & Memory Layout**: Struct layout alignment across kernel, SDK, and mlibc sysdeps (`check-native-runtime-layout.py`).
- **Concurrency & Threads**: Thread admission limits, thread pooling, and thread lifecycle state machines (`check-native-thread-admission.py`).
- **Task Objects & Synchronization**: Production PID allocator, object handles, futexes, and cross-architecture task synchronization (`check-native-sync.py`, `check-native-task-object.py`, `run-task-object-qemu-series.py`).
- **Binary Fixtures**: ROM sector formatting and static flat executable fixtures for standalone firmware tests (`make-native-file-fixture.py`).

## Files

| File | Make Target / Usage | Description |
|------|---------------------|-------------|
| `check-native-sync.py` | `make check-native-sync` | Multi-architecture RV64/AArch64/x86_64 SMP native synchronization gate |
| `check-native-task-object.py` | `make check-native-task-object` | Task object lifetime, reference counting, and handle recycling verifier |
| `check-native-thread-admission.py` | `make check-native-thread-admission` | Native thread allocation and admission boundary testing under ASan/UBSan |
| `check-native-runtime.py` | `make check-native-runtime` | Full native runtime integration driver |
| `check-native-spawn-admission.py` | `make check-native-spawn-admission` | Flat binary spawn and process admission failure injection tests |
| `check-native-exec-features.py` | `make check-native-exec-features` | Execution feature discovery and CPU capability gate |
| `check-native-runtime-layout.py` | CLI / Gate | Struct wire layout assertion across kernel headers, C library, and SDK |
| `check-execution.py` | `make check-execution` | Execution contract verification |
| `check-flat-user.py` | `make check-flat-user` | Flat user memory allocation and pin/rollback verifier |
| `make-native-file-fixture.py` | Helper | Builds sparse FAT32 ROM sector fixtures for firmware tests |
| `run-task-object-qemu-series.py` | CLI / Test | Sequential QEMU run series harness for task object measurements |
