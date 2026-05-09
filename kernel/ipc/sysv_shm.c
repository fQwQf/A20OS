#include "ipc/sysv_shm.h"

#include "core/consts.h"
#include "core/defs.h"
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

int sysv_shm_get(int key, size_t size, int shmflg)
{
    if (size == 0) return -EINVAL;

    if (!(shmflg & IPC_CREAT)) {
        for (int i = 0; i < SYSV_SHM_MAX; i++)
            if (g_shm[i].used && g_shm[i].key == key) return i;
        return -ENOENT;
    }

    if (key != 0) {
        for (int i = 0; i < SYSV_SHM_MAX; i++) {
            if (g_shm[i].used && g_shm[i].key == key) {
                if (shmflg & IPC_EXCL) return -EEXIST;
                return i;
            }
        }
    }

    size_t npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (npages > SHM_MAX_PAGES) return -ENOMEM;

    for (int i = 0; i < SYSV_SHM_MAX; i++) {
        if (!g_shm[i].used) {
            pfn_t *pages = kcalloc(npages, sizeof(pfn_t));
            if (!pages) return -ENOMEM;

            for (size_t p = 0; p < npages; p++) {
                pfn_t pfn = pfa_alloc_page();
                if (pfn == PFN_NONE) {
                    for (size_t j = 0; j < p; j++)
                        pfa_free_page(pages[j]);
                    kfree(pages);
                    return -ENOMEM;
                }
                pages[p] = pfn;
                void *va = pfn_to_virt(pfn);
                memset(va, 0, PAGE_SIZE);
            }

            g_shm[i].used = 1;
            g_shm[i].key = key;
            g_shm[i].size = npages * PAGE_SIZE;
            g_shm[i].pages = pages;
            g_shm[i].npages = npages;
            g_shm[i].nattach = 0;
            return i;
        }
    }
    return -ENOSPC;
}

int sysv_shm_size(int shmid, size_t *size)
{
    if (!size) return -EINVAL;
    if (shmid < 0 || shmid >= SYSV_SHM_MAX || !g_shm[shmid].used) return -EINVAL;
    *size = g_shm[shmid].size;
    return 0;
}

uint64_t sysv_shm_at(int shmid, uint64_t shmaddr, int shmflg)
{
    if (shmid < 0 || shmid >= SYSV_SHM_MAX || !g_shm[shmid].used)
        return (uint64_t)-EINVAL;

    sysv_shm_t *shm = &g_shm[shmid];
    task_t *t = proc_current();
    if (!t || !t->mm) return (uint64_t)-EINVAL;

    uint64_t flags = PTE_U | PTE_R | PTE_W;

    uint64_t addr = shmaddr;
    if (addr == 0)
        addr = mm_find_gap(t->mm, MMAP_BASE_ADDR, shm->size);
    if (addr == 0) return (uint64_t)-ENOMEM;

    for (size_t p = 0; p < shm->npages; p++) {
        paddr_t pa = pfn_to_phys(shm->pages[p]);
        if (pt_map(t->pgdir, addr + p * PAGE_SIZE, pa, flags) < 0)
            return (uint64_t)-ENOMEM;
        frame_get(shm->pages[p]);
    }

    vm_area_t *vma = kcalloc(1, sizeof(vm_area_t));
    if (!vma) return (uint64_t)-ENOMEM;
    vma->start = addr;
    vma->end = addr + shm->size;
    vma->vm_flags = VM_SHARED | VM_READ | VM_WRITE;
    vma->pte_flags = flags;
    vma->file_fd = -1;
    mm_insert_vma(t->mm, vma);
    t->mm->total_vm += shm->npages;

    shm->nattach++;
    return addr;
}

int sysv_shm_detach(const void *shmaddr)
{
    if (!shmaddr) return -EINVAL;
    return 0;
}

int sysv_shm_control(int shmid, int cmd, void *buf)
{
    if (shmid < 0 || shmid >= SYSV_SHM_MAX || !g_shm[shmid].used) return -EINVAL;

    cmd &= ~IPC_64_BIT;

    if (cmd == IPC_RMID) {
        if (g_shm[shmid].nattach > 0) return -EBUSY;
        for (size_t p = 0; p < g_shm[shmid].npages; p++)
            pfa_free_page(g_shm[shmid].pages[p]);
        kfree(g_shm[shmid].pages);
        memset(&g_shm[shmid], 0, sizeof(sysv_shm_t));
        return 0;
    }

    if ((cmd == IPC_STAT || cmd == SHM_STAT_ANY) && buf) {
        task_t *cur = proc_current();
        int pid = cur ? cur->pid : 0;
        struct {
            struct { int k; unsigned u,g,cu,cg; unsigned m; int s; long p1,p2; } perm;
            size_t segsz;
            long at, dt, ct;
            int cpid, lpid;
            unsigned long nattch, pad1, pad2;
        } ds;
        memset(&ds, 0, sizeof(ds));
        ds.perm.k = g_shm[shmid].key;
        ds.perm.u = 0;
        ds.perm.g = 0;
        ds.perm.cu = 0;
        ds.perm.cg = 0;
        ds.perm.m = 0666;
        ds.perm.s = shmid;
        ds.segsz = g_shm[shmid].size;
        ds.cpid = pid;
        ds.lpid = pid;
        ds.nattch = (unsigned long)g_shm[shmid].nattach;
        if (copy_to_user(buf, &ds, sizeof(ds)) < 0)
            return -EFAULT;
        return 0;
    }

    if (cmd == SHM_INFO && buf) {
        memset(buf, 0, 64);
        return 0;
    }

    return 0;
}
