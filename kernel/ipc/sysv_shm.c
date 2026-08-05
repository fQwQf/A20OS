#include "ipc/sysv_shm.h"

#include "core/consts.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/string.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

#define IPC_CREAT   01000
#define IPC_EXCL    02000
#define IPC_64_BIT  0x100
#define IPC_RMID    0
#define IPC_SET     1
#define IPC_STAT    2
#define SHM_STAT_ANY 15
#define SHM_INFO      14

#define SYSV_SHM_MAX 32
#define SHM_MAX_PAGES 1024

typedef struct {
    int used;
    int marked_delete;
    int key;
    size_t size;
    pfn_t *pages;
    size_t npages;
    int nattach;
} sysv_shm_t;

static sysv_shm_t g_shm[SYSV_SHM_MAX];
static spinlock_t g_shm_lock = SPINLOCK_INIT;

static void sysv_shm_free_pages(pfn_t *pages, size_t npages)
{
    if (!pages)
        return;
    for (size_t p = 0; p < npages; p++)
        frame_put(pages[p]);
    kfree(pages);
}

static void sysv_shm_free_locked(int shmid, pfn_t **pages, size_t *npages)
{
    *pages = g_shm[shmid].pages;
    *npages = g_shm[shmid].npages;
    memset(&g_shm[shmid], 0, sizeof(sysv_shm_t));
}

static pfn_t *sysv_shm_alloc_pages(size_t npages)
{
    pfn_t *pages = kcalloc(npages, sizeof(pfn_t));
    if (!pages)
        return NULL;

    for (size_t p = 0; p < npages; p++) {
        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) {
            for (size_t j = 0; j < p; j++)
                pfa_free_page(pages[j]);
            kfree(pages);
            return NULL;
        }
        pages[p] = pfn;
        memset(pfn_to_virt(pfn), 0, PAGE_SIZE);
    }
    return pages;
}

static void sysv_shm_unmap_attached_pages(mm_struct_t *mm, uint64_t addr, size_t npages)
{
    for (size_t p = 0; p < npages; p++) {
        paddr_t pa = 0;
        vaddr_t base = 0;
        size_t size = 0;
        if (pt_unmap_leaf(mm->pgdir, addr + p * PAGE_SIZE, &pa, &base, &size, NULL) == 0 && pa)
            frame_put(phys_to_pfn(pa));
    }
}

static int sysv_shm_range_overlaps(mm_struct_t *mm, uint64_t start, size_t len)
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

void sysv_shm_unref_attach(int shmid)
{
    pfn_t *free_pages = NULL;
    size_t free_npages = 0;
    uint64_t flags = spin_lock_irqsave(&g_shm_lock);
    if (shmid >= 0 && shmid < SYSV_SHM_MAX && g_shm[shmid].used) {
        if (g_shm[shmid].nattach > 0)
            g_shm[shmid].nattach--;
        if (g_shm[shmid].marked_delete && g_shm[shmid].nattach == 0)
            sysv_shm_free_locked(shmid, &free_pages, &free_npages);
    }
    spin_unlock_irqrestore(&g_shm_lock, flags);
    sysv_shm_free_pages(free_pages, free_npages);
}

int sysv_shm_ref_attach(int shmid)
{
    uint64_t flags = spin_lock_irqsave(&g_shm_lock);
    if (shmid < 0 || shmid >= SYSV_SHM_MAX || !g_shm[shmid].used) {
        spin_unlock_irqrestore(&g_shm_lock, flags);
        return -EINVAL;
    }
    g_shm[shmid].nattach++;
    spin_unlock_irqrestore(&g_shm_lock, flags);
    return 0;
}

int sysv_shm_get(int key, size_t size, int shmflg)
{
    uint64_t flags = spin_lock_irqsave(&g_shm_lock);

    if (!(shmflg & IPC_CREAT)) {
        for (int i = 0; i < SYSV_SHM_MAX; i++) {
            if (g_shm[i].used && !g_shm[i].marked_delete && g_shm[i].key == key) {
                if (size > g_shm[i].size) {
                    spin_unlock_irqrestore(&g_shm_lock, flags);
                    return -EINVAL;
                }
                spin_unlock_irqrestore(&g_shm_lock, flags);
                return i;
            }
        }
        spin_unlock_irqrestore(&g_shm_lock, flags);
        return -ENOENT;
    }

    if (key != 0) {
        for (int i = 0; i < SYSV_SHM_MAX; i++) {
            if (g_shm[i].used && !g_shm[i].marked_delete && g_shm[i].key == key) {
                if (size > g_shm[i].size) {
                    spin_unlock_irqrestore(&g_shm_lock, flags);
                    return -EINVAL;
                }
                if (shmflg & IPC_EXCL) {
                    spin_unlock_irqrestore(&g_shm_lock, flags);
                    return -EEXIST;
                }
                spin_unlock_irqrestore(&g_shm_lock, flags);
                return i;
            }
        }
    }

    if (size == 0) {
        spin_unlock_irqrestore(&g_shm_lock, flags);
        return -EINVAL;
    }
    if (size > SHM_MAX_PAGES * PAGE_SIZE) {
        spin_unlock_irqrestore(&g_shm_lock, flags);
        return -ENOMEM;
    }
    size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > SHM_MAX_PAGES) {
        spin_unlock_irqrestore(&g_shm_lock, flags);
        return -ENOMEM;
    }

    int slot = -1;
    for (int i = 0; i < SYSV_SHM_MAX; i++) {
        if (!g_shm[i].used) {
            slot = i;
            break;
        }
    }
    spin_unlock_irqrestore(&g_shm_lock, flags);
    if (slot < 0)
        return -ENOSPC;

    pfn_t *pages = sysv_shm_alloc_pages(npages);
    if (!pages)
        return -ENOMEM;

    flags = spin_lock_irqsave(&g_shm_lock);
    if (key != 0) {
        for (int i = 0; i < SYSV_SHM_MAX; i++) {
            if (g_shm[i].used && !g_shm[i].marked_delete && g_shm[i].key == key) {
                size_t existing_size = g_shm[i].size;
                int existing_id = i;
                spin_unlock_irqrestore(&g_shm_lock, flags);
                sysv_shm_free_pages(pages, npages);
                if (size > existing_size)
                    return -EINVAL;
                return (shmflg & IPC_EXCL) ? -EEXIST : existing_id;
            }
        }
    }
    if (g_shm[slot].used) {
        for (slot = 0; slot < SYSV_SHM_MAX; slot++)
            if (!g_shm[slot].used)
                break;
        if (slot >= SYSV_SHM_MAX) {
            spin_unlock_irqrestore(&g_shm_lock, flags);
            sysv_shm_free_pages(pages, npages);
            return -ENOSPC;
        }
    }
    g_shm[slot].used = 1;
    g_shm[slot].marked_delete = 0;
    g_shm[slot].key = key;
    g_shm[slot].size = npages * PAGE_SIZE;
    g_shm[slot].pages = pages;
    g_shm[slot].npages = npages;
    g_shm[slot].nattach = 0;
    spin_unlock_irqrestore(&g_shm_lock, flags);
    return slot;
}

int sysv_shm_size(int shmid, size_t *size)
{
    if (!size) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_shm_lock);
    if (shmid < 0 || shmid >= SYSV_SHM_MAX || !g_shm[shmid].used) {
        spin_unlock_irqrestore(&g_shm_lock, flags);
        return -EINVAL;
    }
    *size = g_shm[shmid].size;
    spin_unlock_irqrestore(&g_shm_lock, flags);
    return 0;
}

uint64_t sysv_shm_at(int shmid, uint64_t shmaddr, int shmflg)
{
    (void)shmflg;

    task_t *t = proc_current();
    if (!t || !t->mm) return (uint64_t)-EINVAL;

    uint64_t lk = spin_lock_irqsave(&g_shm_lock);
    if (shmid < 0 || shmid >= SYSV_SHM_MAX ||
        !g_shm[shmid].used || g_shm[shmid].marked_delete) {
        spin_unlock_irqrestore(&g_shm_lock, lk);
        return (uint64_t)-EINVAL;
    }

    pfn_t *pages = g_shm[shmid].pages;
    size_t npages = g_shm[shmid].npages;
    size_t shm_size = g_shm[shmid].size;
    g_shm[shmid].nattach++;
    spin_unlock_irqrestore(&g_shm_lock, lk);

    uint64_t vm_flags = VM_SHARED | VM_READ | VM_WRITE | VM_SYSV_SHM;
    uint64_t flags = mm_vm_flags_to_pte_flags(vm_flags);

    uint64_t addr = shmaddr;
    if (addr == 0)
        addr = mm_find_gap(t->mm, MMAP_BASE_ADDR, shm_size);
    if (addr == 0) {
        sysv_shm_unref_attach(shmid);
        return (uint64_t)-ENOMEM;
    }
    if ((addr & (PAGE_SIZE - 1)) || sysv_shm_range_overlaps(t->mm, addr, shm_size)) {
        sysv_shm_unref_attach(shmid);
        return (uint64_t)-EINVAL;
    }

    vm_area_t *vma = kcalloc(1, sizeof(vm_area_t));
    if (!vma) {
        sysv_shm_unref_attach(shmid);
        return (uint64_t)-ENOMEM;
    }

    size_t mapped = 0;
    for (size_t p = 0; p < npages; p++) {
        paddr_t pa = pfn_to_phys(pages[p]);
        if (pt_map(t->mm->pgdir, addr + p * PAGE_SIZE, pa, flags) < 0) {
            sysv_shm_unmap_attached_pages(t->mm, addr, mapped);
            kfree(vma);
            sysv_shm_unref_attach(shmid);
            return (uint64_t)-ENOMEM;
        }
        frame_get(pages[p]);
        mapped++;
    }

    vma->start = addr;
    vma->end = addr + shm_size;
    vma->vm_flags = vm_flags;
    vma->pte_flags = flags;
    vma->file_fd = -1;
    vma->sysv_shmid = shmid;
    uint64_t mm_flags = spin_lock_irqsave(&t->mm->lock);
    mm_insert_vma(t->mm, vma);
    t->mm->total_vm += npages;
    spin_unlock_irqrestore(&t->mm->lock, mm_flags);
    mm_vma_flush_deferred(t->mm);

    return addr;
}

int sysv_shm_detach(const void *shmaddr)
{
    if (!shmaddr) return -EINVAL;
    task_t *t = proc_current();
    if (!t || !t->mm)
        return -EINVAL;

    uint64_t addr = (uint64_t)(uintptr_t)shmaddr;
    vm_area_t *vma = mm_find_vma(t->mm, addr);
    if (!vma || vma->start != addr || !(vma->vm_flags & VM_SYSV_SHM))
        return -EINVAL;
    size_t len = vma->end - vma->start;
    int shmid = vma->sysv_shmid;
    if (shmid < 0)
        return -EINVAL;

    int r = mm_munmap(t->mm, addr, len);
    return r;
}

int sysv_shm_control(int shmid, int cmd, void *buf)
{
    uint64_t flags = spin_lock_irqsave(&g_shm_lock);
    if (shmid < 0 || shmid >= SYSV_SHM_MAX || !g_shm[shmid].used) {
        spin_unlock_irqrestore(&g_shm_lock, flags);
        return -EINVAL;
    }

    cmd &= ~IPC_64_BIT;

    if (cmd == IPC_RMID) {
        pfn_t *pages = NULL;
        size_t npages = 0;
        if (g_shm[shmid].nattach > 0) {
            g_shm[shmid].marked_delete = 1;
            spin_unlock_irqrestore(&g_shm_lock, flags);
            return 0;
        }
        sysv_shm_free_locked(shmid, &pages, &npages);
        spin_unlock_irqrestore(&g_shm_lock, flags);
        sysv_shm_free_pages(pages, npages);
        return 0;
    }

    if ((cmd == IPC_STAT || cmd == SHM_STAT_ANY) && buf) {
        task_t *cur = proc_current();
        int pid = cur ? cur->pid : 0;
        int key = g_shm[shmid].key;
        size_t segsz = g_shm[shmid].size;
        unsigned long nattch = (unsigned long)g_shm[shmid].nattach;
        spin_unlock_irqrestore(&g_shm_lock, flags);
        struct {
            struct { int k; unsigned u,g,cu,cg; unsigned m; int s; long p1,p2; } perm;
            size_t segsz;
            long at, dt, ct;
            int cpid, lpid;
            unsigned long nattch, pad1, pad2;
        } ds;
        memset(&ds, 0, sizeof(ds));
        ds.perm.k = key;
        ds.perm.u = 0;
        ds.perm.g = 0;
        ds.perm.cu = 0;
        ds.perm.cg = 0;
        ds.perm.m = 0666;
        ds.perm.s = shmid;
        ds.segsz = segsz;
        ds.cpid = pid;
        ds.lpid = pid;
        ds.nattch = nattch;
        if (copy_to_user(buf, &ds, sizeof(ds)) < 0)
            return -EFAULT;
        return 0;
    }

    if (cmd == SHM_INFO && buf) {
        spin_unlock_irqrestore(&g_shm_lock, flags);
        char zero[64];
        memset(zero, 0, sizeof(zero));
        return copy_to_user(buf, zero, sizeof(zero)) < 0 ? -EFAULT : 0;
    }

    spin_unlock_irqrestore(&g_shm_lock, flags);
    return 0;
}
