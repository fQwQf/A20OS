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
    int key;
    size_t size;
    pfn_t *pages;
    size_t npages;
    int nattach;
} sysv_shm_t;

static sysv_shm_t g_shm[SYSV_SHM_MAX];
static spinlock_t g_shm_lock = SPINLOCK_INIT;

int sysv_shm_get(int key, size_t size, int shmflg)
{
    if (size == 0) return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&g_shm_lock);

    if (!(shmflg & IPC_CREAT)) {
        for (int i = 0; i < SYSV_SHM_MAX; i++)
            if (g_shm[i].used && g_shm[i].key == key) {
                spin_unlock_irqrestore(&g_shm_lock, flags);
                return i;
            }
        spin_unlock_irqrestore(&g_shm_lock, flags);
        return -ENOENT;
    }

    if (key != 0) {
        for (int i = 0; i < SYSV_SHM_MAX; i++) {
            if (g_shm[i].used && g_shm[i].key == key) {
                if (shmflg & IPC_EXCL) {
                    spin_unlock_irqrestore(&g_shm_lock, flags);
                    return -EEXIST;
                }
                spin_unlock_irqrestore(&g_shm_lock, flags);
                return i;
            }
        }
    }

    size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > SHM_MAX_PAGES) {
        spin_unlock_irqrestore(&g_shm_lock, flags);
        return -ENOMEM;
    }

    for (int i = 0; i < SYSV_SHM_MAX; i++) {
        if (!g_shm[i].used) {
            g_shm[i].used = 1;
            g_shm[i].key = key;
            g_shm[i].size = 0;
            g_shm[i].pages = NULL;
            g_shm[i].npages = npages;
            g_shm[i].nattach = 0;
            spin_unlock_irqrestore(&g_shm_lock, flags);

            pfn_t *pages = kcalloc(npages, sizeof(pfn_t));
            if (!pages) {
                g_shm[i].used = 0;
                return -ENOMEM;
            }

            for (size_t p = 0; p < npages; p++) {
                pfn_t pfn = pfa_alloc_page();
                if (pfn == PFN_NONE) {
                    for (size_t j = 0; j < p; j++)
                        pfa_free_page(pages[j]);
                    kfree(pages);
                    g_shm[i].used = 0;
                    return -ENOMEM;
                }
                pages[p] = pfn;
                void *va = pfn_to_virt(pfn);
                memset(va, 0, PAGE_SIZE);
            }

            flags = spin_lock_irqsave(&g_shm_lock);
            g_shm[i].size = npages * PAGE_SIZE;
            g_shm[i].pages = pages;
            spin_unlock_irqrestore(&g_shm_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&g_shm_lock, flags);
    return -ENOSPC;
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
    uint64_t lk = spin_lock_irqsave(&g_shm_lock);
    if (shmid < 0 || shmid >= SYSV_SHM_MAX || !g_shm[shmid].used) {
        spin_unlock_irqrestore(&g_shm_lock, lk);
        return (uint64_t)-EINVAL;
    }

    sysv_shm_t *shm = &g_shm[shmid];
    size_t npages = shm->npages;
    size_t shm_size = shm->size;
    shm->nattach++;
    spin_unlock_irqrestore(&g_shm_lock, lk);

    task_t *t = proc_current();
    if (!t || !t->mm) return (uint64_t)-EINVAL;

    uint64_t flags = PTE_U | PTE_R | PTE_W;

    uint64_t addr = shmaddr;
    if (addr == 0)
        addr = mm_find_gap(t->mm, MMAP_BASE_ADDR, shm_size);
    if (addr == 0) return (uint64_t)-ENOMEM;

    lk = spin_lock_irqsave(&g_shm_lock);
    for (size_t p = 0; p < npages; p++) {
        paddr_t pa = pfn_to_phys(shm->pages[p]);
        if (pt_map(t->pgdir, addr + p * PAGE_SIZE, pa, flags) < 0) {
            spin_unlock_irqrestore(&g_shm_lock, lk);
            return (uint64_t)-ENOMEM;
        }
        frame_get(shm->pages[p]);
    }
    spin_unlock_irqrestore(&g_shm_lock, lk);

    vm_area_t *vma = kcalloc(1, sizeof(vm_area_t));
    if (!vma) return (uint64_t)-ENOMEM;
    vma->start = addr;
    vma->end = addr + shm_size;
    vma->vm_flags = VM_SHARED | VM_READ | VM_WRITE;
    vma->pte_flags = flags;
    vma->file_fd = -1;
    mm_insert_vma(t->mm, vma);
    t->mm->total_vm += npages;

    return addr;
}

int sysv_shm_detach(const void *shmaddr)
{
    if (!shmaddr) return -EINVAL;
    return 0;
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
        if (g_shm[shmid].nattach > 0) {
            spin_unlock_irqrestore(&g_shm_lock, flags);
            return -EBUSY;
        }
        pfn_t *pages = g_shm[shmid].pages;
        size_t npages = g_shm[shmid].npages;
        memset(&g_shm[shmid], 0, sizeof(sysv_shm_t));
        spin_unlock_irqrestore(&g_shm_lock, flags);
        for (size_t p = 0; p < npages; p++)
            pfa_free_page(pages[p]);
        kfree(pages);
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
        memset(buf, 0, 64);
        return 0;
    }

    spin_unlock_irqrestore(&g_shm_lock, flags);
    return 0;
}
