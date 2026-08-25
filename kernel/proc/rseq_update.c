#include "proc/rseq.h"

#include "mm/mm.h"
#include "mm/vm.h"

/* struct rseq u32 field offsets (include/uapi/linux/rseq.h): the area is
 * 32-byte aligned by ABI contract so these fields never straddle pages. */
#define RSEQ_CPU_ID_START 0u
#define RSEQ_CPU_ID       1u
#define RSEQ_NODE_ID      5u
#define RSEQ_MM_CID       6u

static void rseq_store_u32(task_t *t, size_t off_words, uint32_t v)
{
    void *kaddr = NULL;
    size_t avail = 0;
    if (!t->mm || !t->rseq_area)
        return;
    if (mm_query_leaf_kaddr(t->mm->pgdir, t->rseq_area + off_words * 4,
                            &kaddr, &avail) != 0)
        return;
    if (!kaddr || avail < sizeof(uint32_t))
        return;
    *(volatile uint32_t *)kaddr = v;
}

void rseq_publish(task_t *t)
{
    if (!t || !t->rseq_area || !t->mm)
        return;
    rseq_store_u32(t, RSEQ_CPU_ID_START, t->cpu_id);
    rseq_store_u32(t, RSEQ_CPU_ID, t->cpu_id);
    rseq_store_u32(t, RSEQ_NODE_ID, 0);
    rseq_store_u32(t, RSEQ_MM_CID, t->cpu_id);
}
