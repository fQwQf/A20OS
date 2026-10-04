/*
 * Hypervisor run loop, machine-independent half.  The frozen contract is
 * kernel/include/hyp/hyp_vcpu.h; the design record is
 * docs/hypervisor/00-design.md S5.
 *
 * This file owns three things and nothing else: vcpu lifetime, guest RAM
 * provisioning, and what a guest trap MEANS.  Register save/restore and the
 * guest trap frame are arch code (kernel/arch/riscv64/hyp/).
 *
 * SEAM WITH THE ARCH HALF -- the one thing both sides have to agree on:
 * hyp_vcpu_handle_trap() mutates vcpu->regs[]/vcpu->pc ONLY.  It never
 * touches the trap_frame it is handed; that struct is arch-shaped and this
 * file is not.  The arch side therefore owns the writeback: on a return of
 * 1 it must copy vcpu->regs[]/vcpu->pc back into the frame it takes sret
 * on, and on a return of 0 it restores arch[] and returns from
 * hyp_arch_vcpu_enter().  Guest RAM is pre-loaded through hyp_vm_load();
 * this half never installs a stage-2 mapping on a guest's behalf.
 */
#include "hyp/hyp_vcpu.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "core/cpu.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"

/*
 * RISC-V H exception codes the run loop acts on.  They are named here rather
 * than taken from kernel/<arch>/include/platform.h because that header
 * publishes the generic ECALL-from-U code but not the VS-mode one, and the
 * guest enters VS-mode with hstatus.SPV=1 (S5), so which of the two it
 * issues is a run-shape decision, not a per-arch header fact.  A guest page
 * fault is the code stage-1 and stage-2 faults share in this extension, which
 * is precisely why they are terminal here (see hyp_vcpu_handle_trap).
 *
 * All three ECALL codes are accepted because which one the guest issues
 * depends on the privilege half of the entry, and this run shape enters at
 * privilege S: hyp_vcpu_asm.S sets sstatus.SPP, and SPP is what an ECALL
 * reports against, so a guest entered this way raises 10 (from S-mode) and not
 * 9.  8 (from U-mode) and 9 (from VS-mode proper, i.e. an SPP=0 entry) stay
 * in the table so a guest entered the other way round is still served.
 */
#define HYP_SCAUSE_ECALL_VU   8ULL
#define HYP_SCAUSE_ECALL_VS   9ULL
#define HYP_SCAUSE_ECALL_S   10ULL
#define HYP_SCAUSE_FETCH_GPF  12ULL
#define HYP_SCAUSE_LOAD_GPF   13ULL
#define HYP_SCAUSE_STORE_GPF  15ULL
#define HYP_SCAUSE_INTR_BIT   (1ULL << 63)

/* What a page of guest RAM loaded through hyp_vm_load() gets in stage-2.
 *
 * X, not W: W together with X is a reserved combination in a leaf PTE and the
 * G-stage walk rejects it outright (QEMU, target/riscv/cpu_helper.c, the
 * reserved-RWX switch in the leaf path), so "R|W|X" would fail every guest
 * access on a page that is otherwise mapped correctly.  Loaded RAM is guest
 * code in this slice, the demo guest is register-only, and it enters with
 * sp = 0, so executable-and-readable is the whole of what is needed.  A guest
 * that wants writable RAM needs pages mapped without X -- which means the
 * loaded program cannot live on the same page as its data, and that is a
 * decision for the slice that grows a guest with a stack.
 */
#define HYP_GUEST_PAGE_PROT (PTE_R | PTE_X)

/* The one vcpu whose guest is live kernel-wide, or NULL.  A lock is not held
 * across it, because hyp_arch_vcpu_enter() ends in sret and a lock held across
 * that would be a lock held across arbitrary guest execution.  What makes the
 * slice single-guest is the claim under g_hyp_active_lock, not that lock's
 * scope: it is taken to claim and to release, and across neither the guest nor
 * anything that can block.  The IRQ-off windows in hyp_vcpu_run() are a
 * different thing -- they cover the plain store of vcpu->running that this
 * CPU's own dispatcher reads, which is not shared state. */
static hyp_vcpu_t *hyp_active_vcpu;

/* The claim's lock.  A real cross-CPU lock, unlike arch_local_irq_disable(),
 * which only closes a window on the CPU that executes it: an IRQ-off
 * test-and-set on hyp_active_vcpu would let two CPUs each find the slot free
 * and both program hgatp/hstatus and sret into VS-mode on it. */
static spinlock_t g_hyp_active_lock = SPINLOCK_INIT;

hyp_vcpu_t *hyp_vcpu_active(void)
{
    /* Acquire, because a CPU that sees a vcpu published here must also see
     * everything hyp_vcpu_run() wrote before publishing it (hgatp, hstatus,
     * vcpu->running); a plain load would not order against the claim's store. */
    return __atomic_load_n(&hyp_active_vcpu, __ATOMIC_ACQUIRE);
}

hyp_vcpu_t *hyp_vcpu_create(hyp_vm_t *vm, uint64_t entry_gpa)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return NULL;

    hyp_vcpu_t *vcpu = kcalloc(1, sizeof(*vcpu));
    if (!vcpu)
        return NULL;

    vcpu->magic    = HYP_VCPU_MAGIC;
    vcpu->running  = 0;
    vcpu->vm       = vm;
    vcpu->refcount = 1;
    vcpu->pc       = entry_gpa;
    vcpu->exit     = HYP_EXIT_NONE;

    /* The guest's initial sp and satp are the arch half's call: only
     * hyp_arch_vcpu_setup() knows the guest RAM layout it is entering. */
    vm->refcount++;
    return vcpu;
}

void hyp_vcpu_put(hyp_vcpu_t *vcpu)
{
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC)
        return;

    /* A running guest owns its vcpu for the duration of the enter/exit
     * window; dropping the last reference under it would free the register
     * file the arch half is still restoring into. */
    if (vcpu->running || hyp_active_vcpu == vcpu) {
        kerr("hyp: put of a running vcpu refused\n");
        return;
    }

    if (--vcpu->refcount > 0)
        return;

    hyp_vm_t *vm = vcpu->vm;
    vcpu->vm = NULL;
    vcpu->magic = 0;
    kfree(vcpu);
    /* The VM outlives the vcpu only while someone else holds it. */
    if (vm)
        hyp_vm_put(vm);
}

int hyp_vm_load(hyp_vm_t *vm, uint64_t gpa, const void *bytes, uint64_t len)
{
    if (!vm || vm->magic != HYP_VM_MAGIC || !bytes)
        return -EINVAL;
    if (gpa & (PAGE_SIZE - 1))
        return -EINVAL;
    if (len == 0)
        return 0;
    if (gpa + len < gpa)
        return -EINVAL;

    uint64_t npages = (len + PAGE_SIZE - 1) >> PAGE_SIZE_BITS;
    pfn_t   *pfns  = kcalloc(npages, sizeof(*pfns));
    if (!pfns)
        return -ENOMEM;

    /* Copy BEFORE the mapping goes in: if hyp_s2_map() refuses the page
     * (-EEXIST), the bytes were written into a frame this call still owns
     * and nobody else can name, so a refusal never corrupts an existing
     * mapping.  pfa_alloc_page() is the reclaiming allocator and this loop
     * holds no page-table node lock, which is the condition that makes
     * reclaim safe here (S2: reclaim takes node locks). */
    uint64_t done  = 0;   /* bytes handed to a mapped page */
    uint64_t pages = 0;   /* pages currently in the table */
    pfn_t    cur   = PFN_NONE;
    int      rc    = 0;

    for (uint64_t i = 0; i < npages; i++) {
        cur = pfa_alloc_page();
        if (cur == PFN_NONE) {
            rc = -ENOMEM;
            break;
        }
        pfns[i] = cur;

        void *dst = pfn_to_virt(cur);
        if (!dst) {
            rc = -EINVAL;
            break;
        }
        uint64_t n = len - done;
        if (n > PAGE_SIZE)
            n = PAGE_SIZE;
        memcpy(dst, (const uint8_t *)bytes + done, n);
        /* The tail of a short final page is zeroed rather than left as
         * whatever the recycled frame held: the guest reads whole pages. */
        if (n < PAGE_SIZE)
            memset((uint8_t *)dst + n, 0, PAGE_SIZE - n);

        rc = hyp_s2_map(vm, gpa + i * PAGE_SIZE, cur, HYP_GUEST_PAGE_PROT);
        if (rc)
            break;

        cur = PFN_NONE;   /* the table owns this reference now */
        done += n;
        pages++;
    }

    if (rc) {
        /* Partial load: unwind, so a failed load leaves the VM's address
         * space exactly as it found it. */
        for (uint64_t j = 0; j < pages; j++) {
            hyp_s2_unmap(vm, gpa + j * PAGE_SIZE);
            frame_put(pfns[j]);
        }
        if (cur != PFN_NONE)
            frame_put(cur);
        kerr("hyp: vm_load gpa=%lx len=%lu failed after %lu page(s): %d\n",
             (unsigned long)gpa, (unsigned long)len, (unsigned long)pages, rc);
    }

    kfree(pfns);
    return rc;
}

static void hyp_vcpu_record_fault(hyp_vcpu_t *vcpu, uint64_t scause,
                                  uint64_t stval, uint64_t htval)
{
    vcpu->exit_scause = scause;
    vcpu->exit_stval  = stval;
    vcpu->exit_htval  = htval;
    vcpu->exit        = HYP_EXIT_FAULT;
}

static int hyp_guest_ecall(hyp_vcpu_t *vcpu, uint64_t scause)
{
    uint64_t fid = vcpu->regs[17];

    if (fid == HYP_SBI_CONSOLE_PUTCHAR) {
        putchar((char)(vcpu->regs[10] & 0xff));
        vcpu->pc += 4;   /* sepc past the ecall; arch writes pc back on resume */
        return 1;
    }

    if (fid == HYP_SBI_SHUTDOWN) {
        vcpu->exit_scause = scause;
        vcpu->exit = HYP_EXIT_SHUTDOWN;
        return 0;
    }

    /* The SBI subset is closed on purpose (contract): an unknown call is a
     * guest bug, not an invitation to implement the next one here.  The
     * call id goes in stval because that is the register the guest chose it
     * with, and it is the only detail this exit has. */
    kwarn("hyp: guest SBI call %lu is not implemented\n", (unsigned long)fid);
    hyp_vcpu_record_fault(vcpu, scause, fid, 0);
    return 0;
}

int hyp_vcpu_handle_trap(hyp_vcpu_t *vcpu, void *trap_frame,
                         uint64_t scause, uint64_t stval, uint64_t htval)
{
    (void)trap_frame;

    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC || !vcpu->running)
        return 0;

    /* Host IRQ.  The host IRQ machinery has already run against this frame
     * by the time the dispatcher gets here; the guest just resumes. */
    if (scause & HYP_SCAUSE_INTR_BIT)
        return 1;

    switch (scause & ~HYP_SCAUSE_INTR_BIT) {
    case HYP_SCAUSE_ECALL_VU:
    case HYP_SCAUSE_ECALL_VS:
    case HYP_SCAUSE_ECALL_S:
        return hyp_guest_ecall(vcpu, scause);

    case HYP_SCAUSE_FETCH_GPF:
    case HYP_SCAUSE_LOAD_GPF:
    case HYP_SCAUSE_STORE_GPF:
        /* Guest page fault.  Guest RAM comes only from hyp_vm_load(); an
         * unmapped GPA is the guest walking off what it was provisioned,
         * and the contract forbids a silent on-demand fill here.  RISC-V H
         * reports stage-1 and stage-2 page faults under these same codes
         * (htval distinguishing them), and a guest in this slice has no
         * stage-1 to fault in, so both are terminal. */
        hyp_vcpu_record_fault(vcpu, scause, stval, htval);
        kerr("hyp: guest page fault scause=%lu stval=%lx htval=%lx\n",
             (unsigned long)scause, (unsigned long)stval,
             (unsigned long)htval);
        return 0;

    default:
        kerr("hyp: unhandled guest trap scause=%lu stval=%lx\n",
             (unsigned long)scause, (unsigned long)stval);
        hyp_vcpu_record_fault(vcpu, scause, stval, htval);
        return 0;
    }
}

int hyp_vcpu_run(hyp_vcpu_t *vcpu)
{
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC)
        return -EINVAL;
    if (!vcpu->vm || vcpu->vm->magic != HYP_VM_MAGIC)
        return -EINVAL;

    /* Claim the single guest slot before anything else: a setup that is
     * about to program this CPU's virtualization state must not race a
     * second CPU claiming the same slice.  The window is a test-and-set, and
     * it has to be a LOCKED one: arch_local_irq_disable() only closes the
     * window on the CPU that runs it, so a plain IRQ-off read-modify-write
     * leaves two CPUs free to both find the slot free and then both program
     * hgatp/hstatus and sret into VS-mode on the same guest slot.  This is
     * the same idiom the frame allocator uses for its own exclusive claim
     * (frame.c, spin_lock_irqsave(&pfa.lock)), and for the same reason: the
     * lock is a real cross-CPU lock, not a local one.
     *
     * Nothing holds the lock across the guest -- it is taken to claim and
     * released before hyp_arch_vcpu_enter() -- so this is not a lock held
     * across arbitrary guest execution, which is why trylock (never sleep,
     * never wait for the owner) is the right shape here: losing the race
     * means answering -EBUSY, not waiting. */
    uint64_t claim_flags;
    if (!spin_trylock_irqsave(&g_hyp_active_lock, &claim_flags)) {
        kerr("hyp: vcpu_run refused, guest slot claimed elsewhere\n");
        return -EBUSY;
    }
    if (hyp_active_vcpu) {
        spin_unlock_irqrestore(&g_hyp_active_lock, claim_flags);
        kerr("hyp: vcpu_run refused, guest already live\n");
        return -EBUSY;
    }
    hyp_active_vcpu = vcpu;
    spin_unlock_irqrestore(&g_hyp_active_lock, claim_flags);

    /* Kept for the two IRQ-off windows around vcpu->running below, which
     * guard a plain store this CPU's own dispatcher reads, not shared state. */
    int irqs_were_on = arch_irqs_enabled();

    vcpu->exit        = HYP_EXIT_NONE;
    vcpu->exit_scause = 0;
    vcpu->exit_stval  = 0;
    vcpu->exit_htval  = 0;

    int rc = hyp_arch_vcpu_setup(vcpu);
    if (rc != 0) {
        kerr("hyp: arch_vcpu_setup failed: %d\n", rc);
        goto out;
    }

    /* running and active go up together: the trap dispatcher reads the pair,
     * and between them the host must see a complete "a guest is live"
     * statement, never half of one. */
    arch_local_irq_disable();
    vcpu->running = 1;
    if (irqs_were_on)
        arch_local_irq_enable();

    hyp_arch_vcpu_enter(vcpu);   /* returns only through the exit path */

    arch_local_irq_disable();
    vcpu->running = 0;
    if (irqs_were_on)
        arch_local_irq_enable();

    /* A guest shutdown and a guest fault are both terminal exits, and both
     * are reported through vcpu->exit; a negative return is reserved for the
     * host failing to run the guest at all.  Enter coming back with nothing
     * recorded is exactly that. */
    if (vcpu->exit == HYP_EXIT_NONE || vcpu->exit == HYP_EXIT_ERROR)
        rc = -EIO;
    else
        rc = 0;

out:
    /* Release the claim under the lock it was taken with, so another CPU's
     * claim cannot interleave with this one.  The restore flags come from
     * this lock acquisition, not from the claim above: the interrupt state
     * then is not the one the claim found. */
    uint64_t release_flags = spin_lock_irqsave(&g_hyp_active_lock);
    hyp_active_vcpu = NULL;
    spin_unlock_irqrestore(&g_hyp_active_lock, release_flags);
    return rc;
}