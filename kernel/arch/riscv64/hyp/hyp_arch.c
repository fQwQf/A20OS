/*
 * RISC-V H-extension (hypervisor extension) arch half.  The kernel runs in
 * HS-mode, which is exactly the mode this extension arms: hgatp names the
 * stage-2 root, hfence.gvma invalidates guest translations, and hstatus/
 * hedeleg shape what the guest may trap.  The stage-2 memory path
 * (hyp.c) needs only the first two; the vcpu run loop adds the rest here:
 * hyp_arch_vcpu_setup programs the guest's view of the world and
 * hyp_arch_guest_trap is the dispatcher's query for "is this frame a guest".
 */
#include "hyp/hyp_arch.h"
#include "hyp/hyp_vcpu.h"
#include "arch/riscv64/include/platform.h"
#include "arch/riscv64/include/page_table.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "core/trap.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "hyp_asm_offsets.h"
_Static_assert(__builtin_offsetof(hyp_vcpu_t, fpu) == HYP_ASM_VCPU_FPU_OFF, "FPU offset");
_Static_assert(__builtin_offsetof(hyp_vcpu_t, fcsr) == HYP_ASM_VCPU_FCSR_OFF, "FCSR offset");

/* The DTB is the only safe pre-CSR signal available here: QEMU permits reading
 * hstatus even with H disabled, while later H instructions trap fatally.  A
 * truthful firmware declaration therefore gates the CSR probe.  A firmware
 * that falsely advertises H remains outside what this probe can safely detect. */
static int hyp_probe(void)
{
    static int probed = -1;
    int value = __atomic_load_n(&probed, __ATOMIC_ACQUIRE);
    if (value >= 0) return value;
    value = riscv64_fdt_has_isa_extension("h") &&
            riscv64_fdt_has_isa_extension("sstc");
    if (value) {
        uint64_t hstatus;
        __asm__ volatile("csrr %0, hstatus" : "=r"(hstatus));
        (void)hstatus;
    }
    __atomic_store_n(&probed, value, __ATOMIC_RELEASE);
    return value;
}

int hyp_supported(void)
{
    return hyp_probe();
}

/* Sv39x4 root is 16 KiB aligned. VMID 0 is deliberate: until hardware tag
 * width and remote invalidation are implemented, each activation flushes
 * both stages locally before guest entry. */
static uint64_t hyp_make_hgatp(pte_t *root, uint16_t vmid)
{
    /* The PPN field is a PHYSICAL page number.  virt_to_pfn() does not return
     * one: a pfn is an index into its allocator range, equal to (pa >> 12)
     * only when range->base is 0 (include/mm/frame.h:146-172), so packing the
     * raw pfn drops range->base and points the G-stage walk at a page that
     * holds no page table -- the tree is correct and every guest access still
     * reports a second-stage fault.  va_to_pa() is the conversion hyp_s2_walk()
     * already uses for the non-leaf entries it writes. */
    uint64_t ppn = va_to_pa(root) >> PAGE_SIZE_BITS;
    return (8ULL << 60) | ((uint64_t)vmid << 44) | ppn;
}

void hyp_arch_vmid_fenced(uint16_t vmid)
{
    /* Nothing has run under this VMID yet, so no translation can exist --
     * the fence is for the FIRST activation, which programs hgatp anyway.
     * Kept as a hook so VMID reuse (if ever adopted) has one place to
     * fence. */
    (void)vmid;
}

void hyp_arch_s2_fence(uint16_t vmid, uint64_t gpa)
{
    /* hfence.gvma rs1, rs2: rs1 = gpa>>2 (page-granular), rs2 = VMID.
     * Emitted through .insn because the tree-wide -march does not carry
     * the `h` extension and the assembler rejects the mnemonic; .insn is a
     * directive, not an opcode, so it encodes without the extension:
     * R-type, opcode=SYSTEM(0x73), func3=0, func7=0110001, rd=x0. */
    uint64_t page = gpa >> 2;
    __asm__ volatile(".insn r 0x73, 0, 0x31, x0, %0, %1"
                     :: "r"(page), "r"((uint64_t)vmid) : "memory");
}

/* ---- the host CSR and fence hooks (hyp_arch.h) ----
 *
 * The CSR NUMBERS are the whole point of this half.  They are spelled as
 * numbers because the tree-wide -march (rv64imafdc_zicsr_zifencei) carries
 * neither the `h` extension nor Sstc, so neither mnemonic assembles -- the
 * same reason hyp_arch_s2_fence() above goes through .insn.  The generic half
 * keeps those numbers as DECODE data (which CSR number a guest instruction
 * names, kernel/hyp/hyp_vcpu.c: HYP_CSR_SATP/HYP_CSR_VSTIMECMP) and comes here
 * for the act of reading or writing one.
 */

/* vsatp, CSR 0x280: the GUEST's stage-1 root as HS-mode sees it.  Not satp
 * (0x180) -- that is the host's own table, and the trap-return into the guest
 * parks the host's satp and puts vsatp in its place. */
uint64_t hyp_arch_vsatp_get(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, 0x280" : "=r"(v));
    return v;
}

void hyp_arch_vsatp_set(uint64_t satp)
{
    __asm__ volatile("csrw 0x280, %0" :: "r"(satp) : "memory");
}

/* vstimecmp, CSR 0x24d: the Sstc comparator, whose expiry QEMU turns into
 * mip.VSTIP for this hart.  0x24d and not 0x14d because this is the VIRTUAL
 * register; kernel/arch/riscv64/platform/timer.c uses 0x14d for the host's own
 * stimecmp. */
uint64_t hyp_arch_vstimecmp_get(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, 0x24d" : "=r"(v));
    return v;
}

void hyp_arch_vstimecmp_set(uint64_t deadline)
{
    __asm__ volatile("csrw 0x24d, %0" :: "r"(deadline) : "memory");
}

void hyp_arch_host_tlb_fence(void)
{
    /* Emulates a guest SFENCE.VMA while V=0. HFENCE.VVMA orders stores
     * against VS-stage walks; SFENCE.VMA would target the host satp. */
    __asm__ volatile(".insn r 0x73, 0, 0x11, x0, x0, x0" ::: "memory");
}

/* ---- vcpu half: guest entry state and the trap dispatcher's query ---- */

/*
 * hstatus field positions, taken from QEMU's target/riscv/cpu_bits.h
 * (HSTATUS_*) because the tree's -march carries no `h` and therefore knows
 * the CSR names but not this layout; Linux's asm/csr.h agrees on every bit
 * used here.
 */
#define HYP_HSTATUS_SPV  (1ULL << 7)   /* trap-return destination is virtual */
#define HYP_HSTATUS_SPVP (1ULL << 8)
#define HYP_HSTATUS_HU   (1ULL << 9)
#define HYP_HSTATUS_VTW  (1ULL << 21)
#define HYP_HSTATUS_VTVM (1ULL << 20)  /* guest VS-CSR accesses trap to HS */

/* SPV/SPVP select VS supervisor return, VTVM intercepts VS address-space
 * changes, HU remains clear. Bit 10 is reserved; there is no hstatus.VIE. */
#define HYP_HSTATUS_GUEST_ON  (HYP_HSTATUS_SPV | HYP_HSTATUS_SPVP | HYP_HSTATUS_VTVM | HYP_HSTATUS_VTW)
#define HYP_HSTATUS_GUEST_OFF (HYP_HSTATUS_GUEST_ON | HYP_HSTATUS_HU)

/*
 * v2 delegation.  The guest is a real kernel, so it keeps every trap that is
 * its own business and the hypervisor keeps only what has to be the
 * hypervisor's.
 *
 * hedeleg -- every delegatable exception except the ecalls.  Per code:
 *   0 inst addr misaligned, 1 inst access fault, 2 illegal instruction,
 *   3 breakpoint, 4 load addr misaligned, 5 load access fault,
 *   6 store addr misaligned, 7 store access fault   -- the guest's own
 *   8 ecall from U-mode    -- REQUIRED: a guest user process runs at VS-U and
 *                              its ecall must land on the guest's vs tvec,
 *                              not here
 *   12/13/15 inst/load/store page fault             -- the guest page-faults
 *                              its own user processes; this is the whole point
 *                              of running a real kernel as the guest
 *   18 software check (Zicfiss)                     -- the guest's own CFI
 * NOT delegated, and NOT delegable:
 *   9/10/11 ecall -- QEMU refuses these bits outright while V=1
 *     (write_hedeleg masks with vs_delegable_excps, target/riscv/csr.c:1802);
 *     code 10 is what the guest raises for the ecall path this slice serves,
 *     so excluding the whole ecall group is the contract's "except
 *     ecall-from-VS" and reaches further than the single bit 10
 *   20/21/23 guest inst/load/store page fault -- never delegatable (same
 *     mask); these are the second-stage faults hyp_vcpu.c routes
 *   22 virtual instruction fault -- never delegatable, and the code the
 *     VTVM-trapped CSR accesses arrive as
 */
#define HYP_HEDELEG_DEFAULT ((1ULL << 0)  | (1ULL << 1)  | (1ULL << 2)  | \
                             (1ULL << 3)  | (1ULL << 4)  | (1ULL << 5)  | \
                             (1ULL << 6)  | (1ULL << 7)  | (1ULL << 8)  | \
                             (1ULL << 12) | (1ULL << 13) | (1ULL << 15) | \
                             (1ULL << 18))

/* hideleg is the contract's HYP_HIDELEG_DEFAULT (VS-level SSI/STI/SEI, i.e.
 * bits 2/6/10): the guest dispatches its own interrupts from vs tvec and WFI
 * in the guest wakes on whatever is left pending.  The bits QEMU keeps are
 * exactly vs_delegable_ints = (VS_MODE_INTERRUPTS | LOCAL_INTERRUPTS) &
 * ~MIP_LCOFIP (target/riscv/csr.c:1779), and VS_MODE_INTERRUPTS is 2/6/10
 * (cpu_bits.h:790), so every bit of that mask survives the write.  rmw_hideleg64
 * applies the mask by ANDing, not by refusing the write (csr.c:4658-4668), so
 * a mask outside it would read back as 0 -- which is what the S-level bits 1/5/9
 * this macro used to name did, silently delegating nothing. */

/* Terminal exit path: restores the host context from vcpu->arch[] and returns
 * to hyp_arch_vcpu_enter()'s caller.  Declared here rather than in the frozen
 * header because only the arch half -- hyp_arch_guest_trap() -- calls it. */
void __attribute__((noreturn)) hyp_arch_vcpu_exit(hyp_vcpu_t *vcpu);

/* ASM: the guest trap prelude in hyp_vcpu_asm.S.  Installed in stvec while
 * the guest runs, so hyp_arch_guest_trap() has to name it to put it back. */
extern void hyp_guest_trap_entry(void);

/* The assembly hardcodes these; a struct change that moves one of them would
 * corrupt the guest register image instead of failing the build. */
_Static_assert(__builtin_offsetof(hyp_vcpu_t, regs) == HYP_ASM_VCPU_REGS_OFF,
               "hyp_vcpu_asm.S indexes regs[] at HYP_ASM_VCPU_REGS_OFF");
_Static_assert(__builtin_offsetof(hyp_vcpu_t, pc) == HYP_ASM_VCPU_PC_OFF,
               "hyp_vcpu_asm.S indexes pc at HYP_ASM_VCPU_PC_OFF");
_Static_assert(__builtin_offsetof(hyp_vcpu_t, arch) == HYP_ASM_VCPU_ARCH_OFF,
               "hyp_vcpu_asm.S indexes arch[] at HYP_ASM_VCPU_ARCH_OFF");
_Static_assert(__builtin_offsetof(hyp_vcpu_t, tramp) == HYP_ASM_VCPU_TRAMP_OFF,
               "hyp_vcpu_asm.S indexes tramp[] at HYP_ASM_VCPU_TRAMP_OFF");
/* The bank has to be big enough for its own indexing scheme: guest x[31] is
 * tramp[HYP_ASM_TRAMP_X0 + 31].  Without this a shorter HYP_VCPU_TRAMP_U64
 * would still compile and would write the last registers past the field. */
_Static_assert(sizeof(((hyp_vcpu_t *)0)->tramp) / sizeof(uint64_t) ==
                   HYP_ASM_TRAMP_SLOTS,
               "HYP_VCPU_TRAMP_U64 and HYP_ASM_TRAMP_SLOTS disagree");
_Static_assert(HYP_ASM_TRAMP_X0 + 31 < HYP_ASM_TRAMP_SLOTS,
               "tramp[] must hold guest x[31]");

static uint64_t hyp_read_hstatus(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, hstatus" : "=r"(v));
    return v;
}

static void hyp_write_hstatus(uint64_t v)
{
    __asm__ volatile("csrw hstatus, %0" :: "r"(v) : "memory");
}

/* htval carries the guest physical address of a second-stage fault.  It only
 * exists when H is implemented, which is guaranteed by the same probe that
 * makes hyp_vcpu_active() able to name a live guest. */
static uint64_t hyp_read_htval(void)
{
    uint64_t v;
    __asm__ volatile("csrr %0, htval" : "=r"(v));
    return v;
}

int hyp_arch_vcpu_setup(hyp_vcpu_t *vcpu)
{
    if (!hyp_supported())
        return -EOPNOTSUPP;     /* the contract's -ENOTSUP; this tree spells it EOPNOTSUPP */
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC ||
        !vcpu->vm || vcpu->vm->magic != HYP_VM_MAGIC)
        return -EINVAL;

    /* v2 delegation: the guest keeps its own traps and interrupts, and only
     * the ecall path (plus the second-stage and virtual-instruction faults,
     * which cannot be delegated) still lands here.  This is what makes
     * hyp_arch_guest_trap() a classifier of a much smaller stream than in v1,
     * not a dispatcher for every guest event. */
    __asm__ volatile("csrw hedeleg, %0" :: "r"(HYP_HEDELEG_DEFAULT) : "memory");
    __asm__ volatile("csrw hideleg, %0" :: "r"(HYP_HIDELEG_DEFAULT) : "memory");

    /* hcounteren.TM lets the guest read the time CSR from VS-mode at all: with
     * it clear, `csrr time` in the guest raises a virtual instruction fault
     * (the counter gate, target/riscv/csr.c:144-156), which would put every
     * timer_get_ticks() in the guest kernel on the emulation path.  henvcfg
     * .STCE is the same gate for vstimecmp (target/riscv/csr.c:601-612); it is
     * worth writing, but QEMU masks it against menvcfg (read_henvcfg ANDs the
     * M-mode field in), so on a machine whose firmware never set menvcfg.STCE
     * it stays zero and the guest's vstimecmp write arrives as a virtual
     * instruction fault instead.  hyp_vcpu.c emulates that one CSR, so both
     * shapes work; hcounteren.TM has no such fallback because the fault is not
     * an emulatable CSR write. */
    uint64_t hcounteren;
    __asm__ volatile("csrr %0, 0x606" : "=r"(hcounteren));   /* hcounteren */
    __asm__ volatile("csrw 0x606, %0" :: "r"(hcounteren | (1ULL << 1)) : "memory");
    uint64_t henvcfg;
    __asm__ volatile("csrr %0, 0x60a" : "=r"(henvcfg));       /* henvcfg */
    __asm__ volatile("csrw 0x60a, %0" :: "r"(henvcfg | (1ULL << 63)) : "memory");
    __asm__ volatile("csrr %0, 0x60a" : "=r"(henvcfg));
    if (!(henvcfg & (1ULL << 63)))
        return -EOPNOTSUPP; /* Firmware has not enabled the virtual timer CSR. */

    /* hgatp names the stage-2 root.  HS-mode translations never consult it --
     * only a VS-mode access runs the G-stage -- so programming it while the
     * host is still executing is inert for the host and arms the guest's own
     * walks for guest entry. Full activation fences follow the CSR restore. */
    /* RUN owns a CPU until the next HS trap. Arm a bounded host slice even
     * when the previous task left no immediate timer work. Guest timers use
     * vstimecmp and cannot substitute for this host scheduling deadline. */
    timer_set_interval(riscv64_timer_freq() / 100);
    __asm__ volatile("csrs sie, %0" :: "r"(1ULL << 5) : "memory");
    task_t *host = proc_current();
    if (host && host->trap_ctx) ARCH_TRAP_FAST_RETURN_DISARM(host->trap_ctx);

    uint64_t hgatp = hyp_make_hgatp(vcpu->vm->s2_root, vcpu->vm->vmid);
    __asm__ volatile("csrw hgatp, %0" :: "r"(hgatp) : "memory");

    __asm__ volatile("fence.i" ::: "memory");
    /* The guest's view of the hypervisor.  enter() re-asserts the same bits:
     * SPV in particular is cleared by every trap-return, so nothing here may be
     * treated as the entry state. */
    uint64_t hs = hyp_read_hstatus();
    hs &= ~(HYP_HSTATUS_HU | (0x3fULL << 12) | (3ULL << 32));
    hyp_write_hstatus(hs | HYP_HSTATUS_GUEST_ON | (2ULL << 32)); /* VSXL=RV64 */

    hyp_arch_vstimecmp_set(vcpu->timer_deadline);
    /* A fresh vCPU has a zeroed CSR bank; subsequent activations restore
     * its own bank. Never inherit the previously running guest's VS state. */
    __asm__ volatile("csrw 0x200, %0" :: "r"(vcpu->vsstatus) : "memory");
    __asm__ volatile("csrw 0x204, %0" :: "r"(vcpu->vsie) : "memory");
    __asm__ volatile("csrw 0x205, %0" :: "r"(vcpu->vstvec) : "memory");
    __asm__ volatile("csrw 0x240, %0" :: "r"(vcpu->vsscratch) : "memory");
    __asm__ volatile("csrw 0x241, %0" :: "r"(vcpu->vsepc) : "memory");
    __asm__ volatile("csrw 0x242, %0" :: "r"(vcpu->vscause) : "memory");
    __asm__ volatile("csrw 0x243, %0" :: "r"(vcpu->vstval) : "memory");
    __asm__ volatile("csrw 0x280, %0" :: "r"(vcpu->vsatp) : "memory");
    __asm__ volatile("csrw 0x645, %0" :: "r"(vcpu->hvip & HYP_HIDELEG_DEFAULT) : "memory");
    /* VMID 0 fallback: invalidate both stages on every activation. */
    __asm__ volatile(".insn r 0x73, 0, 0x31, x0, x0, x0" ::: "memory");
    __asm__ volatile(".insn r 0x73, 0, 0x11, x0, x0, x0" ::: "memory");
    return 0;
}


/*
 * Bank -> vcpu.  The frame is NOT the source of truth for the guest's
 * registers, and none of it can be: the prelude that got us here had already
 * switched to the host's sp and tp before __trap_from_kernel ran, so the
 * frame's x[2] and x[4] are host values by the time C sees them, and every
 * other slot is a register the prelude spent after banking the guest's copy.
 * The guest's image is in vcpu->tramp[], written by hyp_guest_trap_entry
 * before it touched anything, and that is where it is read from.
 *
 * This is what makes t0 correct in vcpu->regs[5] for the first time: the
 * earlier prelude reached the vcpu through t0, so nothing else held the
 * guest's t0 and the frame's copy of the slot was the pointer.  The bank holds
 * it because the prelude writes x1 and x3..x31 straight to host memory.
 *
 * x[0] is hardwired zero in hardware and the guest image keeps it that way;
 * on the host path that slot carries the address-space token, which is why the
 * guest copy must not touch it.
 */
static void hyp_frame_to_vcpu(hyp_vcpu_t *vcpu, trap_context_t *ctx)
{
    for (int i = 1; i < 32; i++)
        vcpu->regs[i] = vcpu->tramp[HYP_ASM_TRAMP_X0 + i];
    vcpu->regs[0] = 0;
    vcpu->regs[2] = vcpu->tramp[HYP_ASM_TRAMP_GUEST_SP];
    vcpu->pc = TRAP_CTX_EPC(ctx);
    /* arch[23] records VU versus VS across a resumable RUN exit. */
    vcpu->arch[23] = !(ctx->sstatus & (1ULL << 8));
    for (int i = 0; i < 32; ++i)
        vcpu->fpu[i] = ctx->f[i];
    vcpu->fcsr = ctx->fcsr;
}

/* Re-arm the guest trap channel for the sret that is about to leave.
 *
 * sscratch has to name the bank again because the C half runs between the
 * bank and the guest: __trap_from_kernel's own entry does not write sscratch,
 * but nothing makes that a property we may rely on across future changes to
 * the host trap path, and the resume is the last point where the vcpu is known
 * to be the one the guest is about to run on.
 *
 * The host sp/tp slots are refreshed from arch[] -- arch[1] and arch[3] in the
 * layout hyp_vcpu.h pins beside HYP_VCPU_ARCH_U64, which is where
 * hyp_arch_vcpu_enter() took them and which does not move for the life of the
 * run.  They are refreshed rather than assumed only so that the value cannot
 * silently go stale if the enter path ever starts adjusting sp before arming
 * (it does not: it arms first, then builds the frame below sp).  They are NOT
 * the current C sp -- that is a few frames deeper than the frame the trap
 * entry is about to pop, and writing it here would aim the NEXT guest trap's
 * frame at a stack region that only happens to be free.
 */
static void hyp_arm_guest_trap_channel(hyp_vcpu_t *vcpu)
{
    vcpu->tramp[HYP_ASM_TRAMP_HOST_SP] = vcpu->arch[1];
    vcpu->tramp[HYP_ASM_TRAMP_HOST_TP] = vcpu->arch[3];
    __asm__ volatile("csrw sscratch, %0"
                     :: "r"((uint64_t)&vcpu->tramp[0]) : "memory");
}

/*
 * vcpu -> frame, on the resume side.  This is the whole fixup channel: the run
 * loop edits vcpu->regs[] and vcpu->pc (sepc past an ecall, a rewritten GPR)
 * and never the frame, so the image the trap-return hands back to the guest has
 * to be built from the vcpu again.  sp and tp come from the banked guest values
 * here, which is what puts the guest back on its own stack.
 */
static void hyp_vcpu_to_frame(hyp_vcpu_t *vcpu, trap_context_t *ctx)
{
    for (int i = 1; i < 32; i++)
        ctx->x[i] = vcpu->regs[i];
    TRAP_CTX_EPC(ctx) = vcpu->pc;
}

/*
 * The dispatcher's question, asked from kernel_trap_handler() for every trap
 * that lands on the kernel entry.  Returns 1 when the frame belonged to the
 * running guest, which covers both outcomes: the run loop fixed the guest image
 * up and the ordinary trap-return resumes the guest, or the guest hit a
 * terminal exit and control has already left through hyp_arch_vcpu_exit().
 */
int hyp_arch_guest_trap(void *trap_frame)
{
    hyp_vcpu_t *vcpu = hyp_vcpu_active();

    /* Tested first: on a CPU without H this must not even reach a hstatus
     * read, and a live vcpu is only possible on one that has it. */
    if (!vcpu || !vcpu->running)
        return 0;

    /* hstatus.SPV is set by hardware on a trap out of VS-mode and cleared by
     * the trap-return that entered the guest, so while we are in HS-mode it is
     * what separates this trap from a host kernel trap -- both arrive here. */
    if (!(hyp_read_hstatus() & HYP_HSTATUS_SPV))
        return 0;

    trap_context_t *ctx = trap_frame;
    uint64_t scause = arch_read_cause();

    /* From here until the guest resumes the CPU is in HS-mode on the host's
     * own sp and tp, so a trap taken inside this handler is a host trap, not a
     * second guest trap, and it must not be run through the prelude again:
     * the live registers are the host's and sscratch is the host's own, so the
     * prelude would bank host registers over the guest's and resume the guest
     * from the wreckage.  hyp_guest_trap_entry takes stvec over at its last
     * step, before it jumps here, so that already holds; the writes below and
     * on the resume path state the same invariant from the C side, where a
     * reader looks for it and where a future change to the prelude's tail would
     * otherwise silently drop it.  The published pointer follows it. */
    __asm__ volatile("csrw stvec, %0" :: "r"((uint64_t)__trap_from_kernel) : "memory");

    hyp_frame_to_vcpu(vcpu, ctx);

    /* A host interrupt reaching a running guest is the host's to run: the IRQ
     * machinery services the source against the guest frame and the trap
     * return below resumes the guest, which is why this is the arch side's
     * job and not the run loop's.  Skipping it would leave the source pending,
     * and the guest would re-trap on it the instant it resumed.  A preemption
     * request raised here (proc_sched_tick only asks) is consumed after the
     * guest is gone, at the syscall's own safe point. */
    if (scause & CAUSE_INTR_MASK)
        arch_handle_irq(scause & CAUSE_CODE_MASK, 0);

    int resume = hyp_vcpu_handle_trap(vcpu, ctx, scause, arch_read_tval(),
                                      hyp_read_htval());
    if (__atomic_load_n(&vcpu->vm->broken, __ATOMIC_ACQUIRE)) {
        vcpu->exit = HYP_EXIT_ERROR; resume = 0;
    }
    if (resume && vcpu->exit == HYP_EXIT_YIELD) resume = 0;
    if (resume) hyp_vcpu_sync_irqs(vcpu);
    if (resume && hyp_vcpu_control_exit(vcpu)) resume = 0;
    if (resume && vcpu->vm->managed_console &&
        ((scause & CAUSE_INTR_MASK) || hyp_console_pending(vcpu->vm))) {
        vcpu->exit = HYP_EXIT_YIELD;
        resume = 0;
    }
    if (resume) {
        hyp_vcpu_to_frame(vcpu, ctx);
        /* Guest is live again: the vector and the bank channel go back with
         * it, in that order relative to the sret -- nothing may trap between
         * the two, and nothing can: the prelude's step 1 (SIE off) plus the
         * interrupt-off window hyp_arch_vcpu_enter holds across its own sret is
         * the same reasoning, and here the remaining window is the handful of
         * instructions in __trap_from_kernel's tail before its sret with
         * interrupts still disabled from the trap we are returning from. */
        __asm__ volatile("csrw stvec, %0" :: "r"((uint64_t)hyp_guest_trap_entry)
                         : "memory");
        hyp_arm_guest_trap_channel(vcpu);
        return 1;
    }

    /* Terminal exit.  vcpu->regs/pc still hold the image this trap arrived on,
     * which is what the exit details describe; the host context comes back
     * from arch[] without ever returning to the trap entry. */
    vcpu->running = 0;
    hyp_arch_vcpu_exit(vcpu);
}

/* A guest trap reaches kernel_trap_handler through the guest prelude, after
 * that prelude banks the guest sp/tp and joins __trap_from_kernel. This hook
 * must classify the frame before generic host bookkeeping overwrites x[0]
 * with the host address-space token. A handled frame either resumes the guest
 * through the normal trap return or tears it down before returning here. */
int arch_hyp_guest_trap(trap_context_t *ctx)
{
    return hyp_arch_guest_trap(ctx);
}

static __attribute__((unused)) void hyp_riscv_put(hyp_vcpu_t *vcpu)
{
    __asm__ volatile("csrr %0, 0x645" : "=r"(vcpu->hvip));
    vcpu->timer_deadline = hyp_arch_vstimecmp_get();
    hyp_arch_vstimecmp_set((uint64_t)-1);
    __asm__ volatile("csrr %0, 0x200" : "=r"(vcpu->vsstatus));
    __asm__ volatile("csrr %0, 0x204" : "=r"(vcpu->vsie));
    __asm__ volatile("csrr %0, 0x205" : "=r"(vcpu->vstvec));
    __asm__ volatile("csrr %0, 0x240" : "=r"(vcpu->vsscratch));
    __asm__ volatile("csrr %0, 0x241" : "=r"(vcpu->vsepc));
    __asm__ volatile("csrr %0, 0x242" : "=r"(vcpu->vscause));
    __asm__ volatile("csrr %0, 0x243" : "=r"(vcpu->vstval));
    __asm__ volatile("csrr %0, 0x280" : "=r"(vcpu->vsatp));
    __asm__ volatile("csrw 0x280, zero" ::: "memory");
    __asm__ volatile("csrw hgatp, zero" ::: "memory");
    __asm__ volatile("csrw 0x645, zero" ::: "memory");
    __asm__ volatile(".insn r 0x73, 0, 0x31, x0, x0, x0" ::: "memory");
}

static __attribute__((unused)) void hyp_riscv_sync_irqs(hyp_vcpu_t *vcpu,
                                                       unsigned set, unsigned clear)
{
    (void)vcpu;
    uint64_t on = 0, off = 0;
    const unsigned bit[] = {2, 6, 10};
    for (unsigned i = 0; i < 3; i++) {
        if (set & (1u << i)) on |= 1ULL << bit[i];
        if (clear & (1u << i)) off |= 1ULL << bit[i];
    }
    uint64_t live;
    __asm__ volatile("csrr %0, 0x645" : "=r"(live));
    live = (live & ~off) | on;
    __asm__ volatile("csrw 0x645, %0" :: "r"(live) : "memory");
}

#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
extern int hyp_riscv_probe_smp(hyp_vm_t *);
extern int hyp_riscv_remote_fence(hyp_vm_t *, uint32_t, unsigned);
extern void hyp_riscv_reset(hyp_vcpu_t *, uint64_t, uint64_t, uint64_t);
extern int hyp_riscv_vm_init(hyp_vm_t *vm);
extern void hyp_riscv_vm_destroy(hyp_vm_t *vm);
extern int hyp_riscv_s2_map(hyp_vm_t *, uint64_t, pfn_t, pte_t);
extern int hyp_riscv_s2_pin(hyp_vm_t *, uint64_t, pfn_t *);
extern int hyp_riscv_s2_unmap(hyp_vm_t *, uint64_t);
extern paddr_t hyp_riscv_s2_translate(hyp_vm_t *, uint64_t);
extern int hyp_riscv_s2_audit(hyp_vm_t *, mm_pt_audit_report_t *);
static int hyp_riscv_set_boot(hyp_vcpu_t *vcpu, uint64_t hartid, uint64_t dtb)
{
    vcpu->regs[10] = hartid;
    vcpu->regs[11] = dtb;
    return 0;
}

const hyp_backend_t *hyp_arch_backend(void)
{
    static const hyp_backend_t backend = {
        .name = "riscv-h", .s2_root_order = 2, .max_running_vcpus = CONFIG_NR_CPUS,
        .probe_smp = hyp_riscv_probe_smp, .remote_fence = hyp_riscv_remote_fence,
        .reset = hyp_riscv_reset,
        .gpa_limit = 1ULL << 39, .ram_base = 0x80000000ULL,
        .vm_init = hyp_riscv_vm_init, .vm_destroy = hyp_riscv_vm_destroy,
        .pin = hyp_riscv_s2_pin, .sync_irqs = hyp_riscv_sync_irqs,
        .map = hyp_riscv_s2_map, .unmap = hyp_riscv_s2_unmap,
        .translate = hyp_riscv_s2_translate, .audit = hyp_riscv_s2_audit,
        .set_boot = hyp_riscv_set_boot,
        .load = hyp_arch_vcpu_setup, .enter = hyp_arch_vcpu_enter,
        .put = hyp_riscv_put,
    };
    return &backend;
}

#endif
