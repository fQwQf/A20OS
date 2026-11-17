/*
 * Hypervisor foundation, machine-independent half: the stage-2 address
 * space and the VM container.  See kernel/include/hyp/hyp.h for the
 * contract and docs/zh/hypervisor/00-design.md for the design record.
 */
#include "hyp/hyp.h"
#include "hyp/hyp_vcpu.h"
#include "mm/pt.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "cg/cgroup.h"
#include "cg/cgroup_impl.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/string.h"

/* Host PTE permission input is translated by the selected stage-2 backend. */
#define HYP_RAM_PAGE_PROT (PTE_R | PTE_W | HYP_PROT_EXEC)

static uint64_t hyp_live_vms, hyp_live_vcpus, hyp_mapped_pages, hyp_table_pages;
void hyp_account_vcpu(int delta)
{
    __atomic_fetch_add(&hyp_live_vcpus, delta, __ATOMIC_RELAXED);
}
void hyp_account_mapped(int delta)
{
    __atomic_fetch_add(&hyp_mapped_pages, delta, __ATOMIC_RELAXED);
}
void hyp_account_tables(int delta)
{
    __atomic_fetch_add(&hyp_table_pages, delta, __ATOMIC_RELAXED);
}
void hyp_stats_get(struct hyp_stats *out)
{
    out->vms = __atomic_load_n(&hyp_live_vms, __ATOMIC_ACQUIRE);
    out->vcpus = __atomic_load_n(&hyp_live_vcpus, __ATOMIC_ACQUIRE);
    out->mapped_pages = __atomic_load_n(&hyp_mapped_pages, __ATOMIC_ACQUIRE);
    out->table_pages = __atomic_load_n(&hyp_table_pages, __ATOMIC_ACQUIRE);
    out->free_host_pages = pfa_free_count();
}

hyp_vm_t *hyp_vm_create(uint64_t mem_size)
{
    const hyp_backend_t *backend = hyp_arch_backend();
    if (!hyp_supported() || !backend || !backend->vm_init || !backend->vm_destroy ||
        !backend->map || !backend->unmap || !backend->translate ||
        !backend->load || !backend->enter || !backend->put ||
        mem_size == 0 || (mem_size & (PAGE_SIZE - 1)) ||
        backend->ram_base >= backend->gpa_limit ||
        mem_size > backend->gpa_limit - backend->ram_base)
        return NULL;
    hyp_vm_t *vm = kcalloc(1, sizeof(*vm));
    if (!vm)
        return NULL;
    vm->backend = backend;
    vm->mem_size = mem_size;
    vm->ram_base = backend->ram_base;
    vm->ram_size = mem_size;
    vm->refcount = 1;
    vm->vcpu_limit = vm->parallel_limit = 1;
    spin_init(&vm->control_lock);
    spin_init(&vm->console_lock);
    spin_init(&vm->s2_lock);
    task_t *owner = proc_current();
    vm->charge_cg = owner ? owner->cgroup : NULL;
    if (backend->vm_init(vm) != 0) {
        kfree(vm);
        return NULL;
    }
    vm->magic = HYP_VM_MAGIC;
    __atomic_fetch_add(&hyp_live_vms, 1, __ATOMIC_RELEASE);
    return vm;
}

void hyp_vm_put(hyp_vm_t *vm)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return;
    if (__atomic_sub_fetch(&vm->refcount, 1, __ATOMIC_ACQ_REL) != 0)
        return;
    vm->backend->vm_destroy(vm);
    if (vm->charge_cg && vm->charged_pages)
        cg_mem_uncharge(vm->charge_cg, vm->charged_pages);
    vm->magic = 0;
    __atomic_fetch_sub(&hyp_live_vms, 1, __ATOMIC_RELEASE);
    kfree(vm);
}

int hyp_vm_configure_smp(hyp_vm_t *vm, unsigned count)
{
    if (!vm || vm->magic != HYP_VM_MAGIC || !count || count > HYP_MAX_VCPUS)
        return -EINVAL;
    int rc = hyp_vm_begin_update(vm);
    if (rc) return rc;
    uint64_t flags = spin_lock_irqsave(&vm->control_lock);
    unsigned existing = vm->vcpu_count;
    spin_unlock_irqrestore(&vm->control_lock, flags);
    if (existing) rc = -EBUSY;
    else if (count > 1 && (!vm->backend->reset || !vm->backend->probe_smp))
        rc = -EOPNOTSUPP;
    else if (count > 1) rc = vm->backend->probe_smp(vm);
    if (!rc) {
        vm->vcpu_limit = count;
        vm->parallel_limit = count < vm->backend->max_running_vcpus ?
                             count : vm->backend->max_running_vcpus;
    }
    hyp_vm_end_update(vm);
    return rc;
}

int hyp_vm_remote_fence(hyp_vm_t *vm, uint32_t cpus, unsigned kind)
{
    cpus &= ~(1u << cpu_current_id());
    if (!cpus) return 0;
    if (!vm->backend->remote_fence) return -EOPNOTSUPP;
    return vm->backend->remote_fence(vm, cpus, kind);
}

int hyp_s2_map(hyp_vm_t *vm, uint64_t gpa, pfn_t pfn, pte_t prot)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return -EINVAL;
    return vm->backend->map(vm, gpa, pfn, prot);
}

int hyp_s2_unmap(hyp_vm_t *vm, uint64_t gpa)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return -EINVAL;
    return vm->backend->unmap(vm, gpa);
}

paddr_t hyp_s2_translate(hyp_vm_t *vm, uint64_t gpa)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return 0;
    return vm->backend->translate(vm, gpa);
}

static int hyp_vm_copy_guest(hyp_vm_t *vm, uint64_t gpa, void *bytes,
                             uint64_t len, int write)
{
    if (!vm || vm->magic != HYP_VM_MAGIC || (!bytes && len)) return -EINVAL;
    if (!len) return 0;
    if (!vm->backend->pin) return -EOPNOTSUPP;
    hyp_vm_get(vm);
    uint64_t flags = spin_lock_irqsave(&vm->control_lock);
    uint64_t base = vm->ram_base, size = vm->ram_size;
    int updating = vm->updating;
    spin_unlock_irqrestore(&vm->control_lock, flags);
    if (updating || len > size || gpa < base || gpa - base > size - len) {
        hyp_vm_put(vm);
        return updating ? -EBUSY : -EFAULT;
    }
    int rc = 0;
    uint64_t done = 0;
    while (done < len) {
        pfn_t pfn;
        rc = vm->backend->pin(vm, gpa, &pfn);
        if (rc) break;
        uint64_t offset = gpa & (PAGE_SIZE - 1);
        uint64_t n = PAGE_SIZE - offset;
        if (n > len - done) n = len - done;
        uint8_t *page = (uint8_t *)pfn_to_virt(pfn) + offset;
        if (write) memcpy(page, (uint8_t *)bytes + done, n);
        else memcpy((uint8_t *)bytes + done, page, n);
        frame_put(pfn);
        gpa += n; done += n;
    }
    hyp_vm_put(vm);
    return rc;
}

int hyp_vm_copy_from_guest(hyp_vm_t *vm, void *dst, uint64_t gpa, uint64_t len)
{
    return hyp_vm_copy_guest(vm, gpa, dst, len, 0);
}

int hyp_vm_copy_to_guest(hyp_vm_t *vm, uint64_t gpa, const void *src, uint64_t len)
{
    return hyp_vm_copy_guest(vm, gpa, (void *)src, len, 1);
}

int hyp_s2_audit(hyp_vm_t *vm, mm_pt_audit_report_t *out)
{
    if (!vm || vm->magic != HYP_VM_MAGIC || !vm->backend->audit)
        return -EOPNOTSUPP;
    return vm->backend->audit(vm, out);
}

/* Reserve configuration work without holding a spinlock over allocation or
 * user copy. RUN and configuration changes cannot overlap. */
int hyp_vm_begin_update(hyp_vm_t *vm)
{
    uint64_t flags = spin_lock_irqsave(&vm->control_lock);
    int rc = vm->updating || vm->run_count ? -EBUSY : 0;
    if (!rc)
        vm->updating = 1;
    spin_unlock_irqrestore(&vm->control_lock, flags);
    return rc;
}

void hyp_vm_end_update(hyp_vm_t *vm)
{
    uint64_t flags = spin_lock_irqsave(&vm->control_lock);
    vm->updating = 0;
    spin_unlock_irqrestore(&vm->control_lock, flags);
}

void hyp_vm_get(hyp_vm_t *vm)
{
    if (vm)
        __atomic_add_fetch(&vm->refcount, 1, __ATOMIC_RELAXED);
}

void hyp_vm_get_status(hyp_vm_t *vm, struct hyp_vm_status *status)
{
    uint64_t flags = spin_lock_irqsave(&vm->control_lock);
    status->exit = __atomic_load_n(&vm->broken, __ATOMIC_ACQUIRE) ? HYP_EXIT_ERROR :
                   __atomic_load_n(&vm->shutdown, __ATOMIC_ACQUIRE) ? HYP_EXIT_SHUTDOWN : vm->last_exit[0];
    status->scause = vm->last_exit[1];
    status->stval = vm->last_exit[2];
    status->htval = vm->last_exit[3];
    spin_unlock_irqrestore(&vm->control_lock, flags);
    status->marker_seen = hyp_vm_marker_seen(vm);
    status->console_bytes = hyp_vm_console_bytes(vm);
    status->rx_bytes = hyp_vm_rx_bytes(vm);
}

/* ---- v2: the on-demand RAM window (hyp_vcpu.h) ---- */

int hyp_vm_set_ram(hyp_vm_t *vm, uint64_t base, uint64_t size)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return -EINVAL;
    if (size == 0)
        return -EINVAL;
    /* The window must be inside what hgatp can name: this implementation
     * programs root entries [0,512) only, so a window that reaches past
     * HYP_GPA_LIMIT would be silently unreachable rather than refused. */
    if (size > vm->mem_size || size > vm->backend->gpa_limit ||
        base > vm->backend->gpa_limit - size ||
        ((base | size) & (PAGE_SIZE - 1)))
        return -ERANGE;

    /* Plain stores, and that is enough: the host sets the window before the
     * run, and while a guest is live only hyp_ram_fill() reads it -- on the
     * CPU running the guest, which is the CPU that published it. */
    int rc = hyp_vm_begin_update(vm);
    if (rc)
        return rc;
    if (vm->charged_pages) {
        hyp_vm_end_update(vm);
        return -EBUSY;
    }
    vm->ram_base = base;
    vm->ram_size = size;
    hyp_vm_end_update(vm);
    return 0;
}

int hyp_vm_ram_contains(hyp_vm_t *vm, uint64_t gpa)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return 0;
    return gpa >= vm->ram_base && gpa - vm->ram_base < vm->ram_size;
}

/*
 * Demand-fill one guest page.  The second-stage trap handler calls this with
 * the htval address and the run loop resumes the guest when it returns 0; a
 * guest access outside the window is not ours to serve and stays a fault.
 *
 * Zeroed, because the window is a supply channel for memory the guest has not
 * written yet (its own page tables, bss, fresh stacks) and a recycled frame's
 * old contents would be the guest's first read of its own address space.
 */
int hyp_ram_fill(hyp_vm_t *vm, uint64_t gpa)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return -EINVAL;
    if (gpa & (PAGE_SIZE - 1))
        return -EINVAL;
    if (!hyp_vm_ram_contains(vm, gpa))
        return -EFAULT;
#if (defined(ARCH_HAS_PGTABLE_OPS) || defined(ARCH_HAS_HYP_S2)) && !defined(CONFIG_NOMMU)

    /* Already translated: there is nothing to fill.  Reached when the guest
     * touches the page twice through two different faults (or when a caller
     * re-asks), and answering 0 keeps the call idempotent for its only two
     * possible callers. */
    if (hyp_s2_translate(vm, gpa) != 0)
        return 0;

    pfn_t pfn = pfa_alloc_flags(0, 0);
    if (pfn == PFN_NONE)
        return -ENOMEM;
    memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

    /* Charged to whoever is running the guest, the same rule every other
     * allocation on this path uses (mm/fault.c, ipc/userfaultfd.c): the cgroup
     * of the current task is the thing that gets the guest's memory bill. */
    if (vm->charge_cg && cg_mem_charge(vm->charge_cg, 1) != 0) {
        frame_put(pfn);
        return -ENOMEM;
    }

    int rc = hyp_s2_map(vm, gpa, pfn, HYP_RAM_PAGE_PROT);
    if (rc) {
        if (vm->charge_cg)
            cg_mem_uncharge(vm->charge_cg, 1);
        frame_put(pfn);
        return rc == -EEXIST ? 0 : rc;
    }
    __atomic_add_fetch(&vm->charged_pages, 1, __ATOMIC_RELAXED);
    return 0;
#else
    /* Without a stage-2 there is nowhere to put the page even if one could be
     * allocated, and -EFAULT is deliberately NOT what this returns: the
     * second-stage fault path in hyp_vcpu.c reads -EFAULT as "outside the RAM
     * window, try the device model", and handing it a page the kernel cannot
     * back would send the guest at a GPA it never owns. */
    (void)gpa;
    return -ENOSYS;
#endif /* ARCH_HAS_PGTABLE_OPS && !CONFIG_NOMMU */
}

/* ---- kernel-side selftest (docs/zh/hypervisor/00-design.md, S4) ---- */

#if (defined(ARCH_HAS_PGTABLE_OPS) || defined(ARCH_HAS_HYP_S2)) && !defined(CONFIG_NOMMU)

static int st_fail(const char *step)
{
    kinfo("HYP_SELFTEST: FAIL at %s\n", step);
    return -1;
}

int hyp_selftest(void)
{
    if (!hyp_supported()) {
        kinfo("HYP_SELFTEST: SKIP (no virtualization extension)\n");
        return 0;
    }

    kinfo("HYP_SELFTEST: create\n");
    hyp_vm_t *vm = hyp_vm_create(4 << 20);
    if (!vm)
        return st_fail("create");

    /* Map four pages at the demo guest's RAM base, each holding its own
     * pattern, and read them back THROUGH the stage-2 translation -- that
     * is the pair (gpa->host pa) and (host pa -> bytes) agreeing. */
    const uint64_t base = vm->ram_base;
    pfn_t pfns[4];
    for (int i = 0; i < 4; i++) {
        pfns[i] = pfa_alloc_page();
        if (pfns[i] == PFN_NONE)
            return st_fail("alloc");
        memset(pfn_to_virt(pfns[i]), 0x41 + i, PAGE_SIZE);
        if (hyp_s2_map(vm, base + (uint64_t)i * PAGE_SIZE, pfns[i],
                       PTE_R | PTE_W) != 0)
            return st_fail("map");
    }
    for (int i = 0; i < 4; i++) {
        paddr_t pa = hyp_s2_translate(vm, base + (uint64_t)i * PAGE_SIZE);
        if (pa != pfn_to_phys(pfns[i]))
            return st_fail("translate");
        if (*(volatile uint8_t *)pfn_to_virt(pfns[i]) != (uint8_t)(0x41 + i))
            return st_fail("readback");
    }

    /* The lend is visible on both sides: every mapped frame carries
     * FRAME_F_GUEST, and the stage-2 audit cross-checks flag against
     * mapping -- a lent-but-unmapped or mapped-but-unlent frame fails
     * here. */
    for (int i = 0; i < 4; i++)
        if (!frame_is_lent_to_guest(pfns[i]))
            return st_fail("lend-flag");
    mm_pt_audit_report_t rep;
    if (hyp_s2_audit(vm, &rep) != 0)
        return st_fail("audit");
    if (rep.entries == 0 || rep.seg_bad_slot != 0)
        return st_fail("audit-counters");

    /* Unmap one page: translation goes away and the frame comes back
     * (flag cleared, still holding the caller's reference). */
    if (hyp_s2_unmap(vm, base + 2 * PAGE_SIZE) != 0)
        return st_fail("unmap");
    if (hyp_s2_translate(vm, base + 2 * PAGE_SIZE) != 0)
        return st_fail("unmap-translate");
    if (frame_is_lent_to_guest(pfns[2]))
        return st_fail("unmap-return");
    if (hyp_s2_map(vm, base + 2 * PAGE_SIZE, pfns[2], PTE_R | PTE_W) != 0)
        return st_fail("remap");
    if (hyp_s2_map(vm, base + 2 * PAGE_SIZE, pfns[2], PTE_R | PTE_W) != -EEXIST)
        return st_fail("remap-eexist");

    /* ---- failure paths: every refusal must leave the address space and the
     * frame accounting exactly as it found them.  These are the negative
     * controls HYP-02 asks for; a map that refuses after taking ownership, or
     * a load that keeps half its pages, is a leak that only shows up after
     * enough failed creates. */
    uint8_t *failbuf = kmalloc(PAGE_SIZE * 2);
    if (!failbuf)
        return st_fail("failbuf");
    memset(failbuf, 0x5a, PAGE_SIZE * 2);

    /* Out-of-window and bad-frame refusals happen before any ownership moves. */
    if (hyp_s2_map(vm, vm->backend->gpa_limit, pfns[0], PTE_R) != -ERANGE)
        return st_fail("map-range");
    if (hyp_s2_map(vm, base + 5 * PAGE_SIZE, PFN_NONE, PTE_R) != -EINVAL)
        return st_fail("map-bad-pfn");
    if (hyp_s2_map(vm, base + 5 * PAGE_SIZE, virt_to_pfn(vm->s2_root), PTE_R) != -EBUSY)
        return st_fail("map-page-table-frame");
    if (hyp_s2_map(vm, base + 5 * PAGE_SIZE, pfns[0], PTE_R) != -EBUSY)
        return st_fail("map-duplicate-frame");
    if (hyp_ram_fill(vm, 0) != -EFAULT)
        return st_fail("fill-outside-window");
    /* The UART is device space, not RAM: a fill there must refuse and must
     * not allocate a frame. */
    if (hyp_ram_fill(vm, 0x10000000ULL) != -EFAULT)
        return st_fail("fill-device-gpa");

    /* A load that collides with an existing mapping keeps the old mapping,
     * returns the frames it allocated, and unmaps the pages it installed.
     * Map only the SECOND page of the range so the first one is installed and
     * has to be unwound. */
    uint64_t region = base + 8 * PAGE_SIZE;
    pfn_t keep = pfa_alloc_page();
    if (keep == PFN_NONE)
        return st_fail("keep-alloc");
    memset(pfn_to_virt(keep), 0x33, PAGE_SIZE);
    if (hyp_s2_map(vm, region + PAGE_SIZE, keep, PTE_R | PTE_W) != 0)
        return st_fail("partial-setup");
    /* A failed load may leave the page-table nodes it created in place (they
     * are shared and reusable); what must be identical is the mapped-page
     * count and the surviving mapping. */
    struct hyp_stats st_before, st_after;
    hyp_stats_get(&st_before);
    if (hyp_vm_load(vm, region, failbuf, PAGE_SIZE * 2) != -EEXIST)
        return st_fail("load-partial-refuse");
    if (hyp_s2_translate(vm, region) != 0)
        return st_fail("load-partial-unwind");
    if (hyp_s2_translate(vm, region + PAGE_SIZE) != pfn_to_phys(keep))
        return st_fail("load-partial-clobber");
    hyp_stats_get(&st_after);
    if (st_after.mapped_pages != st_before.mapped_pages)
        return st_fail("load-partial-leak");
    if (hyp_vm_load(vm, 0xFFFFFFF000ULL, failbuf, PAGE_SIZE) != -EINVAL)
        return st_fail("load-out-of-window");
    hyp_stats_get(&st_after);
    if (st_after.mapped_pages != st_before.mapped_pages)
        return st_fail("refusal-leak");
    if (hyp_s2_unmap(vm, region + PAGE_SIZE) != 0)
        return st_fail("partial-teardown");
    frame_put(keep);

    /* Low memory: a cgroup with a zero limit refuses the charge, so both the
     * bulk loader and the demand-fill path must return -ENOMEM without leaving
     * a mapping or a frame behind.  Using the real charge path keeps this
     * deterministic instead of trying to exhaust the machine. */
    cg_node_t tiny;
    memset(&tiny, 0, sizeof(tiny));
    spin_init(&tiny.lock);
    cg_mem_init(&tiny.res.mem);
    tiny.res.mem.limit = 0;
    struct cg_node *saved_cg = vm->charge_cg;
    vm->charge_cg = &tiny;
    uint64_t oom_gpa = region + 16 * PAGE_SIZE;
    struct hyp_stats st_oom0, st_oom1;
    hyp_stats_get(&st_oom0);
    if (hyp_vm_load(vm, oom_gpa, failbuf, PAGE_SIZE) != -ENOMEM)
        return st_fail("load-oom");
    if (hyp_s2_translate(vm, oom_gpa) != 0)
        return st_fail("load-oom-map");
    if (hyp_ram_fill(vm, oom_gpa) != -ENOMEM)
        return st_fail("fill-oom");
    hyp_stats_get(&st_oom1);
    if (st_oom1.mapped_pages != st_oom0.mapped_pages)
        return st_fail("oom-leak");
    vm->charge_cg = saved_cg;
    kfree(failbuf);

    /* Exercise the final root slot, including Sv32x4's fourth root page. */
    pfn_t edge = pfa_alloc_page();
    uint64_t edge_gpa = vm->backend->gpa_limit - PAGE_SIZE;
    if (edge == PFN_NONE || hyp_s2_map(vm, edge_gpa, edge, PTE_R | PTE_W))
        return st_fail("edge-map");
    if (hyp_s2_translate(vm, edge_gpa) != pfn_to_phys(edge) ||
        hyp_s2_audit(vm, &rep))
        return st_fail("edge-audit");
    if (hyp_s2_unmap(vm, edge_gpa)) return st_fail("edge-unmap");
    frame_put(edge);

    /* Destroy: every lent frame returns with its reference dropped, so the
     * buddy sees exactly the four frames this test allocated, free again. */
    hyp_vm_put(vm);
    for (int i = 0; i < 4; i++) {
        if (frame_is_lent_to_guest(pfns[i]))
            return st_fail("destroy-flag");
    }

    kinfo("HYP_SELFTEST: PASS (vmid=%u entries=%lu)\n",
          0, (unsigned long)rep.entries);
    return 0;
}

#else /* no host page tables in this configuration */

int hyp_selftest(void)
{
    /* SKIP rather than FAIL, and NOT by way of the hyp_supported() test above:
     * that test asks whether the CPU has a virtualization extension, which on
     * this configuration can be answered 1 while every step below is
     * impossible.  Reporting the reason this build cannot run the test is the
     * useful half of the line; the caller still gets the contract's 0 for
     * "not a failure". */
    kinfo("HYP_SELFTEST: SKIP (no host page tables in this configuration)\n");
    return 0;
}

#endif /* ARCH_HAS_PGTABLE_OPS && !CONFIG_NOMMU */
