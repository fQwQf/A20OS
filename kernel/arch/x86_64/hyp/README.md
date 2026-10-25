# x86 SVM backend

This backend executes guests with AMD SVM and four-level nested page tables
(NPT). Intel VMX is not implemented. `hyp_supported()` requires SVM, NPT,
FXSAVE/SSE, at least two ASIDs, an MMU build, and firmware that has not set
`VM_CR.SVMDIS`. It does not report support just because CPUID names a feature.
The implementation has been exercised with QEMU TCG and nested AMD KVM.

Hardware definitions follow [AMD64 Architecture Programmer's Manual, Volume 2,
publication 24593](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2), chapter 15
(SVM), the nested-paging section, and appendix B (VMCB layout). Hardware field
byte offsets are recorded in `hyp_svm.h` rather than relying on C packing.

## Boot and register contract

A fresh vCPU starts in flat 32-bit protected mode, with paging disabled, CPL 0,
CS selector 8, data selectors 16, zero segment bases and 4 GiB limits. Entry must
fit in 32 bits. RAM starts at GPA `0x100000`. The loader must supply a stack and
any GDT/IDT needed by its guest; no PC firmware or multiboot handoff is supplied.
`set_boot` passes the opaque boot-data GPA in ESI and the virtual CPU id in EDI.

`regs[]` uses instruction-register numbering: RAX, RCX, RDX, RBX, RSP, RBP, RSI,
RDI, R8 through R15. `pc` holds guest RIP. VM exits record the raw SVM EXITCODE,
EXITINFO1 and EXITINFO2 in `exit_scause`, `exit_stval` and `exit_htval`.

The guest ABI is:

- `VMMCALL`, EAX=1, EBX=byte: write the virtual console, return EAX=0.
- `VMMCALL`, EAX=8: request a clean shutdown.
- `HLT`: advance RIP and return a resumable yield.
- An unrecognized hypercall, port I/O, debug-register access or unsupported MSR
  produces a guest fault. Host I/O and MSRs are never passed through.

CPUID presents a small legacy/SSE CPU model without APIC, PMU, SVM or XSAVE/AVX.
The backend emulates EFER, syscall/sysenter MSRs, FS/GS bases and CR4 writes.
A guest may configure its own page tables; the instruction decoder understands
legacy, PAE and four-level guest paging. OSXSAVE and unimplemented CR4 features
are rejected. Full 64-bit OS boot and paging-mode transitions are not covered
by the current smoke tests.

## Ownership and scheduling

Each vCPU owns eight contiguous pages for the guest/host VMCBs, hardware host
save area, and intercept permission maps. Both maps start with all accesses
intercepted. VMLOAD/VMSAVE bank segment and syscall state; software banks GPRs,
legacy FPU/SSE state, CR2, and host debug registers. Host GS is restored before
returning to C. Backend pages are freed by the vCPU destroy callback.

NPT leaves own guest frames through the common stage-2 implementation. Every
VMRUN flushes all ASIDs, including after migration or a local demand fill. A
VM currently supports one vCPU; stage-2 updates from a different active CPU are
rejected. Execute-only NPT mappings are rejected because NPT cannot encode them.

Physical interrupts use host IF independently of guest IF. Entry uses
CLGI/STI/VMRUN/CLI, restores host state, then STGI. A host LAPIC one-shot deadline
of at most 10 ms ensures even `CLI; JMP .` returns control. Repeated short runs
preserve earlier timer deadlines. Guest IRQ injection, SMP, a virtual APIC,
PC device emulation and arbitrary Linux/Windows boot are not implemented.

## Verification

Build and run the common hardware test from the repository root:

```sh
make ARCH=x86_64 BRINGUP=1 kernel-only -j8
python3 tools/check_hyp_multiarch.py --arch x86_64 --kernel PATH/kernel.elf --accel tcg
python3 tools/check_hyp_multiarch.py --arch x86_64 --kernel PATH/kernel.elf --accel kvm
python3 tools/check_hyp_multiarch.py --arch x86_64 --kernel PATH/kernel.elf --accel tcg --without-virtualization
```

The opt-in `a20.hyp_test=1` test checks NPT mapping, auditing and ownership;
three guest console/shutdown lifecycles; a demand-filled second page accessed
at offset 4; and timer preemption plus STOP of an interrupts-disabled infinite
loop. Disabling SVM must report SKIP and preserve normal host power-off.
