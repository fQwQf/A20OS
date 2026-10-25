# x86 hardware hypervisor backends

The dispatch layer selects AMD SVM/NPT or Intel VMX/EPT after checking the
features used by the implementation. Both backends execute real guest
instructions and own independent state for every vCPU. VMX requires
unrestricted guests, four-level write-back EPT, all-context INVEPT and the VMX
preemption timer;
firmware must permit VMX outside SMX. SVM requires NPT and firmware SVMDIS clear.

Hardware definitions follow the [Intel SDM, volume 3C, chapters 24–31](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
and [AMD64 APM, volume 2, chapter 15 and appendix B](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2).

## Guest contract

Fresh vCPUs start in flat 32-bit protected mode, unpaged, CPL 0, CS=8,
data selectors=16, zero segment bases, 4 GiB limits and IF=0. Entry must fit
32 bits. RAM starts at GPA 0x100000. ESI holds the opaque boot-data GPA and EDI
the virtual CPU id. The guest supplies its stack, GDT/IDT and device model.
`regs[]` follows instruction register numbering (RAX, RCX, RDX, RBX, RSP, RBP,
RSI, RDI, R8…R15). `pc` is RIP. Exit fields retain raw hardware diagnostics.

Use VMMCALL on SVM or VMCALL on VMX:

- EAX=1, EBX=byte writes the console and returns EAX=0.
- EAX=8 requests VM shutdown.
- EAX=9, EBX=CPU id, ECX=entry GPA, EDX=opaque starts a stopped vCPU.
- EAX=10 stops this vCPU, which can subsequently be restarted.
- EAX=11, EBX=CPU id returns its lifecycle state.
- HLT advances RIP and yields.

Both backends expose a restricted CPUID model with legacy FPU/SSE and no
XSAVE/AVX, APIC, PMU or nested virtualization. Unknown hypercalls, I/O,
MOV DR and unsupported MSRs fault the guest. SVM has emulation for paging
transitions and syscall/base MSRs; VMX currently deliberately exposes only
the 32-bit ABI, EFER.SCE, PAT reads and SYSENTER MSRs. VMX rejects LME/NXE
writes rather than exposing unbanked long-mode syscall state. Full 64-bit OS
boot is outside the verified contract.

## State, concurrency and translation

SVM owns eight pages per vCPU: VMCBs, HSAVE and intercept maps. VMLOAD/VMSAVE
bank segment/syscall state. Assembly banks GPRs and FP/SSE; the backend also
preserves host CR2/debug/control state and restores host GS before C.

VMX owns four pages per vCPU: VMXON, VMCS and aligned software state.
VMCS host fields restore segment bases, GDT/IDT/TR, CRs, EFER, PAT and SYSENTER.
Assembly banks GPRs, FP/SSE, CR2 and DR6; put restores host CR0/CR4/debug state.
VMCLEAR before VMXOFF releases VMCS ownership on every put; the next physical
CPU can load it and relaunch with refreshed host fields, retaining guest state.

A VM supports up to CONFIG_NR_CPUS concurrent vCPUs. Each VMRUN flushes all
ASIDs; every VMX launch/resume performs all-context INVEPT. Stage-2 additions
are allowed while other vCPUs run. Remote unmap is rejected until active
vCPUs stop, so backing is never freed with a remote stale translation. A
shared VM requires managed console mode to bypass the legacy single-runner
console adapter. There is no virtual APIC or guest interrupt injection yet;
SMP uses the explicit start/stop ABI and shared memory.

Physical interrupt exits, the VMX preemption timer and a bounded host LAPIC
deadline preempt a guest
running CLI; JMP . independently of guest IF. Repeated short entries preserve
earlier timer deadlines.

## Running

SVM runs with QEMU `-cpu max -accel tcg` or AMD `-cpu host -accel kvm`:

```sh
make ARCH=x86_64 BRINGUP=1 NR_CPUS=2 kernel-only -j8
# Run pc machine, smp=2, 1 GiB RAM with kernel.elf. Supply both arguments
# through -append and -fw_cfg name=opt/x86/cmdline,string=...:
# a20.hyp_test=1 a20.hyp_smp=1
```

The host used during development is AMD, and QEMU TCG does not implement
Intel VMX. VMX is therefore executed with the upstream Bochs 3.0 software CPU:

```sh
curl -L https://github.com/bochs-emu/Bochs/archive/refs/tags/REL_3_0_FINAL.tar.gz -o bochs.tar.gz
tar xf bochs.tar.gz
cd Bochs-REL_3_0_FINAL/bochs
./configure --enable-vmx=2 --enable-smp --enable-x86-64 --with-nogui --disable-docbook --enable-debugger --enable-disasm
make -j8
# Return to A20OS, use the fresh CONFIG_NR_CPUS=2 kernel.bin:
python3 kernel/arch/x86_64/hyp/check_vmx_bochs.py --bochs-dir /path/to/Bochs-REL_3_0_FINAL/bochs --kernel /path/to/kernel.bin --cpus 2
```

The runner boots the real Bochs BIOS, then a minimal protected-mode Multiboot
loader. It uses debugger loadmem in 4 KiB chunks to avoid Bochs 3.0's missing
address increment in multi-chunk loadmem. The CPU has 1g_pages added because
the existing A20OS early direct map uses 1 GiB pages. Serial and emulator
logs are retained under .kernel-build/vmx-bochs. Bochs reports its ACPI power
off as process exit 1; the runner verifies guest/host markers independently.
The smoke checks stage-2 ownership, three output/shutdown lifecycles,
demand-fill, executable/write permission denial (NX/RO), timer preemption and
STOP. SMP additionally requires two pinned
host workers concurrently owning RUN, shared-memory rendezvous and clean
CPU_OFF from both guests. VMX software execution is not a substitute for
performance validation or an Intel hardware test.
