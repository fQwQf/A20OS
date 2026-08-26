#include "core/perf_event.h"

#include "core/errno.h"
#include "core/fcntl.h"
#include "core/lock.h"
#include "core/perf.h"
#include "core/poll.h"
#include "core/refcount.h"
#include "core/string.h"
#include "core/timer.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

/*
 * A20OS software perf-event objects — ABI-agnostic core module.
 *
 * The Linux perf_event_open(2) surface decodes its wire attributes onto
 * perf_event_create_fd()/the returned event fd below.
 *
 * PERF_TYPE_SOFTWARE events are sourced from per-task counters maintained by
 * the fault handler and scheduler plus monotonic time.  Hardware/PMC events
 * (PERF_TYPE_HARDWARE, HW_CACHE, RAW, BREAKPOINT, ...) are refused with
 * -EINVAL because the kernel has no PMU driver; that is the honest boundary
 * rather than faking counts.  The mmap() ring buffer is not provided
 * (read(2) is the counter interface); group leader semantics are not
 * supported (group_fd must be -1).
 *
 * read_format supports PERF_FORMAT_TOTAL_TIME_ENABLED/RUNNING/ID/LOST.
 * time values are reported in nanoseconds.  TASK_CLOCK and the page-fault
 * events freeze at their last observed value once the target task exits.
 */

typedef struct a20_perf_event {
    spinlock_t lock;
    refcount_t refcount;
    uint64_t config;
    uint64_t read_format;
    uint64_t id;
    int pid;              /* target pid, -1 = system-wide */
    int cpu;
    int disabled;
    uint64_t sample_period;
    uint64_t base_count;
    uint64_t base_enabled;
    uint64_t base_running;
    uint64_t enabled_start;
    uint64_t count_start;
} a20_perf_event_t;

static uint64_t g_perf_next_id = 1;

static uint64_t perf_source_value(a20_perf_event_t *ev, int *alive)
{
    *alive = 1;
    switch (ev->config) {
    case PERF_COUNT_SW_CPU_CLOCK:
    case PERF_COUNT_SW_TASK_CLOCK:
        if (ev->config == PERF_COUNT_SW_TASK_CLOCK && ev->pid >= 0) {
            task_t *t = proc_find_get(ev->pid);
            if (!t) {
                *alive = 0;
                return 0;
            }
            uint64_t v = t->total_time;
            proc_put(t);
            return v;
        }
        return timer_get_ticks();
    case PERF_COUNT_SW_PAGE_FAULTS:
        if (ev->pid >= 0) {
            task_t *t = proc_find_get(ev->pid);
            if (!t) {
                *alive = 0;
                return 0;
            }
            uint64_t v = __atomic_load_n(&t->perf_page_faults,
                                         __ATOMIC_RELAXED);
            proc_put(t);
            return v;
        }
        return __atomic_load_n(&g_perf_sw_page_faults, __ATOMIC_RELAXED);
    case PERF_COUNT_SW_PAGE_FAULTS_MAJ:
        if (ev->pid >= 0) {
            task_t *t = proc_find_get(ev->pid);
            if (!t) {
                *alive = 0;
                return 0;
            }
            uint64_t v = __atomic_load_n(&t->perf_page_faults_maj,
                                         __ATOMIC_RELAXED);
            proc_put(t);
            return v;
        }
        return __atomic_load_n(&g_perf_sw_page_faults_maj, __ATOMIC_RELAXED);
    case PERF_COUNT_SW_PAGE_FAULTS_MIN:
        if (ev->pid >= 0) {
            task_t *t = proc_find_get(ev->pid);
            if (!t) {
                *alive = 0;
                return 0;
            }
            uint64_t tot = __atomic_load_n(&t->perf_page_faults,
                                           __ATOMIC_RELAXED);
            uint64_t maj = __atomic_load_n(&t->perf_page_faults_maj,
                                           __ATOMIC_RELAXED);
            proc_put(t);
            return tot - maj;
        }
        return __atomic_load_n(&g_perf_sw_page_faults, __ATOMIC_RELAXED) -
               __atomic_load_n(&g_perf_sw_page_faults_maj, __ATOMIC_RELAXED);
    case PERF_COUNT_SW_CONTEXT_SWITCHES:
        if (ev->pid >= 0) {
            task_t *t = proc_find_get(ev->pid);
            if (!t) {
                *alive = 0;
                return 0;
            }
            uint64_t v = __atomic_load_n(&t->perf_switches, __ATOMIC_RELAXED);
            proc_put(t);
            return v;
        }
        return __atomic_load_n(&g_perf_sw_context_switches, __ATOMIC_RELAXED);
    default:
        /* CPU_MIGRATIONS, ALIGNMENT/EMULATION_FAULTS, DUMMY, BPF_OUTPUT,
         * CGROUP_SWITCHES: no backing hardware/software source, report 0. */
        return 0;
    }
}

static uint64_t perf_ticks_to_ns(uint64_t ticks)
{
    return ticks * 1000000000ULL / TICKS_PER_SEC;
}

static void perf_event_snapshot(a20_perf_event_t *ev, uint64_t *count_out,
                                uint64_t *enabled_ns, uint64_t *running_ns)
{
    uint64_t now = timer_get_ticks();
    uint64_t count = ev->base_count;
    uint64_t enabled = ev->base_enabled;
    uint64_t running = ev->base_running;

    if (!ev->disabled) {
        uint64_t dt = now - ev->enabled_start;
        enabled += dt;
        switch (ev->config) {
        case PERF_COUNT_SW_CPU_CLOCK:
            count += dt;
            running += dt;
            break;
        case PERF_COUNT_SW_TASK_CLOCK: {
            int alive;
            uint64_t src = perf_source_value(ev, &alive);
            uint64_t delta =
                (alive && src >= ev->count_start) ? src - ev->count_start : 0;
            count += delta;
            running += delta;
            break;
        }
        default: {
            int alive;
            uint64_t src = perf_source_value(ev, &alive);
            uint64_t delta =
                (alive && src >= ev->count_start) ? src - ev->count_start : 0;
            count += delta;
            running += dt;
            break;
        }
        }
    }
    *count_out = count;
    *enabled_ns = perf_ticks_to_ns(enabled);
    *running_ns = perf_ticks_to_ns(running);
}

static void perf_event_enable(a20_perf_event_t *ev)
{
    if (!ev->disabled)
        return;
    ev->disabled = 0;
    ev->enabled_start = timer_get_ticks();
    int alive;
    ev->count_start = perf_source_value(ev, &alive);
}

static void perf_event_disable(a20_perf_event_t *ev)
{
    if (ev->disabled)
        return;
    uint64_t count, enabled, running;
    perf_event_snapshot(ev, &count, &enabled, &running);
    ev->base_count = count;
    ev->base_enabled = enabled;
    ev->base_running = running;
    ev->disabled = 1;
}

static void perf_event_reset(a20_perf_event_t *ev)
{
    ev->base_count = 0;
    ev->base_enabled = 0;
    ev->base_running = 0;
    if (!ev->disabled) {
        ev->enabled_start = timer_get_ticks();
        int alive;
        ev->count_start = perf_source_value(ev, &alive);
    }
}

static int perf_event_read(vfile_t *vf, char *buf, size_t count)
{
    a20_perf_event_t *ev = vf ? vf->priv : NULL;
    if (!ev)
        return -EBADF;

    uint64_t vals[5];
    unsigned n = 0;
    uint64_t c, en, ru;
    perf_event_snapshot(ev, &c, &en, &ru);
    vals[n++] = c;
    if (ev->read_format & PERF_FORMAT_TOTAL_TIME_ENABLED)
        vals[n++] = en;
    if (ev->read_format & PERF_FORMAT_TOTAL_TIME_RUNNING)
        vals[n++] = ru;
    if (ev->read_format & PERF_FORMAT_ID)
        vals[n++] = ev->id;
    if (ev->read_format & PERF_FORMAT_LOST)
        vals[n++] = 0;
    if (count < n * sizeof(uint64_t))
        return -EINVAL;
    /* The read op receives a kernel-mapped buffer (read_into_user). */
    memcpy(buf, vals, n * sizeof(uint64_t));
    return (int)(n * sizeof(uint64_t));
}

static int perf_event_poll(vfile_t *vf, short events)
{
    (void)vf;
    short revents = 0;
    /* read(2) never blocks for a perf event (the counter is always
     * readable), so POLLIN is always ready. */
    if (events & POLLIN)
        revents |= POLLIN;
    if (events & POLLOUT)
        revents |= POLLOUT;
    return revents;
}

static int perf_event_close(vfile_t *vf)
{
    a20_perf_event_t *ev = vf ? vf->priv : NULL;
    if (ev) {
        vf->priv = NULL;
        if (refcount_dec_and_test(&ev->refcount))
            kfree(ev);
    }
    return 0;
}

static int perf_event_ioctl(vfile_t *vf, unsigned long req, void *arg)
{
    a20_perf_event_t *ev = vf ? vf->priv : NULL;
    if (!ev)
        return -EBADF;

    switch (req & 0xffffffffUL) {
    case PERF_EVENT_IOC_ENABLE:
        perf_event_enable(ev);
        return 0;
    case PERF_EVENT_IOC_DISABLE:
        perf_event_disable(ev);
        return 0;
    case PERF_EVENT_IOC_REFRESH: {
        int val = 0;
        if (arg && copy_from_user(&val, arg, sizeof(val)) < 0)
            return -EFAULT;
        perf_event_enable(ev);
        (void)val;
        return 0;
    }
    case PERF_EVENT_IOC_RESET:
        perf_event_reset(ev);
        return 0;
    case PERF_EVENT_IOC_PERIOD: {
        uint64_t period = 0;
        if (copy_from_user(&period, arg, sizeof(period)) < 0)
            return -EFAULT;
        ev->sample_period = period;
        return 0;
    }
    case PERF_EVENT_IOC_SET_OUTPUT:
        /* No mmap ring buffer; the output fd is accepted and ignored. */
        return 0;
    case PERF_EVENT_IOC_ID: {
        if (!arg)
            return -EFAULT;
        return copy_to_user(arg, &ev->id, sizeof(ev->id)) < 0 ? -EFAULT : 0;
    }
    default:
        return -EINVAL;
    }
}

static vfile_ops_t g_perf_event_ops = {
    .read = perf_event_read,
    .poll = perf_event_poll,
    .ioctl = perf_event_ioctl,
    .close = perf_event_close,
};

int perf_event_config_supported(uint64_t config)
{
    switch (config) {
    case PERF_COUNT_SW_CPU_CLOCK:
    case PERF_COUNT_SW_TASK_CLOCK:
    case PERF_COUNT_SW_PAGE_FAULTS:
    case PERF_COUNT_SW_CONTEXT_SWITCHES:
    case PERF_COUNT_SW_CPU_MIGRATIONS:
    case PERF_COUNT_SW_PAGE_FAULTS_MIN:
    case PERF_COUNT_SW_PAGE_FAULTS_MAJ:
    case PERF_COUNT_SW_ALIGNMENT_FAULTS:
    case PERF_COUNT_SW_EMULATION_FAULTS:
    case PERF_COUNT_SW_DUMMY:
    case PERF_COUNT_SW_BPF_OUTPUT:
    case PERF_COUNT_SW_CGROUP_SWITCHES:
        return 1;
    default:
        return 0;
    }
}

int perf_event_create_fd(uint64_t config, uint64_t read_format, int pid,
                         int cpu, uint64_t sample_period, int disabled,
                         int cloexec)
{
    a20_perf_event_t *ev = kmalloc(sizeof(*ev));
    if (!ev)
        return -ENOMEM;
    memset(ev, 0, sizeof(*ev));
    spin_init(&ev->lock);
    refcount_set(&ev->refcount, 1);
    ev->config = config;
    ev->read_format = read_format;
    ev->pid = pid;
    ev->cpu = cpu;
    ev->sample_period = sample_period;
    ev->id = __atomic_fetch_add(&g_perf_next_id, 1, __ATOMIC_RELAXED);
    if (ev->id == 0)
        ev->id = __atomic_fetch_add(&g_perf_next_id, 1, __ATOMIC_RELAXED);

    ev->disabled = disabled ? 1 : 0;
    if (!ev->disabled) {
        ev->enabled_start = timer_get_ticks();
        int alive;
        ev->count_start = perf_source_value(ev, &alive);
    }

    vfile_t *vf = vfile_alloc();
    if (!vf) {
        kfree(ev);
        return -ENOMEM;
    }
    vf->flags = O_RDWR;
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_perf_event_ops;
    vf->priv = ev;
    return anonfd_install_vfile(vf, cloexec ? O_CLOEXEC : 0);
}
