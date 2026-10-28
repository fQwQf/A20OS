#include "core/kexec_backend.h"

#include "boot/context.h"
#include "core/defs.h"
#include "core/smp.h"
#include "core/string.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "proc/proc.h"

struct kexec_source_chain {
    struct kexec_source_chain *next;
    uint8_t data[PAGE_SIZE - sizeof(void *)];
};

static int ranges_overlap(uint64_t a, uint64_t asz, uint64_t b, uint64_t bsz)
{
    uint64_t aend;
    uint64_t bend;
    if (!asz || !bsz)
        return 0;
    if (__builtin_add_overflow(a, asz, &aend) ||
        __builtin_add_overflow(b, bsz, &bend))
        return 1;
    return a < bend && b < aend;
}

static int destination_ram_ok(uint64_t address, uint64_t size)
{
    if (!size)
        return 1;
    uint64_t end;
    if (__builtin_add_overflow(address, size, &end) || end == address)
        return 0;
    const pfa_range_t *first = pfa_range_for_pa(address);
    const pfa_range_t *last = pfa_range_for_pa(end - 1);
    return first && first == last && address >= first->base && end <= first->end;
}

static int destination_sources_ok(const a20_elf_kexec_ledger_t *image,
                                  uint64_t destination, uint64_t size,
                                  uint64_t control_pa)
{
    const boot_context_t *boot = boot_context_get();
    if (boot && ranges_overlap(destination, size, boot->fdt_physical,
                               boot->fdt_available))
        return 0;
    if (ranges_overlap(destination, size, control_pa, PAGE_SIZE))
        return 0;
    for (uint32_t i = 0; i < image->nr; i++) {
        const struct kexec_source_chain *chain =
            (const void *)(uintptr_t)image->seg[i].pages;
        uint64_t remaining = image->seg[i].pages_bytes;
        while (chain && remaining) {
            uint64_t pa = (uint64_t)va_to_pa(chain);
            if (ranges_overlap(destination, size, pa, PAGE_SIZE))
                return 0;
            chain = chain->next;
            remaining = remaining > PAGE_SIZE - sizeof(void *)
                        ? remaining - (PAGE_SIZE - sizeof(void *)) : 0;
        }
        if (remaining)
            return 0;
    }
    return 1;
}

int kexec_handoff_build(const a20_elf_kexec_ledger_t *image,
                        kexec_handoff_t *control, uint64_t control_pa,
                        uint64_t boot_arg0, uint64_t boot_arg1)
{
    if (!image || !control || image->state != A20_ELF_KX_LOADED || !image->nr ||
        image->nr > KEXEC_MAX_SEGMENTS || image->nstaged != image->nr ||
        (image->entry & 1))
        return -ENOEXEC;
    memset(control, 0, sizeof(*control));
    control->entry = image->entry;
    control->boot_arg0 = boot_arg0;
    control->boot_arg1 = boot_arg1;
    control->nr = image->nr;
    for (uint32_t i = 0; i < image->nr; i++) {
        const a20_elf_kexec_slot_t *slot = &image->seg[i];
        if (slot->bufsz > slot->memsz || slot->pages_bytes != slot->memsz ||
            !destination_ram_ok(slot->mem, slot->memsz) ||
            !destination_sources_ok(image, slot->mem, slot->memsz, control_pa))
            return -EADDRNOTAVAIL;
        uint64_t bytes = 0;
        const struct kexec_source_chain *chain =
            (const void *)(uintptr_t)slot->pages;
        while (bytes < slot->memsz) {
            if (!chain)
                return -EIO;
            chain = chain->next;
            bytes += PAGE_SIZE - sizeof(void *);
        }
        control->seg[i].pages = slot->pages
                                ? (uint64_t)va_to_pa((const void *)(uintptr_t)slot->pages) : 0;
        control->seg[i].mem = slot->mem;
        control->seg[i].bufsz = slot->bufsz;
        control->seg[i].memsz = slot->memsz;
    }
    return 0;
}

int kexec_bind_boot_cpu(uint32_t *saved_affinity, int *changed)
{
    if (!saved_affinity || !changed)
        return -EINVAL;
    task_t *task = proc_current();
    if (!task)
        return -ESRCH;
    if (arch_current_cpu_id() == 0)
        return 0;
    *saved_affinity = task->cpus_allowed;
    proc_sched_config_t config = {
        .fields = PROC_SCHED_AFFINITY,
        .affinity = 1,
    };
    int rc = proc_sched_set(task, &config);
    if (rc < 0)
        return rc;
    *changed = 1;
    sched();
    return arch_current_cpu_id() == 0 ? 0 : -EBUSY;
}

void kexec_restore_affinity(uint32_t saved_affinity, int changed)
{
    if (!changed)
        return;
    proc_sched_config_t config = {
        .fields = PROC_SCHED_AFFINITY,
        .affinity = saved_affinity,
    };
    (void)proc_sched_set(proc_current(), &config);
    sched();
}
