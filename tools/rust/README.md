# Rust Custom Targets & Standalone Core Tooling (`tools/rust/`)

This directory consolidates bare-metal Rust target specifications and standalone `core` compilation manifests for architectures and microcontrollers not bundled with upstream precompiled standard libraries:

## Subdirectories

### 1. `targets/`
Custom target specifications in JSON format:
- `riscv32imafdc-a20.json`: 32-bit RISC-V hardware floating-point (ILP32D) target specification used by QEMU RV32 instances.
- `riscv32imc-a20.json`: 32-bit RISC-V integer/compressed (ILP32) target specification used by embedded microcontrollers (ESP32-C3).

### 2. `core/`
Minimal Cargo project used to invoke `rustc -Z build-std=core` for building isolated `libcore` and `libcompiler_builtins` archives for `aarch64-unknown-none-softfloat` and custom `riscv32*-a20` platforms.
