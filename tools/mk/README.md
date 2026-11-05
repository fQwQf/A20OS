# A20OS Build System Fragments

This directory contains modular Makefile fragments included by the repository root `Makefile` and architecture/board makefiles.

## Organization
- `targets-*.mk`: Core build, verification, gate, packaging, and smoke testing targets.
- `driver-*.mk`: Driver subsystem deployment, sources, and module packaging recipes.
- `rust-*.mk`: Rust kernel and userland toolchain and compilation rules.
- `<platform>.mk`: Platform- and board-specific firmware packaging rules (`esp32c3.mk`, `hi3861.mk`, `stm32.mk`, etc.).
