#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#include "core/string.h"
#include "proc/proc.h"

/*
 * RISC-V architecture-specific syscalls (riscv_flush_icache, riscv_hwprobe).
 *
 * These entries are only meaningful on RISC-V64/RISC-V32.  On other arches
 * the syscall table still registers them (they never execute), so the
 * implementations are guarded at build time by the arch macros.
 */

#if defined(CONFIG_RISCV64) || defined(CONFIG_RISCV32)

#include "mm/mm.h"
#include "proc/proc.h"

/* riscv_hwprobe(2): fill an array of {key, value} pairs with the features the
 * kernel supports.  Unknown keys are answered with key = -1. */
#define RISCV_HWPROBE_KEY_MVENDORID      0
#define RISCV_HWPROBE_KEY_MARCHID        1
#define RISCV_HWPROBE_KEY_MIMPLID        2
#define RISCV_HWPROBE_KEY_BASE_BEHAVIOR  3
#define RISCV_HWPROBE_BASE_BEHAVIOR_IMA  1

struct riscv_hwprobe_kern {
    int64_t key;
    uint64_t value;
};

#define RISCV_HWPROBE_KEY_MASK 0xff
#define RISCV_HWPROBE_KEY_UNSUPPORTED -1

int64_t sys_riscv_hwprobe(const void *pairs, size_t pair_count,
                          size_t cpu_size, const void *cpus,
                          unsigned flags)
{
    (void)cpu_size;
    (void)cpus;
    (void)flags;
    if (!pairs)
        return -EFAULT;

    for (size_t i = 0; i < pair_count; i++) {
        struct riscv_hwprobe_kern pr;
        if (copy_from_user(&pr, (const char *)pairs + i * sizeof(pr),
                           sizeof(pr)) < 0)
            return -EFAULT;
        switch (pr.key & RISCV_HWPROBE_KEY_MASK) {
        case RISCV_HWPROBE_KEY_MVENDORID:
            pr.value = 0;
            break;
        case RISCV_HWPROBE_KEY_MARCHID:
            pr.value = 0;
            break;
        case RISCV_HWPROBE_KEY_MIMPLID:
            pr.value = 0;
            break;
        case RISCV_HWPROBE_KEY_BASE_BEHAVIOR:
            pr.value = RISCV_HWPROBE_BASE_BEHAVIOR_IMA;
            break;
        default:
            pr.key = RISCV_HWPROBE_KEY_UNSUPPORTED;
            pr.value = 0;
            break;
        }
        if (copy_to_user((char *)pairs + i * sizeof(pr), &pr, sizeof(pr)) < 0)
            return -EFAULT;
    }
    return 0;
}

/* riscv_flush_icache(2): RISC-V programs call this after self-modifying code.
 * A20OS flushes the icache for the affected range through the arch helper. */
int64_t sys_riscv_flush_icache(uint64_t start, uint64_t end, uint64_t flags)
{
    (void)flags;
    if (end < start)
        return -EINVAL;
    /* The caller maps the range in user space; flush through the current mm
     * page table.  We use the generic range flush on the direct map of the
     * physical backing. */
    arch_flush_icache_range((void *)(uintptr_t)start,
                            (size_t)(end - start));
    return 0;
}

#else

int64_t sys_riscv_hwprobe(const void *pairs, size_t pair_count,
                          size_t cpu_size, const void *cpus, unsigned flags)
{
    (void)pairs;
    (void)pair_count;
    (void)cpu_size;
    (void)cpus;
    (void)flags;
    return -ENOSYS;
}

int64_t sys_riscv_flush_icache(uint64_t start, uint64_t end, uint64_t flags)
{
    (void)start;
    (void)end;
    (void)flags;
    return -ENOSYS;
}

#endif
