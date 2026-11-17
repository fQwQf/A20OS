# Developer Guide Documentation Index

This directory is designed for A20OS kernel and system developers, providing a comprehensive technical guide covering everything from environment configuration, compilation and building, instance declaration, running and debugging, to the internal implementation of the toolchain.

---

## Development and Debugging Guidelines

| Document | Description |
|---|---|
| [build.md](build.md) | Comprehensive guide to building and running (dependency installation, cross-compilers, QEMU initialization, and troubleshooting) |
| [instances.md](instances.md) | Guide to declaring and instantiating systems (unified entry points for `instances/*.toml` and `tools/a20`) |
| [debug.md](debug.md) | Practical guide to kernel debugging (GDB debugging, breakpoint settings, and QEMU debugging interface) |
| [boot-model.md](boot-model.md) | System startup models and hardware detection processes (Multiboot2, FDT/DTB, UEFI) |
| [external-dependencies.md](external-dependencies.md) | Specifications for external dependencies and third-party libraries |
| [tooling-internals.md](tooling-internals.md) | Insights into system building and host tool technology (analysis of the scripts used in `tools/`) |
| [userland-submodules.md](userland-submodules.md) | Synchronization and management of user-space submodules (Alpine rootfs, musl, ports) |
