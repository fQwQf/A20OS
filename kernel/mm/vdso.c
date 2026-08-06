/*
 * A20OS vDSO — kernel side (riscv64).
 *
 * Design reference: docs/hybrid-kernel/00-design.md §7.
 *
 * The vDSO image (built from arch/riscv64/vdso/) is embedded via objcopy
 * and mapped read-only+executable at A20_VDSO_VA into every Linux-ABI task
 * at exec; one global vvar data page is mapped read-only at A20_VVAR_VA.
 * Both VMAs are VM_PFNMAP|VM_DONTFORK so the generic VMA/fault/teardown
 * paths ignore them; fork re-maps them explicitly via vdso_fork_map().
 *
 * The vvar anchor is written once at boot and again whenever the realtime
 * clock is set; monotonic time needs no updates because both the kernel
 * and the vDSO read the same free-running time CSR.
 */
#include "core/arch.h"

#ifdef ARCH_HAS_VDSO

#include "core/types.h"
#include "core/string.h"
#include "core/klog.h"
#include "mm/vdso.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "core/timer.h"

extern const uint8_t _binary_vdso_elf_start[];
extern const uint8_t _binary_vdso_elf_end[];

#define A20_USER_STACK_LIMIT_VA \
    ((USER_STACK_TOP + PAGE_SIZE) - USER_STACK_MAX_SIZE)

_Static_assert((A20_VDSO_VA & PAGE_OFFSET_MASK) == 0,
               "vDSO address must be page aligned");
_Static_assert((A20_VVAR_VA & PAGE_OFFSET_MASK) == 0,
               "vvar address must be page aligned");
_Static_assert(A20_VDSO_VA + A20_VDSO_MAX_PAGES * PAGE_SIZE <= A20_VVAR_VA,
               "reserved vDSO image overlaps vvar");
_Static_assert(A20_VVAR_VA + 2 * PAGE_SIZE <= A20_USER_STACK_LIMIT_VA,
               "vvar must stay below the maximum user stack with a guard page");

static pfn_t          g_vdso_pfn[A20_VDSO_MAX_PAGES];
static uint32_t       g_vdso_pages;
static pfn_t          g_vvar_pfn;
static a20_vvar_t    *g_vvar;

void vdso_init(uint64_t boot_cycles, uint64_t timer_freq)
{
    /* The raw vdso.elf file is embedded: with p_offset == p_vaddr for the
     * single LOAD segment, the file layout is the in-memory layout, ELF
     * header included (musl parses it via AT_SYSINFO_EHDR). */
    uint32_t size = (uint32_t)(_binary_vdso_elf_end - _binary_vdso_elf_start);
    g_vdso_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (g_vdso_pages == 0 || g_vdso_pages > A20_VDSO_MAX_PAGES) {
        klog(KLOG_ERR, "vdso: bad image size %u\n", size);
        g_vdso_pages = 0;
        return;
    }

    for (uint32_t i = 0; i < g_vdso_pages; i++) {
        g_vdso_pfn[i] = pfa_alloc_page();
        if (g_vdso_pfn[i] == PFN_NONE) {
            while (i > 0)
                pfa_free_page(g_vdso_pfn[--i]);
            g_vdso_pages = 0;
            klog(KLOG_ERR, "vdso: out of frames\n");
            return;
        }
        void *va = pfn_to_virt(g_vdso_pfn[i]);
        memset(va, 0, PAGE_SIZE);
        uint32_t off = i * PAGE_SIZE;
        uint32_t n = (size - off < PAGE_SIZE) ? size - off : PAGE_SIZE;
        memcpy(va, _binary_vdso_elf_start + off, n);
    }

    g_vvar_pfn = pfa_alloc_page();
    if (g_vvar_pfn == PFN_NONE) {
        for (uint32_t i = 0; i < g_vdso_pages; i++)
            pfa_free_page(g_vdso_pfn[i]);
        g_vdso_pages = 0;
        klog(KLOG_ERR, "vdso: out of frames for vvar\n");
        return;
    }
    g_vvar = (a20_vvar_t *)pfn_to_virt(g_vvar_pfn);
    memset(g_vvar, 0, PAGE_SIZE);

    g_vvar->mult = ((uint64_t)1000000000ULL << 32) / timer_freq;
    g_vvar->boot_cycles = boot_cycles;
    /* seq starts at 0; realtime anchor is filled by vdso_sync_realtime()
     * from timekeeping_init right after this. */

    klog(KLOG_INFO, "vdso: %u page(s) at 0x%lx, vvar at 0x%lx\n",
         g_vdso_pages, (unsigned long)A20_VDSO_VA, (unsigned long)A20_VVAR_VA);
}

void vdso_sync_realtime(uint64_t sec, uint64_t nsec, uint64_t base_cyc)
{
    if (!g_vvar)
        return;
    /* Seqlock: the vDSO reader retries while seq is odd or changes.  The
     * anchor cycle is the exact tick the timekeeping core recorded, so
     * the vDSO and the syscall path agree bit for bit. */
    g_vvar->seq++;
    __sync_synchronize();
    g_vvar->rt_base_cyc = base_cyc;
    g_vvar->rt_base_sec = sec;
    g_vvar->rt_base_nsec = nsec;
    __sync_synchronize();
    g_vvar->seq++;
}

vaddr_t vdso_auxv_ehdr(void)
{
    return g_vdso_pages ? (vaddr_t)A20_VDSO_VA : 0;
}

static void vdso_append_vma(vm_area_t **list, vm_area_t *newv)
{
    vm_area_t **pp = list;
    vm_area_t *prev = NULL;
    while (*pp && (*pp)->start < newv->start) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    newv->next = *pp;
    newv->prev = prev;
    if (*pp) (*pp)->prev = newv;
    *pp = newv;
}

/*
 * vdso_map_image — map the vDSO/vvar frames into @pgdir and append the
 * VMAs to @list.  The caller serializes the list (mm->lock, or the
 * exec-time image list which is not yet shared).
 */
int vdso_map_image(pt_root_t *pgdir, vm_area_t **list)
{
    if (!pgdir || !list || !g_vdso_pages || !g_vvar)
        return -1;

    pte_t code_f = PTE_V | PTE_U | PTE_R | PTE_X | PTE_A | PTE_LEAF;
    pte_t data_f = PTE_V | PTE_U | PTE_R | PTE_A | PTE_LEAF;

    for (uint32_t i = 0; i < g_vdso_pages; i++) {
        if (pt_map(pgdir, A20_VDSO_VA + (vaddr_t)i * PAGE_SIZE,
                   pfn_to_phys(g_vdso_pfn[i]), code_f) < 0)
            return -1;
    }
    if (pt_map(pgdir, A20_VVAR_VA, pfn_to_phys(g_vvar_pfn), data_f) < 0)
        return -1;

    vm_area_t *code = kcalloc(1, sizeof(*code));
    vm_area_t *data = kcalloc(1, sizeof(*data));
    if (!code || !data) {
        kfree(code);
        kfree(data);
        return -1;
    }
    code->start = A20_VDSO_VA;
    code->end = A20_VDSO_VA + (vaddr_t)g_vdso_pages * PAGE_SIZE;
    code->vm_flags = VM_READ | VM_EXEC | VM_SHARED | VM_PFNMAP | VM_DONTFORK;
    data->start = A20_VVAR_VA;
    data->end = A20_VVAR_VA + PAGE_SIZE;
    data->vm_flags = VM_READ | VM_SHARED | VM_PFNMAP | VM_DONTFORK;
    vdso_append_vma(list, code);
    vdso_append_vma(list, data);
    return 0;
}

static int vdso_map_mm(mm_struct_t *mm)
{
    if (!mm)
        return -1;
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    int r = vdso_map_image(mm->pgdir, &mm->mmap);
    if (r == 0)
        mm->has_vdso = 1;
    spin_unlock_irqrestore(&mm->lock, flags);
    return r;
}

int vdso_exec_map(mm_struct_t *mm)
{
    int r = vdso_map_mm(mm);
    if (r < 0)
        klog(KLOG_ERR, "vdso: exec map failed\n");
    return r;
}

int vdso_fork_map(mm_struct_t *child_mm)
{
    /* Called from mm_fork; frames are global and never freed, so this
     * cannot fail for lack of the pages themselves. */
    return vdso_map_mm(child_mm);
}

#endif /* ARCH_HAS_VDSO */
