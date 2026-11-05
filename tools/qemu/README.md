# QEMU & Firmware Boot Verification Utilities (`tools/qemu/`)

This directory contains standalone test harnesses and verification runners that execute under the QEMU emulator or validate guest boot artifacts, firmware handoffs, and multi-guest configurations.

## Tools & Runners

| Script | Purpose | Supported Architectures |
|--------|---------|-------------------------|
| `check_uefi_boot.py` | Standalone x86 UEFI loader verification under OVMF with clean S5 poweroff check | x86_64 |
| `check_uefi_framebuffer.py` | Native OVMF GOP framebuffer, fbdev RGB24 rendering, and HDA PCM audio via QMP screendump | x86_64 |
| `check_uefi_world.py` | Full UEFI world image verification across AHCI/USB storage, GOP display, and USB input | x86_64 |
| `check_dt_framebuffer.py` | Device Tree simple-framebuffer reservation and userspace scanout rendering validation | AArch64, ARM32 |
| `check_arm32_shell.py` | Diskless ARM32 RAMFS interactive shell, child lifecycle, timers, and poweroff runner | ARMv7-A (arm32) |
| `check_serial_shell.py` | Multi-architecture serial shell oracle verifying command output, FPU state, timers, and sync | ARM32, RISC-V, x86_64 |
| `check_hyp_multiarch.py` | Hardware hypervisor extension smoke runner across supported ISAs (TCG/KVM) | AArch64, ARM32, RISC-V 32/64, x86_64, LoongArch 64 |
| `check-smp-native.py` | Multi-core SMP functional selftest verifying thread migration, IPIs, timer IRQs, and reap | AArch64, RISC-V 64, x86_64 |
| `check-multi-qemu-kernel.py` | Multi-instance QEMU execution runner for shared kernel, cluster threads, and storage workloads | x86_64 |
| `ckpt_svc_verify.py` | Post-run validator for service checkpoint/kexec/restore monotonic counter continuity | RISC-V 64 |
