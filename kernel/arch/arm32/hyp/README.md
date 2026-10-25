# ARMv7 HYP backend

`arm32-hyp` requires ARMv7 virtualization extensions and an initial handoff in
HYP mode. QEMU `virt,virtualization=on -cpu cortex-a15` provides that handoff.
A host booted directly in SVC, including `virtualization=off`, reports the
backend unavailable. NOMMU builds keep the backend unavailable.

The host remains in SVC, using its original short-descriptor stage-1 MMU.
A physical-address HYP monitor runs with its own MMU disabled. It keeps an
independent stack and activation pointer for every physical CPU. Guest stage-2
uses 64-bit LPAE descriptors, a 32-bit IPA window, three levels, 4 KiB pages,
AF=1 and independent S2 read/write permissions. All guest S1/S2 translations
are invalidated before every entry, including resumption after demand fill.
VMID 0 is deliberately reused only with this full invalidation.

The monitor captures r0-r12, PC/CPSR, all PL1 banked registers, the stage-1
translation/control/fault/TLS registers, VFP/NEON d0-d31 and FP control state.
It restores the host bank before returning to SVC. Physical interrupts route to
HYP while the guest runs and return a resumable exit; the host processes the
pending interrupt after RUN has released its activation. Guest access to the
physical timer, PMU, CP14 debug registers and implementation coprocessor state
is trapped or denied. Guest FP, virtual timer state and virtual MPIDR are private.

HVC/SMC dispatch provides the smoke console, shutdown and PSCI 1.1 VERSION,
CPU_ON, CPU_OFF, AFFINITY_INFO, FEATURES, SYSTEM_OFF and SYSTEM_RESET. CPU_ON
resets a stopped vCPU in ARM SVC mode with r0=context; Linux primary boot uses
r0=0, r1=~0, r2=DTB. PL011 MMIO supports polling output and empty RX. There is no
virtual GIC or virtual external interrupt delivery, and these tests do not
establish complete guest OS boot support.

The QEMU virt host supports up to eight GICv2 CPU targets. Host SMP uses PSCI
CPU_ON, a separate physical secondary entry, per-CPU SVC and
exception stacks, banked GIC CPU interfaces and reschedule SGIs. The initial raw
ARM image relocates from QEMU's raw boot load address to the linked physical
address, preserving r2 so boot arguments can be read from the DTB. ELF boot is
still supported, but QEMU does not supply its raw Linux boot arguments there.

## Runtime checks

```sh
make ARCH=arm32 BRINGUP=1 NR_CPUS=2 kernel-only -j8
qemu-system-arm -machine virt,virtualization=on -cpu cortex-a15 -accel tcg \
  -m 1G -nographic -smp 2 -kernel PATH/kernel.bin \
  -append 'a20.hyp_test=1 a20.hyp_smp=1'
```

The positive run must show `HYP_SELFTEST: PASS`, three guest output rounds,
`preemption+stop PASS`, and `HYP_SMP_TEST: PASS backend=arm32-hyp cpus=0,1 peak=2`.
The SMP guests write distinct ready words and stay active on both host CPUs
until the controller releases them, then both use PSCI CPU_OFF. The controller
checks zero resource deltas after destruction. `virtualization=off` must show
`HYP_ARCH_TEST: SKIP (backend unavailable)` and shut down normally.

These checks passed under QEMU TCG on 2026-10-25. They establish the tested entry,
mapping, lifecycle, host preemption and parallel execution paths. No physical
ARM board, virtual GIC, full operating-system guest, or long-running workload
was validated.

Architecture reference: [Arm ARMv7-A/R manual DDI 0406C.d](https://documentation-service.arm.com/static/5f8daeb7f86e16515cdb8c4e).

The final validation also includes the highest legal GPA root slot, real
NX/read-only guest faults, and the shared parallel rendezvous after permission
fault cleanup.
