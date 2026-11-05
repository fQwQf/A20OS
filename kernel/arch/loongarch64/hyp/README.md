# Experimental LoongArch64 LVZ backend

This backend is **not accepted as working LVZ support**. No LVZ-capable
machine or nested LVZ KVM accelerator is available in this workspace.
QEMU TCG currently reports CPUCFG2.LVZ=0 and implements neither the guest CSR
bank nor HVCL entry/exit. Compilation and the unavailable-backend negative
control pass; native guest execution, isolation, preemption, and guest SMP
on LVZ hardware have not been validated.

Availability remains closed by default. A future hardware experiment must
explicitly provide `a20.lvz_experimental=1`. The backend also requires actual
CPUCFG2.LVZ, LVZ version 1, exactly eight scratch CSRs, a host MMU, and root
control of memory attributes. CPUCFG is checked before any LVZ CSR or
instruction is touched. Opt-in does not override an absent hardware feature.

The implementation contains an actual native entry/exit path:

- A per-vCPU integer/guest-CSR bank and independent assembly context preserve
  guest GPRs before touching guest sp/tp. Root KS6/KS7 provide the exit channel.
  The exit restores host callee-saved registers, EUEN, EENTRY, ECFG, PGDL,
  PRMD/ERA, GSTAT/GCFG/GTLBC/GINTC/GCNTC, and the root scratch registers.
- The GPA tree uses native 64-bit LoongArch descriptors and existing metadata
  with the board's three- or four-level page-walk profile. Entry selects the
  tree through root PGDL, uses GID/TGID 1, and invalidates both guest translation
  stages on every hardware entry. The existing root TLB refill handler walks
  this tree; the backend does not substitute a software CPU emulator.
- ERTN enters guest mode through GSTAT.PVM. A separate root EENTRY captures
  exit status, BADV/BADI and guest PC. Missing GPA pages demand-fill; NR/D/NX
  permissions are checked separately from concurrent page publication.
- The root EUEN is zero for the entire guest interval. FPU, LSX, LASX and LBT
  accesses therefore trap before exposing host extension-register contents;
  these guest facilities are unsupported, and their CPUCFG bits are hidden.
- Root host-timer interrupts return a RUN slice. Software/external IRQ requests
  have initial guest CSR paths. Parallel vCPU admission and shared GPA mappings
  are wired through the common SMP lifecycle, but are untested on LVZ.

The small smoke ABI is A20-specific `HVCL 0`: a7=1 outputs a0, a7=8 shuts
its VM down, and a7=9 stops the calling hart. `hyp_test_guest.c` contains
linked LoongArch machine code for output/shutdown, second-page demand fill,
a tight loop, and the common two-hart ready/release rendezvous. These payloads
are compiled inputs for a future positive control, not locally executed tests.
This does not implement the Linux KVM paravirtual ABI.

Guest timer programming, guest FP/vector/LBT, IOCSR/MMIO devices, cache
maintenance, remote guest TLB shootdown, nested LVZ, complete OS boot and
physical-board execution are outside the implemented experiment. A guest
that enables TCFG is stopped with a fault and its timer is disabled on exit;
its timer phase is not silently approximated. Unimplemented privileged exits
are faults. The common stage-2 code rejects unmap while a remote CPU can
retain translations. No hardware positive, including SMP, is claimed here.

## Local verification

```sh
make ARCH=loongarch64 BRINGUP=1 UART_CMDLINE=y kernel-only -j12
python3 tools/qemu/check_hyp_multiarch.py --arch loongarch64 --build \
  --without-virtualization
```

LoongArch QEMU direct boot does not put `-append` into the DTB used by this
kernel. The UART command-line build option is needed; the runner waits for
the UART command prompt, then sends `a20.hyp_test=1 a20.lvz_experimental=1`
and a newline.
The explicit opt-in negative control must still print
`HYP_ARCH_TEST: SKIP (backend unavailable)` and power off normally. Its log
is `.kernel-build/smoke/hyp-loongarch64-disabled-tcg.log` (with `-smp` after
the architecture name when `--cpus 2` is selected).

The implementation was checked against the official
[LoongArch base architecture manual](https://loongson.github.io/LoongArch-Documentation/LoongArch-Vol1-EN.html)
(CPUCFG and native descriptors), the
[Linux v6.12 LVZ switch](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/v6.12/arch/loongarch/kvm/switch.S),
[guest CSR and vCPU handling](https://kernel.googlesource.com/pub/scm/linux/kernel/git/torvalds/linux/+/v6.12/arch/loongarch/kvm/vcpu.c),
and the corresponding `loongarch.h`, `kvm_csr.h` and `tlb.h` definitions.
Their register protocol informs this implementation; those sources do not
constitute runtime evidence for A20OS.
