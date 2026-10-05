#ifndef _HYP_VCPU_H
#define _HYP_VCPU_H

/*
 * FROZEN CONTRACT for the vcpu slice -- the interface the three parallel
 * implementations (arch/trap assembly, kernel run loop, user API) code
 * against.  Layout here is load-bearing: the enter/exit assembly indexes
 * regs[]/arch[] by fixed offsets, so renaming or reordering a field is an
 * ABI break across three work packages, not a refactor.  If something here
 * is wrong, escalate and change it in ONE place before anything codes
 * against it -- never fork it per agent.
 *
 * Design record: docs/hypervisor/00-design.md S5.  Run shape (decided, do
 * not redesign per agent):
 *
 *   hyp_vcpu_run() [C, kernel/hyp/hyp_vcpu.c]
 *     -> hyp_arch_vcpu_setup()   program hstatus/hedeleg/hideleg, stage hgatp
 *     -> hyp_arch_vcpu_enter()   [asm] save host callee-saved regs into
 *                                vcpu->arch[], build a guest trap frame from
 *                                vcpu->regs/pc on the kernel stack, and take
 *                                the kernel's normal trap-return path: sret
 *                                with hstatus.SPV=1 lands in VS-mode
 *     guest runs; EVERY guest trap enters the kernel's standard trap vector
 *     (stvec), which saves the live (guest) registers into a normal trap
 *     frame; the trap dispatcher sees hyp_arch_guest_trap() true and hands
 *     the frame to the C run-loop handlers, which either
 *       a) fix the frame up (advance sepc past an SBI ecall, demand-map a
 *          second-stage fault) and take the normal trap-return -- sret goes
 *          straight back into the guest, or
 *       b) on terminal exit (guest SBI shutdown, fatal fault), record
 *          vcpu->exit, restore the host context from vcpu->arch[] and return
 *          from hyp_arch_vcpu_enter()'s call site; hyp_vcpu_run() then
 *          returns to its caller like any syscall.
 *   Host interrupts during guest execution trap to HS the same way: the IRQ
 *   handlers run against the guest frame and the trap-return resumes the
 *   guest.  A preemption of the host task mid-guest is just a context switch
 *   around a stack frame -- nothing special.
 */

#include "hyp/hyp.h"

/* arch[] layout, pinned for the assembly (riscv64):
 *   [0]=ra [1]=sp [2]=gp [3]=tp [4..10]=t0-t6 [11..22]=s0-s11
 * 24 slots so widening by one register never moves another field. */
#define HYP_VCPU_ARCH_U64 24

/* tramp[] layout, pinned for the assembly on the same terms as arch[] (the
 * numbers live in hyp_asm_offsets.h).  It is the guest trap prelude's bank:
 * host memory, so the prelude can write it with no register to spare and with
 * no guest page it would first have to fault in.  See the field. */
#define HYP_VCPU_TRAMP_U64 35

typedef enum hyp_exit_reason {
    HYP_EXIT_NONE = 0,     /* has not run to an exit yet */
    HYP_EXIT_SHUTDOWN,     /* guest executed the legacy SBI shutdown call */
    HYP_EXIT_FAULT,        /* fatal guest fault; detail in exit_* fields */
    HYP_EXIT_ERROR,        /* internal error: never make this the happy path */
} hyp_exit_reason_t;

typedef struct hyp_vcpu {
    uint32_t magic;
#define HYP_VCPU_MAGIC 0x48564350 /* 'HVCP' */
    /* Set while a guest is live on this vcpu -- the trap dispatcher's
     * "is this a guest trap" test.  One guest at a time in this slice. */
    int      running;
    hyp_vm_t *vm;
    int      refcount;

    /* ---- asm-visible layout: offsets pinned, see hyp_vcpu_asm.S ---- */
    uint64_t regs[32];  /* [0]=x0 (kept 0) ... [31]=x31 */
    uint64_t pc;        /* guest entry / resume sepc */
    uint64_t arch[HYP_VCPU_ARCH_U64];

    /* Guest trap bank, APPENDED after arch[] so the three offsets above keep
     * the values the assembly was frozen against (hyp_asm_offsets.h pins all
     * four with _Static_assert in hyp_arch.c).  This is the KVM-riscv shape
     * for a trap taken out of VS-mode: the live registers are the guest's,
     * and the prelude that banks them has to reach host memory without a GPR
     * to load an address with and without a guest page it can safely touch --
     * neither is available, because a guest stack is unmapped at the guest's
     * own first fault.  sscratch carries the bank pointer (programmed by
     * hyp_arch_vcpu_enter, re-armed by hyp_arch_guest_trap on every resume),
     * so the prelude spends no register and touches no guest memory:
     *
     *   [0] host sp   -- what __trap_from_kernel builds its frame on
     *   [1] host tp   -- the sp guard dereferences tp before anything else
     *   [2] guest sp  -- stolen out of sscratch by the prelude; x[2] is not
     *                   recoverable from the frame, which carries the host's
     *   [3+n] guest x[n], n = 1..31
     *
     * [0] and [1] are refreshed by hyp_arch_vcpu_enter and by the resume
     * path; [2] and [3+n] are written by the prelude on every guest trap. */
    uint64_t tramp[HYP_VCPU_TRAMP_U64];
    /* ---- end asm-visible ---- */

    /* Exit detail, valid after hyp_vcpu_run() returns. */
    hyp_exit_reason_t exit;
    uint64_t exit_scause;
    uint64_t exit_stval;   /* faulting guest VA */
    uint64_t exit_htval;   /* second-stage fault: guest GPA >> 2, raw */
} hyp_vcpu_t;

/* ---- kernel C API (kernel/hyp/hyp_vcpu.c implements) ---- */

/* Create a vcpu over vm with its entry pinned to entry_gpa (the demo guest
 * runs at 0x80000000).  Takes one reference on vm. */
hyp_vcpu_t *hyp_vcpu_create(hyp_vm_t *vm, uint64_t entry_gpa);
void        hyp_vcpu_put(hyp_vcpu_t *vcpu);

/* Copy len bytes into guest memory starting at gpa, allocating and lending
 * the backing frames (kernel-owned guest RAM for this slice; a shared
 * zero-copy VMO attach is a later slice and must not be improvised here).
 * gpa and len must be page-consistent (gpa page-aligned; len rounds up). */
int  hyp_vm_load(hyp_vm_t *vm, uint64_t gpa, const void *bytes, uint64_t len);

/* Run the guest until a terminal exit.  Returns 0 when the guest exited
 * cleanly (vcpu->exit says why; HYP_EXIT_SHUTDOWN is the demo's success
 * case) and a negative errno on internal failure.  Not reentrant: one
 * running guest kernel-wide in this slice, enforced, not assumed. */
int  hyp_vcpu_run(hyp_vcpu_t *vcpu);

/* ---- arch-provided (kernel/arch/riscv64/hyp/) ---- */

/* Program what the guest entry needs: hstatus (SPV=1, SP2P=1, VTVM=1,
 * HU=0), hedeleg=0 / hideleg=0 (everything intercepts to HS in this
 * slice), and stage vm's hgatp value so enter can program it.  Returns 0
 * or -ENOTSUP when the CPU has no H extension. */
int  hyp_arch_vcpu_setup(hyp_vcpu_t *vcpu);

/* ASM: save host callee-saved registers into vcpu->arch[] per the pinned
 * layout, build the guest trap frame from vcpu->regs/pc, and enter the
 * guest via the kernel trap-return path.  Returns only through the exit
 * path (someone restored arch[] and returned for it). */
void hyp_arch_vcpu_enter(hyp_vcpu_t *vcpu);

/* C, called from the trap dispatcher for every trap while the dispatcher
 * cannot otherwise tell whose frame it is looking at.  Returns 1 when the
 * frame belongs to a running guest and was handled (fixed up for resume,
 * or exited the guest), 0 when the trap is the host's own and must be
 * handled by the host paths untouched. */
int  hyp_arch_guest_trap(void *trap_frame);

/* ---- the arch/run-loop seam (both sides code against these) ---- */

/* The vcpu whose guest is live kernel-wide right now, or NULL.  Single
 * running guest in this slice, enforced by hyp_vcpu_run(), not assumed. */
hyp_vcpu_t *hyp_vcpu_active(void);

/* Implemented by kernel/hyp/hyp_vcpu.c; called by the arch trap dispatch
 * for every guest trap (the arch side has already copied the trap frame's
 * GPRs into vcpu->regs and pc).  Returns 1 when the guest resumes (the
 * arch side then takes the normal trap-return with the updated frame) and
 * 0 when the guest exited to the host (arch[] already restored).  Causes
 * handled here: legacy SBI ecall (putchar/shutdown), second-stage page
 * fault (detail recorded, no silent on-demand fill), host IRQ (handled by
 * the host IRQ machinery, guest resumed), anything else = HYP_EXIT_FAULT. */
int  hyp_vcpu_handle_trap(hyp_vcpu_t *vcpu, void *trap_frame,
                          uint64_t scause, uint64_t stval, uint64_t htval);

/* Guest-visible legacy SBI calls the run loop must serve (everything else
 * in the ecall handler is HYP_EXIT_FAULT, not improvisation):
 *   a7=0x01 console_putchar(a0=char)  -> kernel console, resume guest
 *   a7=0x08 shutdown()                -> HYP_EXIT_SHUTDOWN
 * Guest RAM backing is pre-loaded through hyp_vm_load(); a second-stage
 * fault on a GPA nothing loaded is HYP_EXIT_FAULT with detail, never a
 * silent on-demand fill. */
#define HYP_SBI_CONSOLE_PUTCHAR 0x01
#define HYP_SBI_SHUTDOWN        0x08

/* ================================================================
 * v2 -- A20OS as guest (real kernel, two-stage MMU, delegated traps).
 * Design record: docs/hypervisor/01-a20os-guest.md.  These additions
 * supersede the v1 "intercept everything" shape where they conflict.
 * ================================================================ */

/* The guest RAM window.  A second-stage fault on a GPA inside
 * [base, base+size) demand-fills one zeroed host frame and maps it RWX
 * (stage-2 has no U bit); outside the window, the device-model table
 * decides (hyp_dev_mmio).  hyp_vm_load()d pages stay eagerly mapped; the
 * window covers what the guest allocates for itself (page tables, bss,
 * stacks).  Default window when never set: [0x80000000, mem_size). */
int  hyp_vm_set_ram(hyp_vm_t *vm, uint64_t base, uint64_t size);

/* Device model, one guest MMIO access (store=1 writes *value, store=0
 * reads into *value; len is the access width in bytes).  Returns 1 when
 * handled (guest resumes), 0 when the address is unknown (caller exits
 * with HYP_EXIT_FAULT).  Owned by kernel/hyp/hyp_dev.c.  Required
 * members, so the smoke can gate on real behaviour:
 *   - qemu-virt 16550 UART [0x10000000, 0x10001000): THR writes go to the
 *     host console and count into the guest console byte counter; LSR
 *     reads return 0x60 (THR+TSR empty); IER/FCR etc. store silently.
 *   - CLINT mtime [0x2000000, 0x2010000): reads return the host timebase
 *     (a guest with a frozen clock spins forever in its delay loops);
 *     other CLINT registers RAZ/WI.
 *   - everything else RAZ/WI with a per-page counter (the guest's PLIC
 *     init must not wedge it; interrupts are delegated, not routed). */
int  hyp_dev_mmio(hyp_vm_t *vm, uint64_t gpa, int store, uint64_t *value,
                  int len);

/* The arch/run-loop seam for second-stage faults (implemented in
 * kernel/hyp/hyp.c, called by the trap handler in hyp_vcpu.c): demand-fill
 * one zeroed frame and map it, when gpa is inside the RAM window.  0 =
 * filled, -EFAULT outside the window, -ENOMEM on exhaustion.  Charges the
 * current task's cgroup. */
int  hyp_ram_fill(hyp_vm_t *vm, uint64_t gpa);

/* Delegation (hyp_arch_vcpu_setup v2): guest OS handles its own traps.
 *   hedeleg = every exception except ecall-from-VS (scause 10) --
 *             the guest page-faults its own user processes; the exact
 *             mask follows the priv-spec exception table, arch side owns it;
 *   hideleg = HYP_HIDELEG_DEFAULT -- the guest runs its own ISR dispatch
 *             from vs tvec; WFI in the guest wakes on pending delegated
 *             interrupts, so no WFI emulation is needed.
 * VS-mode CSR accesses (satp/sie/stvec/...) hit the VS variants in
 * hardware -- no interception, nothing to save while one guest runs. */
/* Bits 2/6/10 = MIP_VSSIP/VSTIP/VSEIP, the VS-level interrupt file.  NOT
 * 1/5/9: those are MIP_SSIP/STIP/SEIP, the S-level bits, and they are
 * read-only zero in hideleg (priv spec, "Hypervisor Trap Delegation"), so
 * a mask built from them reads back as 0 and delegates nothing at all.
 * QEMU agrees and does it in software: rmw_hideleg64 ANDs every write with
 * vs_delegable_ints = (VS_MODE_INTERRUPTS | LOCAL_INTERRUPTS) & ~MIP_LCOFIP
 * (target/riscv/csr.c:1779), and VS_MODE_INTERRUPTS is exactly 2/6/10
 * (cpu_bits.h:790).  Hardware then renumbers VS causes 10/6/2 to what VS-mode
 * sees as 9/5/1, so nothing on the guest side changes. */
#define HYP_HIDELEG_DEFAULT (((uint64_t)1 << 2) | ((uint64_t)1 << 6) | \
                             ((uint64_t)1 << 10))

/* Guest boot protocol: the kernel enters VS-mode with a0=hartid,
 * a1=DTB GPA (the loader copied the host's own FDT blob into guest
 * memory; if the host kept no pointer, the loader builds a minimal FDT
 * with chosen/bootargs). */
int  hyp_vcpu_set_boot(hyp_vcpu_t *vcpu, uint64_t hartid, uint64_t dtb_gpa);

/* Guest console marker: the device model scans every guest UART byte
 * against `marker` (<= 32 bytes) -- this is how the boot smoke asserts
 * "the guest reached its banner" without trusting host/guest log
 * interleaving on one shared console. */
void hyp_vm_set_marker(hyp_vm_t *vm, const char *marker);
int  hyp_vm_marker_seen(hyp_vm_t *vm);

/* Guest console byte count since run start (for the smoke's report). */
uint64_t hyp_vm_console_bytes(hyp_vm_t *vm);

#endif /* _HYP_VCPU_H */
