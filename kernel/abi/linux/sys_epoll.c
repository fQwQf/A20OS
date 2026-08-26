#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "fs/eventpoll.h"
#include "sys/usercopy.h"

/*
 * A20OS Linux ABI epoll — wire translation only.
 *
 * The epoll instance engine (interest-list state machine, cycle detection,
 * wait protocol) lives in kernel/fs/eventpoll.c.  This file decodes the
 * arch-packed epoll_event wire structs and the syscall surface onto it.
 */

#if defined(CONFIG_X86_64)
#define EPOLL_EVENT_PACKED __attribute__((packed))
#else
#define EPOLL_EVENT_PACKED
#endif

typedef struct {
    uint32_t events;
    uint64_t data;
} EPOLL_EVENT_PACKED wire_epoll_event_t;

int64_t sys_epoll_create1(int flags)
{
    return eventpoll_create(flags);
}

int64_t sys_epoll_create(int size)
{
    if (size <= 0) return -EINVAL;
    return eventpoll_create(0);
}

int64_t sys_epoll_ctl(int epfd, int op, int fd, void *event)
{
    uint32_t events = 0;
    uint64_t data = 0;

    if (op != EPOLL_CTL_DEL) {
        if (!event) return -EFAULT;
        wire_epoll_event_t ev;
        memset(&ev, 0, sizeof(ev));
        if (copy_from_user(&ev, event, sizeof(ev)) < 0)
            return -EFAULT;
        events = ev.events;
        data = ev.data;
    }
    return eventpoll_ctl(epfd, op, fd, events, data);
}

static int64_t epoll_wait_common(int epfd, void *events, int maxevents,
                                 int timeout_ms, const void *sigmask,
                                 size_t sigsetsize)
{
    if (!events || maxevents <= 0 || maxevents > 1024)
        return -EINVAL;

    struct eventpoll_event *out =
        proc_scratch_buffer((size_t)maxevents * sizeof(*out));
    if (!out)
        return -ENOMEM;

    int n = eventpoll_wait(epfd, out, maxevents, timeout_ms,
                           sigmask, sigsetsize);
    if (n <= 0)
        return n;

    for (int i = 0; i < n; i++) {
        wire_epoll_event_t ev = { .events = out[i].events,
                                  .data = out[i].data };
        if (copy_to_user((char *)events + (size_t)i * sizeof(ev),
                         &ev, sizeof(ev)) < 0)
            return -EFAULT;
    }
    return n;
}

int64_t sys_epoll_wait(int epfd, void *events, int maxevents, int timeout)
{
    return epoll_wait_common(epfd, events, maxevents, timeout, NULL, 0);
}

int64_t sys_epoll_pwait(int epfd, void *events, int maxevents,
                        int timeout, const void *sigmask, size_t sigsetsize)
{
    return epoll_wait_common(epfd, events, maxevents, timeout,
                             sigmask, sigsetsize);
}

struct linux_timespec64_epoll {
    int64_t tv_sec;
    int64_t tv_nsec;
};

int64_t sys_epoll_pwait2(int epfd, void *events, int maxevents,
                         const void *timeout, const void *sigmask,
                         size_t sigsetsize)
{
    int timeout_ms = -1;
    if (timeout) {
        struct linux_timespec64_epoll ts;
        if (copy_from_user(&ts, timeout, sizeof(ts)) < 0)
            return -EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL)
            return -EINVAL;
        uint64_t total_ns = (uint64_t)ts.tv_sec * 1000000000ULL +
                            (uint64_t)ts.tv_nsec;
        uint64_t ms = total_ns / 1000000ULL;
        timeout_ms = ms > (uint64_t)0x7fffffff ? 0x7fffffff : (int)ms;
    }
    return epoll_wait_common(epfd, events, maxevents, timeout_ms,
                             sigmask, sigsetsize);
}
