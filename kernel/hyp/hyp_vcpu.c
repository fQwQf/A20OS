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
#include "cg/cgroup.h"
#include "core/cpu.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"

/* What a page of guest RAM loaded through hyp_vm_load() gets in stage-2.
 *
 * R|W|X, the same leaf the demand-fill window uses (hyp.c,
 * HYP_RAM_PAGE_PROT), and for the same reason: hyp_vm_load() carries no
 * per-page flags, so the loader cannot tell a code page from a data page and
 * cannot map them differently.  R|X was enough while the v1 demo guest was
 * register-only, but it is not enough for a real kernel: the guest image's
 * .data and .bss are loaded by hyp_vm_load() like everything else, and the
 * first thing A20OS's entry.S does is an LR/SC on a lock word in .data
 * (.kernel-build smoke log hyp-a20os-riscv64.log, "undecodable guest access
 * at pc=80200044", scause 23 on gpa 0x8060de78) -- a store to a page mapped
 * read-only, which faults before the guest has printed anything.  Page
 * granularity is what makes the split impossible anyway: .data and .bss share
 * pages.
 *
 * R|W|X is valid. Guest stage-1 permissions still apply.
 */
#define HYP_GUEST_PAGE_PROT (PTE_R | PTE_W | PTE_X)

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
    vcpu->timer_deadline = (uint64_t)-1;

    /* The guest's initial sp and satp are the arch half's call: only
     * hyp_arch_vcpu_setup() knows the guest RAM layout it is entering. */
    hyp_vm_get(vm);
    return vcpu;
}

/* Guest boot registers (contract): the loader puts the hartid and the DTB's
 * GPA in a0/a1, the SBI/S-mode convention _start reads them from.  Only the
 * two argument registers are described, so that is all this writes: every
 * other register, including sp, stays whatever create() left. */
int hyp_vcpu_set_boot(hyp_vcpu_t *vcpu, uint64_t hartid, uint64_t dtb_gpa)
{
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_hyp_active_lock);
    if (hyp_active_vcpu == vcpu) {
        spin_unlock_irqrestore(&g_hyp_active_lock, flags);
        return -EBUSY;
    }
    int rc = vcpu->vm->backend->set_boot ?
             vcpu->vm->backend->set_boot(vcpu, hartid, dtb_gpa) : -EOPNOTSUPP;
    spin_unlock_irqrestore(&g_hyp_active_lock, flags);
    return rc;
}

void hyp_vcpu_get(hyp_vcpu_t *vcpu)
{
    if (vcpu)
        __atomic_add_fetch(&vcpu->refcount, 1, __ATOMIC_RELAXED);
}

void hyp_vcpu_put(hyp_vcpu_t *vcpu)
{
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC)
        return;

    if (__atomic_sub_fetch(&vcpu->refcount, 1, __ATOMIC_ACQ_REL) != 0)
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
    int update_rc = hyp_vm_begin_update(vm);
    if (update_rc)
        return update_rc;
    if (len > vm->ram_size || gpa < vm->ram_base ||
        gpa - vm->ram_base > vm->ram_size - len ||
        len > (uint64_t)-1 - (PAGE_SIZE - 1)) {
        hyp_vm_end_update(vm);
        return -EINVAL;
    }
    uint64_t npages = (len + PAGE_SIZE - 1) >> PAGE_SIZE_BITS;
    if (vm->charge_cg && cg_mem_charge(vm->charge_cg, npages) != 0) {
        hyp_vm_end_update(vm);
        return -ENOMEM;
    }
    pfn_t   *pfns  = kcalloc(npages, sizeof(*pfns));
    if (!pfns) {
        if (vm->charge_cg)
            cg_mem_uncharge(vm->charge_cg, npages);
        hyp_vm_end_update(vm);
        return -ENOMEM;
    }

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

    if (rc) {
        if (vm->charge_cg)
            cg_mem_uncharge(vm->charge_cg, npages);
    } else {
        __atomic_add_fetch(&vm->charged_pages, npages, __ATOMIC_RELAXED);
    }
    kfree(pfns);
    hyp_vm_end_update(vm);
    return rc;
}

int hyp_vcpu_run_result(hyp_vcpu_t *vcpu, hyp_exit_reason_t *reason)
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
        return -EBUSY;
    }
    if (hyp_active_vcpu) {
        spin_unlock_irqrestore(&g_hyp_active_lock, claim_flags);
        return -EBUSY;
    }
    uint64_t vm_flags = spin_lock_irqsave(&vcpu->vm->control_lock);
    if (vcpu->vm->updating) {
        spin_unlock_irqrestore(&vcpu->vm->control_lock, vm_flags);
        spin_unlock_irqrestore(&g_hyp_active_lock, claim_flags);
        return -EBUSY;
    }
    vcpu->vm->run_count++;
    spin_unlock_irqrestore(&vcpu->vm->control_lock, vm_flags);
    hyp_vcpu_get(vcpu); /* Run owns a reference independent of its handle. */
    hyp_active_vcpu = vcpu;
    spin_unlock_irqrestore(&g_hyp_active_lock, claim_flags);

    /* Kept for the two IRQ-off windows around vcpu->running below, which
     * guard a plain store this CPU's own dispatcher reads, not shared state. */
    int irqs_were_on = arch_irqs_enabled();
    /* CPU-local backend state must not migrate between load, entry and put. */
    arch_local_irq_disable();

    if (vcpu->vm->managed_console && hyp_console_pending(vcpu->vm)) {
        vcpu->exit = HYP_EXIT_YIELD;
        if (reason)
            *reason = HYP_EXIT_YIELD;
        /* No backend state has been loaded yet. */
        uint64_t flags = spin_lock_irqsave(&g_hyp_active_lock);
        vm_flags = spin_lock_irqsave(&vcpu->vm->control_lock);
        vcpu->vm->run_count--;
        spin_unlock_irqrestore(&vcpu->vm->control_lock, vm_flags);
        hyp_active_vcpu = NULL;
        spin_unlock_irqrestore(&g_hyp_active_lock, flags);
        hyp_vcpu_put(vcpu);
        if (irqs_were_on)
            arch_local_irq_enable();
        return 0;
    }
    vcpu->exit        = HYP_EXIT_NONE;
    vcpu->exit_scause = 0;
    vcpu->exit_stval  = 0;
    vcpu->exit_htval  = 0;

    int rc = vcpu->vm->backend->load(vcpu);
    if (rc != 0) {
        kerr("hyp: arch_vcpu_setup failed: %d\n", rc);
        goto out;
    }

    /* running and active go up together: the trap dispatcher reads the pair,
     * and between them the host must see a complete "a guest is live"
     * statement, never half of one. */
    arch_local_irq_disable();
    vcpu->running = 1;
    vcpu->vm->backend->enter(vcpu);
    if (vcpu->vm->backend->put)
        vcpu->vm->backend->put(vcpu);

    arch_local_irq_disable();
    vcpu->running = 0;

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
    vm_flags = spin_lock_irqsave(&vcpu->vm->control_lock);
    if (reason)
        *reason = vcpu->exit;
    vcpu->vm->last_exit[0] = rc < 0 ? HYP_EXIT_ERROR : vcpu->exit;
    vcpu->vm->last_exit[1] = vcpu->exit_scause;
    vcpu->vm->last_exit[2] = vcpu->exit_stval;
    vcpu->vm->last_exit[3] = vcpu->exit_htval;
    vcpu->vm->run_count--;
    spin_unlock_irqrestore(&vcpu->vm->control_lock, vm_flags);
    hyp_active_vcpu = NULL;
    spin_unlock_irqrestore(&g_hyp_active_lock, release_flags);
    hyp_vcpu_put(vcpu);
    if (irqs_were_on)
        arch_local_irq_enable();
    return rc;
}

int hyp_vcpu_run(hyp_vcpu_t *vcpu)
{
    return hyp_vcpu_run_result(vcpu, NULL);
}
