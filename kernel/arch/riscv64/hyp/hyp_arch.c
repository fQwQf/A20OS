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
#include "hyp_asm_offsets.h"

/* hstatus exists only when the H extension is implemented, so reading it IS
 * the probe.  (csrr misa was tried first and turned out to raise an
 * illegal-instruction trap under QEMU's rv64 CPU -- the spec calls misa
 * any-privilege, so the reason is unresolved; the CSR the extension itself
 * adds is the honest probe.)  Read once at first use. */
static int hyp_probe(void)
{
    static int probed = -1;
    if (probed < 0) {
        uint64_t hstatus;
        __asm__ volatile("csrr %0, hstatus" : "=r"(hstatus));
        (void)hstatus;
        probed = 1;
    }
    return probed;
}

int hyp_supported(void)
{
    return hyp_probe();
}

/*
 * hgatp layout (RV64): MODE[63:60] | VMID[59:44] | PPN[43:0].
 * MODE 8 is the Sv39 walk -- QEMU spells it VM_1_10_SV39 and gives it the
 * number 8 (target/riscv/cpu_bits.h) -- i.e. the three-level, 9-bit-index,
 * 8-byte-PTE form, with the non-leaf PPN read as one contiguous field.  That
 * is the shape hyp.c builds, so the tree is walked exactly as it is written;
 * the split PPN encoding the x4 extensions define for non-leaf PTEs is not
 * what this hart implements.  VMID is implementation-defined in width;
 * writing our 16-bit counter into the WARL field keeps only the bits the hart
 * implements, so two VMs whose tags alias in the hardware VMID would share
 * a TLB tag -- hyp_next_vmid() wrapping is the documented limit that leads
 * there, and a full hfence on wrap is the cheap future fix.
 */
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
#define HYP_HSTATUS_VTVM (1ULL << 20)  /* guest VS-CSR accesses trap to HS */

/*
 * The VS-facing permission bits, set for the guest and cleared behind it.
 * SP2P (trapping HS-mode accesses to VS-mode CSRs) is deliberately absent:
 * QEMU implements no such CSR, so writing it here raises an illegal
 * instruction on the platform this slice is measured on, and with
 * hedeleg/hideleg zero nothing delegates to VS-mode anyway, so its reset
 * value is inert here.
 */
#define HYP_HSTATUS_GUEST_ON  (HYP_HSTATUS_SPV | HYP_HSTATUS_SPVP | HYP_HSTATUS_VTVM)
#define HYP_HSTATUS_GUEST_OFF (HYP_HSTATUS_GUEST_ON | HYP_HSTATUS_HU)

/* Terminal exit path: restores the host context from vcpu->arch[] and returns
 * to hyp_arch_vcpu_enter()'s caller.  Declared here rather than in the frozen
 * header because only the arch half -- hyp_arch_guest_trap() -- calls it. */
void __attribute__((noreturn)) hyp_arch_vcpu_exit(hyp_vcpu_t *vcpu);

/* The guest trap prelude (hyp_vcpu_asm.S) has to switch to the host stack and
 * host tp before the kernel's trap entry stores anything, so it needs the live
 * vcpu without going through C.  NULL whenever no guest is running, which is
 * also the prelude's "not a guest trap" answer.  One running guest at a time
 * is the run loop's rule; this only mirrors it. */
hyp_vcpu_t *g_hyp_arch_vcpu;

/* The assembly hardcodes these; a struct change that moves one of them would
 * corrupt the guest register image instead of failing the build. */
_Static_assert(__builtin_offsetof(hyp_vcpu_t, regs) == HYP_ASM_VCPU_REGS_OFF,
               "hyp_vcpu_asm.S indexes regs[] at HYP_ASM_VCPU_REGS_OFF");
_Static_assert(__builtin_offsetof(hyp_vcpu_t, pc) == HYP_ASM_VCPU_PC_OFF,
               "hyp_vcpu_asm.S indexes pc at HYP_ASM_VCPU_PC_OFF");
_Static_assert(__builtin_offsetof(hyp_vcpu_t, arch) == HYP_ASM_VCPU_ARCH_OFF,
               "hyp_vcpu_asm.S indexes arch[] at HYP_ASM_VCPU_ARCH_OFF");

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

    /* Nothing is delegated in this slice: every guest exception and every
     * guest interrupt traps to HS, which is what makes hyp_arch_guest_trap()
     * the one place a trap is classified. */
    __asm__ volatile("csrw hedeleg, zero" ::: "memory");
    __asm__ volatile("csrw hideleg, zero" ::: "memory");

    /* hgatp names the stage-2 root.  HS-mode translations never consult it --
     * only a VS-mode access runs the G-stage -- so programming it while the
     * host is still executing is inert for the host and arms the guest's own
     * walks for the sret in hyp_arch_vcpu_enter().  No fence is needed: the
     * VMID is fresh, and hyp_vm_create() already fenced it. */
    uint64_t hgatp = hyp_make_hgatp(vcpu->vm->s2_root, vcpu->vm->vmid);
    __asm__ volatile("csrw hgatp, %0" :: "r"(hgatp) : "memory");

    /* The guest's view of the hypervisor.  enter() re-asserts the same bits:
     * SPV in particular is cleared by every trap-return, so nothing here may be
     * treated as the entry state. */
    hyp_write_hstatus((hyp_read_hstatus() | HYP_HSTATUS_GUEST_ON) & ~HYP_HSTATUS_HU);

    /* vsatp stays where the hart reset it, at MODE=Bare, and that is exactly
     * what this slice wants.  VS-mode's stage-1 register is vsatp, NOT satp:
     * the trap-return into the guest parks the host's satp and puts vsatp in
     * its place, so satp is never the guest's table and there is nothing here
     * to program or to swap.  Bare means VS-stage-1 makes no translation, the
     * guest's virtual address is its guest physical address, and hgatp is then
     * the only table the guest's accesses walk -- which is the whole point of
     * this slice.  A guest that wants paging would need its own Sv39 tree in
     * guest RAM, reachable through hgatp at a VS-virtual address below 2^41
     * (the G-stage rejects anything wider); that is a later slice. */
    return 0;
}


/*
 * Frame -> vcpu.  The frame's x[2] and x[4] hold HOST values by the time C
 * sees them, and that is not a fact about the guest's register state but about
 * the ORDER in hyp_guest_trap_entry: the prelude stores sp and tp into
 * vcpu->regs[] and only then reloads the host's sp and tp from arch[1]/arch[3]
 * before jumping to the ordinary kernel entry.  So the frame this C path reads
 * was built on the host stack with the host tp -- architectural x2 and x4 are
 * the host pair, never the guest's, which is why the two banked values are the
 * only guest registers not recovered from the frame.
 *
 * x[0] is hardwired zero in hardware and the guest image keeps it that way;
 * on the host path that slot carries the address-space token, which is why the
 * guest copy must not touch it.
 */
static void hyp_frame_to_vcpu(hyp_vcpu_t *vcpu, trap_context_t *ctx)
{
    uint64_t guest_sp = vcpu->regs[2];
    uint64_t guest_tp = vcpu->regs[4];

    for (int i = 1; i < 32; i++)
        vcpu->regs[i] = ctx->x[i];
    vcpu->regs[0] = 0;
    vcpu->regs[2] = guest_sp;
    vcpu->regs[4] = guest_tp;
    vcpu->pc = TRAP_CTX_EPC(ctx);
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
     * second guest trap.  Clearing the published pointer says so: the prelude
     * hands such a trap straight to the kernel entry instead of banking the
     * host's sp/tp as if they were the guest's.  Restored on the resume path,
     * where the guest is live again. */
    g_hyp_arch_vcpu = NULL;

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

    if (hyp_vcpu_handle_trap(vcpu, ctx, scause, arch_read_tval(),
                             hyp_read_htval())) {
        hyp_vcpu_to_frame(vcpu, ctx);
        g_hyp_arch_vcpu = vcpu;
        return 1;
    }

    /* Terminal exit.  vcpu->regs/pc still hold the image this trap arrived on,
     * which is what the exit details describe; the host context comes back
     * from arch[] without ever returning to the trap entry. */
    vcpu->running = 0;
    hyp_arch_vcpu_exit(vcpu);
}
