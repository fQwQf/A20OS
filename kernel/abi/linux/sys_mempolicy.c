#include "syscall_impl.h"

#include "mm/mempolicy.h"
#include "proc/proc.h"

/*
 * NUMA memory policy syscalls.  A20OS has a single NUMA node, so policies
 * are validated and stored per-task (kernel/mm/mempolicy.c) but never move
 * physical pages.
 */

#define MPOL_DEFAULT    0
#define MPOL_PREFERRED  1
#define MPOL_BIND       2
#define MPOL_INTERLEAVE 3
#define MPOL_LOCAL      4

#define MPOL_MF_STRICT  1
#define MPOL_MF_MOVE    2
#define MPOL_MF_MOVE_ALL 4

#define MAX_NUMNODES_BITS 64

static uint64_t mempolicy_copy_nodemask(const unsigned long *nmask,
                                        unsigned long maxnode)
{
    (void)maxnode;
    if (!nmask)
        return 0;
    uint64_t bits = 0;
    if (copy_from_user(&bits, nmask, sizeof(bits)) < 0)
        return (uint64_t)-EFAULT;
    return bits;
}

int64_t sys_set_mempolicy(int mode, const unsigned long *nmask,
                          unsigned long maxnode)
{
    (void)maxnode;
    uint64_t bits = mempolicy_copy_nodemask(nmask, maxnode);
    if (bits == (uint64_t)-EFAULT)
        return -EFAULT;
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    return mempolicy_set(t, mode, bits);
}

int64_t sys_get_mempolicy(int *policy, unsigned long *nmask,
                          unsigned long maxnode, uint64_t addr,
                          unsigned long flags)
{
    (void)addr;
    (void)maxnode;
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    if (flags == 0) {
        if (policy) {
            int mode = mempolicy_get_mode(t);
            if (mode < 0)
                return mode;
            if (copy_to_user(policy, &mode, sizeof(mode)) < 0)
                return -EFAULT;
        }
        if (nmask) {
            uint64_t bits = 1; /* node 0 */
            if (copy_to_user(nmask, &bits, sizeof(bits)) < 0)
                return -EFAULT;
        }
        return 0;
    }
    /* MPOL_F_NODE / MPOL_F_ADDR queries report node 0. */
    if (policy) {
        int n = 0;
        if (copy_to_user(policy, &n, sizeof(n)) < 0)
            return -EFAULT;
    }
    return 0;
}

int64_t sys_mbind(uint64_t addr, unsigned long len, int mode,
                  const unsigned long *nmask, unsigned long maxnode,
                  unsigned flags)
{
    (void)maxnode;
    uint64_t bits = mempolicy_copy_nodemask(nmask, maxnode);
    if (bits == (uint64_t)-EFAULT)
        return -EFAULT;
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    return mempolicy_mbind(t, addr, (size_t)len, mode, bits, flags);
}

int64_t sys_migrate_pages(int pid, unsigned long maxnode,
                          const unsigned long *old_nodes,
                          const unsigned long *new_nodes)
{
    (void)pid;
    (void)maxnode;
    (void)old_nodes;
    (void)new_nodes;
    /* Single-node system: migrating between node 0 and node 0 is a no-op. */
    return 0;
}

int64_t sys_move_pages(int pid, unsigned long nr_pages, const void *pages,
                       const int *nodes, int *status, int flags)
{
    (void)pid;
    (void)nr_pages;
    (void)pages;
    (void)nodes;
    (void)flags;
    /* No page can move on a single-node system; report every page as already
     * on node 0 (status value 0 = MPOL_MF_MOVE success semantics). */
    if (status && nr_pages) {
        if (nr_pages > SIZE_MAX / sizeof(int))
            return -EINVAL;
        size_t st_bytes = nr_pages * sizeof(int);
        int *st = proc_scratch_buffer(st_bytes);
        if (!st)
            return -ENOMEM;
        for (unsigned long i = 0; i < nr_pages; i++)
            st[i] = 0;
        if (copy_to_user(status, st, st_bytes) < 0)
            return -EFAULT;
    }
    return 0;
}

int64_t sys_set_mempolicy_home_node(uint64_t addr, unsigned long len,
                                    int home_node, unsigned long flags)
{
    (void)addr;
    (void)len;
    (void)home_node;
    (void)flags;
    return 0;
}
