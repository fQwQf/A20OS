#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "core/perf_event.h"
#include "sys/usercopy.h"

/*
 * A20OS Linux ABI perf_event_open(2) — wire translation only.
 *
 * Software event objects live in kernel/core/perf_event.c.  This file
 * decodes struct perf_event_attr (uapi/linux/perf_event.h layout), applies
 * the Linux surface rules and hands the resolved request to the core.
 *
 * Hardware/PMC events (PERF_TYPE_HARDWARE, HW_CACHE, RAW, BREAKPOINT, ...)
 * are refused with -EINVAL because the kernel has no PMU driver; that is
 * the honest boundary rather than faking counts.  The mmap() ring buffer
 * is not provided (read(2) is the counter interface); group leader
 * semantics are not supported (group_fd must be -1).
 */

#define PERF_TYPE_HARDWARE 0
#define PERF_TYPE_SOFTWARE 1
#define PERF_TYPE_TRACEPOINT 2
#define PERF_TYPE_HW_CACHE 3
#define PERF_TYPE_RAW 4
#define PERF_TYPE_BREAKPOINT 5

#define PERF_FLAG_FD_NO_GROUP  (1UL << 0)
#define PERF_FLAG_FD_OUTPUT    (1UL << 1)
#define PERF_FLAG_PID_CGROUP   (1UL << 2)
#define PERF_FLAG_FD_CLOEXEC   (1UL << 3)

/* Wire layout of struct perf_event_attr (uapi/linux/perf_event.h).  The
 * bitfield block at offset 40 is read as a raw u64. */
typedef struct perf_event_attr_kern {
    uint32_t type;
    uint32_t size;
    uint64_t config;
    uint64_t sample_period;
    uint64_t sample_type;
    uint64_t read_format;
    uint64_t flags;
    uint32_t wakeup_events;
    uint32_t bp_type;
    uint64_t config1;
    uint64_t config2;
    uint64_t branch_sample_type;
    uint64_t sample_regs_user;
    uint32_t sample_stack_user;
    int32_t  clockid;
    uint64_t sample_regs_intr;
    uint32_t aux_watermark;
    uint16_t sample_max_stack;
    uint16_t reserved2;
    uint32_t aux_sample_size;
    uint32_t reserved3;
    uint64_t sig_data;
} perf_event_attr_kern_t;

/* Bitfield bits (LSB-first, matches the uapi bitfield order). */
#define PERF_ATTR_DISABLED      (1ULL << 0)

int64_t sys_perf_event_open(const void *attr, int pid, int cpu,
                            int group_fd, unsigned long flags)
{
    if (!attr)
        return -EFAULT;
    if (flags & ~(PERF_FLAG_FD_NO_GROUP | PERF_FLAG_FD_CLOEXEC))
        return -EINVAL;
    if (group_fd >= 0)
        return -EINVAL; /* group leaders not supported */

    perf_event_attr_kern_t a;
    memset(&a, 0, sizeof(a));
    if (copy_from_user(&a, attr, sizeof(uint32_t) * 2) < 0)
        return -EFAULT;
    if (a.size < 64 || a.size > sizeof(a))
        return -EINVAL;
    if (copy_from_user(&a, attr, a.size) < 0)
        return -EFAULT;

    if (a.type != PERF_TYPE_SOFTWARE)
        return -EINVAL; /* no PMU driver: hardware/raw/breakpoint events */
    if (!perf_event_config_supported(a.config))
        return -EINVAL;
    if (a.read_format & ~(PERF_FORMAT_TOTAL_TIME_ENABLED |
                          PERF_FORMAT_TOTAL_TIME_RUNNING |
                          PERF_FORMAT_ID | PERF_FORMAT_LOST))
        return -EINVAL;
    if (pid < -1)
        return -EINVAL;

    /* Linux: pid==0 means the calling task, pid==-1 means system-wide. */
    if (pid == 0) {
        task_t *cur = proc_current();
        pid = cur ? cur->pid : -1;
    }

    return perf_event_create_fd(a.config, a.read_format, pid, cpu,
                                a.sample_period,
                                (a.flags & PERF_ATTR_DISABLED) ? 1 : 0,
                                (flags & PERF_FLAG_FD_CLOEXEC) ? 1 : 0);
}
