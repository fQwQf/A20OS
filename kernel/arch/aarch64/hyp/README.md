# AArch64 EL2 backend

The host remains at EL1. If the boot handoff includes EL2, `entry.S` installs
an MMU-off, physical-address EL2 monitor before dropping to EL1. EL1 calls the
monitor with private `HVC #0xa20`; every guest exit restores host state before
C decodes the exit. Firmware that enters directly at EL1 leaves the backend
unavailable. This backend is disabled for CONFIG_NOMMU.

Implemented:

- AArch64 EL1h guest reset, x0 DTB boot argument, 31 GPRs, SP_EL0/SP_EL1,
  EL1 translation/exception/TLS registers, FP/SIMD and virtual timer state.
- Three-level, 4 KiB stage-2 tables, 39-bit IPA and 32-bit host PA. Multiple vCPUs run concurrently on distinct host CPUs. VMID zero is invalidated on every entry and exit.
- Translation-fault demand fill inside the configured RAM window; stage-2
  permission violations fault rather than using an undefined HPFAR value.
- PL011 polling at IPA 0x09000000, PSCI VERSION/FEATURES/CPU_OFF/SYSTEM_OFF/
  SYSTEM_RESET, SMCCC VERSION. CPU_ON/AFFINITY_INFO use the shared vCPU lifecycle.
- A20 smoke HVC calls: x0=1, x1=byte prints; x0=8 exits cleanly.
- A bounded host CNTP deadline, physical IRQ/FIQ/SError routing to EL2 and
  host yield on IRQ or WFI/WFE, including when guest DAIF masks interrupts.
- Guest CNTP programming, debug/PMU, implementation-defined system register
  accesses and SMC are trapped. Unsupported exits produce a guest fault.

Virtual GIC, virtual interrupt injection and SVE are not implemented.
Guest SMP with shared memory and PSCI start/stop is implemented; the tested
configuration is two vCPUs on two host CPUs. The platform device model is sufficient for small polling
payloads; booting a complete interrupt-driven OS is outside the tested scope.

The state bank occupies a dedicated allocator frame. EL1 cleans/invalidate
cache lines around calls so the MMU-off EL2 monitor never consumes stale
cached state or shares a cache line with unrelated kernel data.

Run the opt-in QEMU test using a raw kernel image so QEMU supplies x0=DTB:

```
make ARCH=aarch64 BRINGUP=1 NR_CPUS=2 kernel-only
python3 tools/check_hyp_multiarch.py --arch aarch64 --cpus 2 --kernel <build>/kernel.bin
python3 tools/check_hyp_multiarch.py --arch aarch64 --cpus 2 --kernel <build>/kernel.bin --without-virtualization
```

The positive test uses `virt,virtualization=on`, verifies mapping/audit/unmap,
executes guests three times with a non-page-aligned store to demand-filled RAM,
then confirms an interrupt-masked infinite guest loop yields and can be stopped.
The negative test uses an EL1-only handoff and requires a clean SKIP.

Architectural references:

- Arm, [Armv8-A virtualization](https://developer.arm.com/-/media/Arm%20Developer%20Community/PDF/Learn%20the%20Architecture/Armv8-A%20virtualization.pdf)
  (EL2 ownership, stage 2, virtual interrupt routing and HPFAR).
- Linux's [Arm64 EL2 definitions](https://android-kvm.googlesource.com/linux/+/6228b38e93d1a7811af1d4ea984a4b65b1d11e05/arch/arm64/include/asm/kvm_arm.h)
  independently cross-check the HCR/VTCR/MDCR field encodings.

The SMP smoke runs two pinned host workers. The guests write separate ready
words in the same demand-filled page and wait for host release, then use PSCI
CPU_OFF. Peak simultaneous RUN ownership must be two and object/page counters
must return to baseline. Native stage-2 permits concurrent zero-to-present
additions; unmap refuses a remote active CPU until its vCPUs have stopped.
NX and read-only mappings are tested by actual denied guest fetch/store.
