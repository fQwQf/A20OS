# RV32 H backend

The host runs in HS mode and executes native RV32 VS guests. Availability
requires H and Sstc in the firmware DTB and firmware enabling STCE. It returns
an error if the per-hart STCE permission is absent.

The Sv32x4 G-stage root is one aligned order-2 allocation with 4096 native
32-bit PTEs and a 34-bit GPA limit. Each of its four pages owns a normal
1024-entry metadata block. Guest backing PA remains limited by the host's
32-bit `paddr_t`; guest page-table entries can address the full GPA range.

Guest entry and traps preserve zero-extended XLEN GPRs in the existing
64-bit public slots, full 64-bit FP registers, FP CSR, VS supervisor CSRs,
and the `vstimecmp`/`vstimecmph` pair. Guest software walks use Sv32.
Each RUN invalidates both translation stages. Host SBI RFENCE performs
synchronous remote HFENCE for active harts; guest SBI implements HSM, IPI,
TIME, and instruction/VS translation fences through the common VM lifecycle.

The QEMU RV32 board now starts host secondary harts through SBI HSM with a
separate entry and per-CPU stacks. Emulated 64-bit atomics serialize across
harts as well as masking local interrupts. The C park/wait implementation is
used; the optional Rust RV32 park/wait adapter remains single-core.

Reference: [RISC-V Hypervisor specification](https://docs.riscv.org/reference/isa/priv/hypervisor).

## Runtime verification (2026-10-25)

Built with the default warnings-as-errors policy and the installed
`riscv64-unknown-elf-gcc` RV32 multilib. The QEMU board is now included in the repository SMP admission list:

```sh
make ARCH=riscv32 BRINGUP=1 NR_CPUS=2 kernel-only -j12
qemu-system-riscv32 -machine virt -cpu rv32,h=true -bios default \
  -m 1G -smp 2 -nographic -kernel PATH/kernel.elf \
  -append 'a20.hyp_test=1 a20.hyp_smp=1'
```

Replace `PATH/kernel.elf` with the directory printed by the build. The tested
firmware was QEMU's default OpenSBI v1.6 (SBI 2.0). It enabled RV32 STCE and
provided the HSM and RFENCE calls. Both host harts came online. The kernel
passed three guest create/execute/shutdown rounds with second-page demand
fill, host-timer preemption of a tight-loop guest, STOP, and two concurrent
vCPUs pinned to host CPUs 0 and 1. Both guest harts reached the shared ready
barrier, the VM recorded `peak=2`, and both exited through HSM hart-stop.
The test also checked VM/vCPU/backing/table accounting after destruction.
Three further complete QEMU invocations passed the same SMP smoke.

Using `-cpu rv32,h=false` with the same two-hart kernel produced
`HYP_ARCH_TEST: SKIP (backend unavailable)` and normal host shutdown.
Logs are retained in `.kernel-build/smoke/hyp-riscv32-*.log` and may be
regenerated with the commands above. No full RV32 guest OS boot, external
virtual interrupt controller, physical-board execution, or more-than-two
concurrent host harts was validated in this pass. MMIO floating-point
accesses are rejected explicitly.

The final validation also includes the highest legal GPA root slot, real
NX/read-only guest faults, and the shared parallel rendezvous after permission
fault cleanup.
