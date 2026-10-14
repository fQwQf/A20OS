#ifndef _FS_EVENTPOLL_H
#define _FS_EVENTPOLL_H

#include <stdint.h>
#include <stddef.h>

/*
 * Kernel-internal epoll instance engine (kernel/fs/eventpoll.c), with the
 * protected interest-table transitions optionally owned by
 * kernel/fs/eventpoll_state.rs. The adapter keeps vfile refs and waiting.
 *
 * The Linux ABI layer decodes the wire structs (arch-packed epoll_event)
 * onto these calls. Event bits match the Linux wire values because they are
 * stored and reported verbatim. A selector retains the C table transitions
 * as a differential control while the Rust state owner is being integrated.
 */

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLLIN     0x001
#define EPOLLOUT    0x004
#define EPOLLERR    0x008
#define EPOLLHUP    0x010
#define EPOLLRDHUP  0x2000
#define EPOLLEXCLUSIVE    (1U << 28)
#define EPOLLWAKEUP       (1U << 29)
#define EPOLLONESHOT      (1U << 30)
#define EPOLLET           (1U << 31)

struct task_t;
struct vfile;

struct eventpoll_event {
    uint32_t events;
    uint64_t data;
};

int eventpoll_create(int flags);
int eventpoll_ctl(int epfd, int op, int fd, uint32_t events, uint64_t data);
int eventpoll_wait(int epfd, struct eventpoll_event *out_events,
                   int maxevents, int timeout_ms,
                   const void *sigmask, size_t sigsetsize);
int eventpoll_slot_contains_file(struct task_t *owner, struct vfile *epoll_vf,
                                 uint64_t file_identity);
/* Called after the final descriptor slot has detached and all fdtable locks
 * are released. vf remains pinned for the duration of the callback. */
void eventpoll_file_final_slot_closed(struct vfile *vf);
/* Cheap exact-target check used by fdtable to skip global slot scans when no
 * epoll instance retains this VFile. */
int eventpoll_file_has_watch(struct vfile *vf);

#endif /* _FS_EVENTPOLL_H */
