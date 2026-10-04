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

#endif /* _HYP_VCPU_H */
