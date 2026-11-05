# MCU Shared Kernel & Hardware Fabric Tools (`tools/mcu/`)

This directory contains verification harnesses for multi-MCU shared kernel memory, hardware bus fabrics, and Unicorn/Verilator instruction models.

## Files

| File | Make Target | Description |
|------|-------------|-------------|
| `check-mcu-shared-kernel.py` | `make check-mcu-shared-kernel` | Instruction model test for dual-core MCU shared RAM and hardware gate arbitration |
| `check-mcu-shared-rtl.py` | `make check-mcu-shared-rtl` | Dual Cortex-M3 firmware execution against compiled Verilator shared fabric |
| `check-mcu-program.py` | `make check-mcu-program` | Production native application execution, SVC handling, and scheduler instruction tests |
