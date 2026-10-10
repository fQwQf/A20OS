#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

/* Architecture-only syscall slots remain unavailable on other ports. */

__attribute__((weak)) int64_t sys_riscv_hwprobe(const void *pairs, size_t pair_count,
                          size_t cpu_size, const void *cpus, unsigned flags)
{
    (void)pairs;
    (void)pair_count;
    (void)cpu_size;
    (void)cpus;
    (void)flags;
    /* riscv_hwprobe is a RISC-V-only syscall; non-RISC-V platforms have no
     * RISC-V hardware to probe. */
    return -EOPNOTSUPP;
}

__attribute__((weak)) int64_t sys_riscv_flush_icache(uint64_t start, uint64_t end, uint64_t flags)
{
    (void)start;
    (void)end;
    (void)flags;
    /* riscv_flush_icache is a RISC-V-only syscall; the icache is managed by
     * the arch port on other platforms. */
    return -EOPNOTSUPP;
}
