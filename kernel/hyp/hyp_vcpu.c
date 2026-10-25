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
#include "core/smp.h"
#include "core/preempt.h"
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
#define HYP_GUEST_PAGE_PROT (PTE_R | PTE_W | HYP_PROT_EXEC)

/* CPU-local activation: one RUN owner per vCPU, with each VM bounded by
 * its configured parallel limit and synchronous stage-2 fences. Distinct managed
 * VMs may execute on different host CPUs. The legacy UART has one owner. */
static hyp_vcpu_t *hyp_active_vcpus[CONFIG_NR_CPUS];
static hyp_vcpu_t *hyp_legacy_vcpu;
static spinlock_t g_hyp_active_lock = SPINLOCK_INIT;

hyp_vcpu_t *hyp_vcpu_active(void)
{
    return __atomic_load_n(&hyp_active_vcpus[cpu_current_id()], __ATOMIC_ACQUIRE);
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
    uint64_t flags = spin_lock_irqsave(&vm->control_lock);
    unsigned id;
    for (id = 0; id < vm->vcpu_limit; id++) if (!vm->vcpus[id]) break;
    if (id == vm->vcpu_limit || vm->updating || __atomic_load_n(&vm->broken, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&vm->shutdown, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&vm->control_lock, flags);
        kfree(vcpu);
        return NULL;
    }
    vcpu->id = id;
    vcpu->hart_state = id ? HYP_HART_STOPPED : HYP_HART_STARTED;
    vm->vcpus[id] = vcpu;
    vm->vcpu_count++;
    hyp_vm_get(vm);
    spin_unlock_irqrestore(&vm->control_lock, flags);
    hyp_account_vcpu(1);
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
    if (vcpu->claimed || hartid != vcpu->id) {
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

    hyp_vm_t *vm = vcpu->vm;
    uint64_t flags = spin_lock_irqsave(&vm->control_lock);
    if (__atomic_sub_fetch(&vcpu->refcount, 1, __ATOMIC_ACQ_REL) != 0) {
        spin_unlock_irqrestore(&vm->control_lock, flags);
        return;
    }
    vm->vcpus[vcpu->id] = NULL;
    vm->vcpu_count--;
    spin_unlock_irqrestore(&vm->control_lock, flags);
    if (vm->backend->vcpu_destroy)
        vm->backend->vcpu_destroy(vcpu);
    vcpu->vm = NULL;
    vcpu->magic = 0;
    hyp_account_vcpu(-1);
    kfree(vcpu);
    /* The VM outlives the vcpu only while someone else holds it. */
    if (vm)
        hyp_vm_put(vm);
}

hyp_vcpu_t *hyp_vm_vcpu_get(hyp_vm_t *vm, uint64_t hartid)
{
    if (hartid >= HYP_MAX_VCPUS) return NULL;
    uint64_t flags = spin_lock_irqsave(&vm->control_lock);
    hyp_vcpu_t *vcpu = vm->vcpus[hartid];
    if (vcpu) hyp_vcpu_get(vcpu);
    spin_unlock_irqrestore(&vm->control_lock, flags);
    return vcpu;
}

int hyp_vm_hart_status(hyp_vm_t *vm, uint64_t hartid)
{
    hyp_vcpu_t *vcpu = hyp_vm_vcpu_get(vm, hartid);
    if (!vcpu) return -EINVAL;
    int state = (int)__atomic_load_n(&vcpu->hart_state, __ATOMIC_ACQUIRE);
    hyp_vcpu_put(vcpu);
    return state;
}

int hyp_vm_hart_start(hyp_vm_t *vm, uint64_t hartid, uint64_t entry, uint64_t opaque)
{
    if ((entry & 1) || !hyp_vm_ram_contains(vm, entry) || !vm->backend->reset)
        return -EINVAL;
    hyp_vcpu_t *vcpu = hyp_vm_vcpu_get(vm, hartid);
    if (!vcpu) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_hyp_active_lock);
    int rc = 0;
    if (vcpu->claimed || __atomic_load_n(&vcpu->hart_state, __ATOMIC_ACQUIRE) != HYP_HART_STOPPED)
        rc = -EALREADY;
    else if (__atomic_load_n(&vm->shutdown, __ATOMIC_ACQUIRE) ||
             (vcpu->requests & HYP_REQUEST_STOP)) rc = -ESHUTDOWN;
    else {
        vm->backend->reset(vcpu, entry, hartid, opaque);
        vcpu->requests &= HYP_REQUEST_PAUSE;
        vcpu->irq_set = vcpu->irq_clear = 0;
        __atomic_store_n(&vcpu->hart_state, HYP_HART_START_PENDING, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&g_hyp_active_lock, flags);
    hyp_vcpu_put(vcpu);
    return rc;
}

static int hyp_vm_hart_mask(hyp_vm_t *vm, uint64_t mask, uint64_t base, unsigned kind, int ipi)
{
    uint32_t cpus = 0;
    hyp_vcpu_t *targets[HYP_MAX_VCPUS] = {0};
    unsigned n = 0;
    for (unsigned id = 0; id < HYP_MAX_VCPUS; id++) {
        if (base != (uint64_t)-1 && (id < base || id - base >= 64 || !(mask & (1ULL << (id - base)))))
            continue;
        hyp_vcpu_t *target = hyp_vm_vcpu_get(vm, id);
        if (!target) {
            if (base == (uint64_t)-1) continue;
            for (unsigned i = 0; i < n; i++) hyp_vcpu_put(targets[i]);
            return -EINVAL;
        }
        targets[n++] = target;
    }
    if (base != (uint64_t)-1) {
        uint64_t valid = base < HYP_MAX_VCPUS ? ((1ULL << (HYP_MAX_VCPUS - base)) - 1) : 0;
        if (mask & ~valid) {
            for (unsigned i = 0; i < n; i++) hyp_vcpu_put(targets[i]);
            return -EINVAL;
        }
    }
    int rc = 0;
    for (unsigned i = 0; i < n; i++) {
        if (ipi) rc = hyp_vcpu_inject_irq(targets[i], HYP_IRQ_SOFTWARE, 1);
        else {
            uint64_t flags = spin_lock_irqsave(&g_hyp_active_lock);
            if (targets[i]->claimed) cpus |= 1u << targets[i]->host_cpu;
            spin_unlock_irqrestore(&g_hyp_active_lock, flags);
        }
        if (rc) break;
    }
    if (!ipi) rc = hyp_vm_remote_fence(vm, cpus, kind);
    for (unsigned i = 0; i < n; i++) hyp_vcpu_put(targets[i]);
    return rc;
}

int hyp_vm_ipi(hyp_vm_t *vm, uint64_t mask, uint64_t base)
{
    return hyp_vm_hart_mask(vm, mask, base, 0, 1);
}
int hyp_vm_fence(hyp_vm_t *vm, uint64_t mask, uint64_t base, unsigned kind)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return hyp_vm_hart_mask(vm, mask, base, kind, 0);
}

void hyp_vm_fail(hyp_vm_t *vm)
{
    __atomic_store_n(&vm->broken, 1, __ATOMIC_RELEASE);
    for (unsigned id = 0; id < HYP_MAX_VCPUS; id++) {
        hyp_vcpu_t *target = hyp_vm_vcpu_get(vm, id);
        if (!target) continue;
        hyp_vcpu_control(target, HYP_CONTROL_STOP);
        hyp_vcpu_put(target);
    }
}

void hyp_vm_shutdown(hyp_vm_t *vm)
{
    __atomic_store_n(&vm->shutdown, 1, __ATOMIC_RELEASE);
    for (unsigned id = 0; id < HYP_MAX_VCPUS; id++) {
        hyp_vcpu_t *target = hyp_vm_vcpu_get(vm, id);
        if (!target) continue;
        hyp_vcpu_control(target, HYP_CONTROL_STOP);
        hyp_vcpu_put(target);
    }
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
    uint64_t retained = 0;
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
            if (!hyp_s2_unmap(vm, gpa + j * PAGE_SIZE)) frame_put(pfns[j]);
            else retained++; /* failed invalidation keeps table ownership */
        }
        if (cur != PFN_NONE)
            frame_put(cur);
        kerr("hyp: vm_load gpa=%lx len=%lu failed after %lu page(s): %d\n",
             (unsigned long)gpa, (unsigned long)len, (unsigned long)pages, rc);
    }

    if (rc) {
        if (vm->charge_cg)
            cg_mem_uncharge(vm->charge_cg, npages - retained);
        __atomic_add_fetch(&vm->charged_pages, retained, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&vm->charged_pages, npages, __ATOMIC_RELAXED);
    }
    kfree(pfns);
    hyp_vm_end_update(vm);
    return rc;
}

/* Pending controls are read on the owning CPU at every guest trap. They
 * never edit the live register bank from a remote CPU. */
int hyp_vcpu_control(hyp_vcpu_t *vcpu, unsigned command)
{
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC || command > HYP_CONTROL_STOP)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_hyp_active_lock);
    if (command == HYP_CONTROL_RESUME)
        __atomic_fetch_and(&vcpu->requests, ~HYP_REQUEST_PAUSE, __ATOMIC_RELEASE);
    else
        __atomic_fetch_or(&vcpu->requests,
                          command == HYP_CONTROL_STOP ? HYP_REQUEST_STOP :
                          command == HYP_CONTROL_PAUSE ? HYP_REQUEST_PAUSE : HYP_REQUEST_YIELD,
                          __ATOMIC_RELEASE);
    unsigned cpu = vcpu->claimed ? vcpu->host_cpu : CONFIG_NR_CPUS;
    spin_unlock_irqrestore(&g_hyp_active_lock, flags);
    if (cpu < CONFIG_NR_CPUS)
        smp_send_reschedule(cpu);
    return 0;
}

int hyp_vcpu_inject_irq(hyp_vcpu_t *vcpu, unsigned irq, int level)
{
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC || irq > HYP_IRQ_EXTERNAL ||
        (level != 0 && level != 1)) return -EINVAL;
    if (!vcpu->vm->backend->sync_irqs) return -EOPNOTSUPP;
    unsigned bit = 1u << irq;
    uint64_t flags = spin_lock_irqsave(&g_hyp_active_lock);
    if (level) { vcpu->irq_set |= bit; vcpu->irq_clear &= ~bit; }
    else { vcpu->irq_clear |= bit; vcpu->irq_set &= ~bit; }
    unsigned cpu = vcpu->claimed ? vcpu->host_cpu : CONFIG_NR_CPUS;
    spin_unlock_irqrestore(&g_hyp_active_lock, flags);
    if (cpu < CONFIG_NR_CPUS) smp_send_reschedule(cpu);
    return 0;
}

void hyp_vcpu_sync_irqs(hyp_vcpu_t *vcpu)
{
    uint64_t flags = spin_lock_irqsave(&g_hyp_active_lock);
    unsigned set = vcpu->irq_set, clear = vcpu->irq_clear;
    vcpu->irq_set = vcpu->irq_clear = 0;
    spin_unlock_irqrestore(&g_hyp_active_lock, flags);
    if ((set || clear) && vcpu->vm->backend->sync_irqs)
        vcpu->vm->backend->sync_irqs(vcpu, set, clear);
}

int hyp_vcpu_control_exit(hyp_vcpu_t *vcpu)
{
    unsigned request = __atomic_load_n(&vcpu->requests, __ATOMIC_ACQUIRE);
    if (request & HYP_REQUEST_STOP)
        vcpu->exit = HYP_EXIT_STOPPED;
    else if (request & HYP_REQUEST_PAUSE)
        vcpu->exit = HYP_EXIT_PAUSED;
    else if (request & HYP_REQUEST_YIELD) {
        __atomic_fetch_and(&vcpu->requests, ~HYP_REQUEST_YIELD, __ATOMIC_ACQ_REL);
        vcpu->exit = HYP_EXIT_YIELD;
    } else return 0;
    return 1;
}

int hyp_vcpu_run_result(hyp_vcpu_t *vcpu, hyp_exit_reason_t *reason)
{
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC ||
        !vcpu->vm || vcpu->vm->magic != HYP_VM_MAGIC)
        return -EINVAL;
    int irqs_were_on = arch_irqs_enabled();
    preempt_disable();
    arch_local_irq_disable();
    unsigned cpu = cpu_current_id();
    uint64_t s2_flags = spin_lock_irqsave(&vcpu->vm->s2_lock);
    uint64_t flags = spin_lock_irqsave(&g_hyp_active_lock);
    uint64_t vm_flags = spin_lock_irqsave(&vcpu->vm->control_lock);
    int legacy = !vcpu->vm->managed_console;
    if (vcpu->claimed || hyp_active_vcpus[cpu] || vcpu->vm->updating ||
        vcpu->vm->run_count >= vcpu->vm->parallel_limit || (legacy && hyp_legacy_vcpu)) {
        spin_unlock_irqrestore(&vcpu->vm->control_lock, vm_flags);
        spin_unlock_irqrestore(&g_hyp_active_lock, flags);
        spin_unlock_irqrestore(&vcpu->vm->s2_lock, s2_flags);
        preempt_enable();
        if (irqs_were_on) arch_local_irq_enable();
        return -EBUSY;
    }
    if (__atomic_load_n(&vcpu->vm->broken, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&vcpu->vm->shutdown, __ATOMIC_ACQUIRE)) {
        int broken = __atomic_load_n(&vcpu->vm->broken, __ATOMIC_ACQUIRE);
        vcpu->exit = broken ? HYP_EXIT_ERROR : HYP_EXIT_STOPPED;
        if (reason) *reason = vcpu->exit;
        spin_unlock_irqrestore(&vcpu->vm->control_lock, vm_flags);
        spin_unlock_irqrestore(&g_hyp_active_lock, flags);
        spin_unlock_irqrestore(&vcpu->vm->s2_lock, s2_flags);
        preempt_enable();
        if (irqs_were_on) arch_local_irq_enable();
        return broken ? -EIO : 0;
    }
    if (__atomic_load_n(&vcpu->hart_state, __ATOMIC_ACQUIRE) == HYP_HART_STOPPED &&
        !(__atomic_load_n(&vcpu->requests, __ATOMIC_ACQUIRE) & HYP_REQUEST_STOP)) {
        vcpu->exit = HYP_EXIT_HART_STOPPED;
        if (reason) *reason = HYP_EXIT_HART_STOPPED;
        spin_unlock_irqrestore(&vcpu->vm->control_lock, vm_flags);
        spin_unlock_irqrestore(&g_hyp_active_lock, flags);
        spin_unlock_irqrestore(&vcpu->vm->s2_lock, s2_flags);
        preempt_enable();
        if (irqs_were_on) arch_local_irq_enable();
        return 0;
    }
    vcpu->claimed = 1;
    vcpu->host_cpu = cpu;
    vcpu->vm->run_count++;
    if (vcpu->vm->run_count > vcpu->vm->peak_run_count)
        vcpu->vm->peak_run_count = vcpu->vm->run_count;
    vcpu->vm->active_cpu_mask |= 1u << cpu;
    hyp_vcpu_get(vcpu);
    if (legacy) hyp_legacy_vcpu = vcpu;
    __atomic_store_n(&hyp_active_vcpus[cpu], vcpu, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&vcpu->vm->control_lock, vm_flags);
    spin_unlock_irqrestore(&g_hyp_active_lock, flags);

    spin_unlock_irqrestore(&vcpu->vm->s2_lock, s2_flags);

    int rc = 0;
    vcpu->exit = HYP_EXIT_NONE;
    vcpu->exit_scause = vcpu->exit_stval = vcpu->exit_htval = 0;
    if (__atomic_load_n(&vcpu->vm->broken, __ATOMIC_ACQUIRE)) { rc = -EIO; goto out; }
    if (hyp_vcpu_control_exit(vcpu)) goto out;
    if (__atomic_load_n(&vcpu->hart_state, __ATOMIC_ACQUIRE) == HYP_HART_STOPPED) {
        vcpu->exit = HYP_EXIT_HART_STOPPED;
        goto out;
    }
    if (vcpu->vm->managed_console && hyp_console_pending(vcpu->vm)) {
        vcpu->exit = HYP_EXIT_YIELD;
        goto out;
    }
    rc = vcpu->vm->backend->load(vcpu);
    if (rc) goto out;
    hyp_vcpu_sync_irqs(vcpu);
    __atomic_store_n(&vcpu->hart_state, HYP_HART_STARTED, __ATOMIC_RELEASE);
    vcpu->running = 1;
    vcpu->vm->backend->enter(vcpu);
    arch_local_irq_disable();
    if (vcpu->vm->backend->put) vcpu->vm->backend->put(vcpu);
    vcpu->running = 0;
    if (vcpu->exit == HYP_EXIT_NONE || vcpu->exit == HYP_EXIT_ERROR) rc = -EIO;
out:
    int failed = rc < 0 || vcpu->exit == HYP_EXIT_FAULT || vcpu->exit == HYP_EXIT_ERROR;
    if (failed) __atomic_store_n(&vcpu->vm->broken, 1, __ATOMIC_RELEASE);
    flags = spin_lock_irqsave(&g_hyp_active_lock);
    vm_flags = spin_lock_irqsave(&vcpu->vm->control_lock);
    if (reason) *reason = vcpu->exit;
    vcpu->vm->last_exit[0] = rc < 0 ? HYP_EXIT_ERROR : vcpu->exit;
    vcpu->vm->last_exit[1] = vcpu->exit_scause;
    vcpu->vm->last_exit[2] = vcpu->exit_stval;
    vcpu->vm->last_exit[3] = vcpu->exit_htval;
    vcpu->vm->run_count--;
    vcpu->vm->active_cpu_mask &= ~(1u << cpu);
    if (vcpu->exit == HYP_EXIT_HART_STOPPED)
        __atomic_store_n(&vcpu->hart_state, HYP_HART_STOPPED, __ATOMIC_RELEASE);
    vcpu->claimed = 0;
    __atomic_store_n(&hyp_active_vcpus[cpu], NULL, __ATOMIC_RELEASE);
    if (hyp_legacy_vcpu == vcpu) hyp_legacy_vcpu = NULL;
    spin_unlock_irqrestore(&vcpu->vm->control_lock, vm_flags);
    spin_unlock_irqrestore(&g_hyp_active_lock, flags);
    if (failed) hyp_vm_fail(vcpu->vm);
    hyp_vcpu_put(vcpu);
    preempt_enable();
    if (irqs_were_on) arch_local_irq_enable();
    return rc;
}

int hyp_vcpu_run(hyp_vcpu_t *vcpu)
{
    return hyp_vcpu_run_result(vcpu, NULL);
}
