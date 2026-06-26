#include "core/arch.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/string.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

void *sysv_shm_kcalloc(size_t nmemb, size_t size)
{
    return kcalloc(nmemb, size);
}

void sysv_shm_kfree(void *ptr)
{
    kfree(ptr);
}

void sysv_shm_zero_pfn(pfn_t pfn)
{
    if (pfn_valid(pfn))
        memset(pfn_to_virt(pfn), 0, PAGE_SIZE);
}

pfn_t sysv_shm_pfa_alloc_page(void)
{
    return pfa_alloc_page();
}

void sysv_shm_pfa_free_page(pfn_t pfn)
{
    pfa_free_page(pfn);
}

void sysv_shm_frame_put(pfn_t pfn)
{
    frame_put(pfn);
}

long sysv_shm_copy_to_user(void *dst, const void *src, size_t n)
{
    return copy_to_user(dst, src, n);
}

void *sysv_shm_proc_current(void)
{
    return proc_current();
}

void *sysv_shm_task_mm(void *task)
{
    if (!task)
        return NULL;
    return ((task_t *)task)->mm;
}

int sysv_shm_task_pid(void *task)
{
    if (!task)
        return 0;
    return ((task_t *)task)->pid;
}

size_t sysv_shm_page_size(void)
{
    return PAGE_SIZE;
}

uint64_t sysv_shm_mmap_base(void)
{
    return MMAP_BASE_ADDR;
}

static int shm_range_overlaps(mm_struct_t *mm, uint64_t start, size_t len)
{
    uint64_t end = start + len;
    if (end < start || end > USER_VA_LIMIT)
        return 1;
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (v->start < end && v->end > start)
            return 1;
        if (v->start >= end)
            break;
    }
    return 0;
}

static void shm_unmap_attached_pages(mm_struct_t *mm, uint64_t addr, size_t npages)
{
    for (size_t p = 0; p < npages; p++) {
        paddr_t pa = 0;
        uint64_t base = 0;
        size_t size = 0;
        if (pt_unmap_leaf(mm->pgdir, addr + p * PAGE_SIZE, &pa, &base, &size, NULL) == 0 && pa)
            frame_put(phys_to_pfn(pa));
    }
}

uint64_t sysv_shm_do_attach(void *mm_raw, uint64_t shmaddr, const pfn_t *pages,
                            size_t npages, size_t size, int shmid)
{
    mm_struct_t *mm = (mm_struct_t *)mm_raw;
    if (!mm || !pages || npages == 0 || size == 0)
        return (uint64_t)-EINVAL;

    uint64_t vm_flags = VM_SHARED | VM_READ | VM_WRITE | VM_SYSV_SHM;
    uint64_t pte_flags = mm_vm_flags_to_pte_flags(vm_flags);

    uint64_t addr = shmaddr;
    if (addr == 0)
        addr = mm_find_gap(mm, MMAP_BASE_ADDR, size);
    if (addr == 0)
        return (uint64_t)-ENOMEM;
    if ((addr & (PAGE_SIZE - 1)) || shm_range_overlaps(mm, addr, size))
        return (uint64_t)-EINVAL;

    vm_area_t *vma = kcalloc(1, sizeof(vm_area_t));
    if (!vma)
        return (uint64_t)-ENOMEM;

    size_t mapped = 0;
    for (size_t p = 0; p < npages; p++) {
        paddr_t pa = pfn_to_phys(pages[p]);
        if (pt_map(mm->pgdir, addr + p * PAGE_SIZE, pa, pte_flags) < 0) {
            shm_unmap_attached_pages(mm, addr, mapped);
            kfree(vma);
            return (uint64_t)-ENOMEM;
        }
        frame_get(pages[p]);
        mapped++;
    }

    vma->start = addr;
    vma->end = addr + size;
    vma->vm_flags = vm_flags;
    vma->pte_flags = pte_flags;
    vma->file_fd = -1;
    vma->sysv_shmid = shmid;
    mm_insert_vma(mm, vma);
    mm->total_vm += npages;

    return addr;
}

void *sysv_shm_mm_find_vma(void *mm_raw, uint64_t addr)
{
    mm_struct_t *mm = (mm_struct_t *)mm_raw;
    if (!mm)
        return NULL;
    return mm_find_vma(mm, addr);
}

int sysv_shm_vma_matches(void *vma_raw, uint64_t addr, int *shmid_out)
{
    vm_area_t *vma = (vm_area_t *)vma_raw;
    if (!vma || vma->start != addr || !(vma->vm_flags & VM_SYSV_SHM))
        return 0;
    if (shmid_out)
        *shmid_out = vma->sysv_shmid;
    return 1;
}

size_t sysv_shm_vma_len(void *vma_raw)
{
    vm_area_t *vma = (vm_area_t *)vma_raw;
    if (!vma)
        return 0;
    return vma->end - vma->start;
}

int sysv_shm_mm_munmap(void *mm_raw, uint64_t addr, size_t len)
{
    mm_struct_t *mm = (mm_struct_t *)mm_raw;
    if (!mm)
        return -EINVAL;
    return mm_munmap(mm, addr, len);
}
