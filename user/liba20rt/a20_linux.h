/*
 * A20 Linux personality facade (docs/hybrid-kernel/05-idl-and-personality.md).
 *
 * Maps Linux-style programming constructs onto Native A20 primitives:
 *   fd table  -> handle table slots (path_open/handle_io/dup/close)
 *   mmap      -> VMO backed mappings (vm_alloc/vm_unmap)
 *   pipe      -> Native channel pair (see a20_personality.h)
 *   socketpair-> Native net socketpair
 *   futex     -> Native futex wait/wake
 *   epoll     -> EventQ-based wait-many
 *
 * This is the second brick of the Linux personality: the object
 * translation layer. Byte-stream semantics for pipe reads are already
 * provided by a20_personality.h.
 */
#ifndef _A20_LINUX_H
#define _A20_LINUX_H

#include "a20_fs.h"
#include "a20_handle.h"
#include "a20_mem.h"
#include "a20_net.h"
#include "a20_sync.h"
#include "a20_event.h"
#include "a20_personality.h"

#define A20_LINUX_FD_MAX 64
#define A20_LINUX_FD_NULL (-1)

typedef struct a20_linux_fdtable {
    a20_handle_t slots[A20_LINUX_FD_MAX];
    uint8_t      used[A20_LINUX_FD_MAX];
    int          next;
} a20_linux_fdtable_t;

static inline void a20_linux_fdtable_init(a20_linux_fdtable_t *t)
{
    if (!t)
        return;
    for (int i = 0; i < A20_LINUX_FD_MAX; i++) {
        t->slots[i] = A20_HANDLE_NULL;
        t->used[i] = 0;
    }
    t->next = 0;
}

static inline int a20_linux_fd_alloc(a20_linux_fdtable_t *t, a20_handle_t h)
{
    if (!t || h == A20_HANDLE_NULL)
        return A20_LINUX_FD_NULL;
    for (int i = 0; i < A20_LINUX_FD_MAX; i++) {
        if (!t->used[i]) {
            t->slots[i] = h;
            t->used[i] = 1;
            return i;
        }
    }
    return A20_LINUX_FD_NULL;
}

static inline a20_handle_t a20_linux_fd_handle(const a20_linux_fdtable_t *t,
                                               int fd)
{
    if (!t || fd < 0 || fd >= A20_LINUX_FD_MAX || !t->used[fd])
        return A20_HANDLE_NULL;
    return t->slots[fd];
}

/* open(2)-ish: resolves @path and registers the handle as an fd. */
static inline int a20_linux_open(a20_linux_fdtable_t *t, const char *path,
                                 uint32_t flags, a20_rights_t rights)
{
    a20_path_open_args_t args;
    a20_memset(&args, 0, sizeof(args));
    args.size = sizeof(args);
    args.version = 1;
    args.dir = A20_HANDLE_NULL;
    args.path = (uint64_t)path;
    args.path_len = (uint32_t)a20_strlen(path);
    args.rights = rights;
    args.flags = flags;
    args.mode = 0644;
    args.out_handle = A20_HANDLE_NULL;
    if (a20_status_is_err(a20_path_open(&args)))
        return A20_LINUX_FD_NULL;
    return a20_linux_fd_alloc(t, args.out_handle);
}

static inline int a20_linux_close(a20_linux_fdtable_t *t, int fd)
{
    a20_handle_t h = a20_linux_fd_handle(t, fd);
    if (h == A20_HANDLE_NULL)
        return -1;
    a20_hdl_close(h);
    t->slots[fd] = A20_HANDLE_NULL;
    t->used[fd] = 0;
    return 0;
}

/* dup(2): duplicate the handle into a fresh slot (same rights). */
static inline int a20_linux_dup(a20_linux_fdtable_t *t, int fd)
{
    a20_handle_t h = a20_linux_fd_handle(t, fd);
    if (h == A20_HANDLE_NULL)
        return A20_LINUX_FD_NULL;
    a20_handle_info_t info;
    a20_memset(&info, 0, sizeof(info));
    if (a20_status_is_err(a20_hdl_query(h, &info)))
        return A20_LINUX_FD_NULL;
    a20_handle_dup_args_t args;
    a20_memset(&args, 0, sizeof(args));
    args.size = sizeof(args);
    args.version = 1;
    args.source = h;
    args.rights_mask = info.rights;
    args.out_handle = A20_HANDLE_NULL;
    if (a20_status_is_err(a20_hdl_dup(&args)))
        return A20_LINUX_FD_NULL;
    return a20_linux_fd_alloc(t, args.out_handle);
}

static inline int64_t a20_linux_read(const a20_linux_fdtable_t *t, int fd,
                                     void *buf, uint64_t len)
{
    a20_handle_t h = a20_linux_fd_handle(t, fd);
    if (h == A20_HANDLE_NULL)
        return -1;
    uint64_t got = 0;
    a20_status_t r = a20_hdl_read_buf(h, buf, len, &got);
    if (a20_status_is_err(r))
        return -1;
    return (int64_t)got;
}

static inline int64_t a20_linux_write(const a20_linux_fdtable_t *t, int fd,
                                      const void *buf, uint64_t len)
{
    a20_handle_t h = a20_linux_fd_handle(t, fd);
    if (h == A20_HANDLE_NULL)
        return -1;
    uint64_t wrote = 0;
    a20_status_t r = a20_hdl_write_buf(h, buf, len, &wrote);
    if (a20_status_is_err(r))
        return -1;
    return (int64_t)wrote;
}

/* mmap(2)-ish anonymous mapping: returns VA or 0. */
static inline uint64_t a20_linux_mmap(uint64_t size, uint32_t prot)
{
    uint64_t addr = 0;
    if (a20_status_is_err(a20_vm_alloc_pages(
            (size + 4095) / 4096, prot, &addr)))
        return 0;
    return addr;
}

static inline int a20_linux_munmap(uint64_t addr, uint64_t len)
{
    return a20_status_is_err(a20_vm_unmap(addr, len)) ? -1 : 0;
}

/* socketpair(2): both endpoints land in the fd table. */
static inline int a20_linux_socketpair(a20_linux_fdtable_t *t, int domain,
                                       int type, int protocol, int fds[2])
{
    a20_handle_t socks[2] = { A20_HANDLE_NULL, A20_HANDLE_NULL };
    if (a20_status_is_err(a20_net_socketpair(domain, type, protocol, socks)))
        return -1;
    fds[0] = a20_linux_fd_alloc(t, socks[0]);
    fds[1] = a20_linux_fd_alloc(t, socks[1]);
    if (fds[0] < 0 || fds[1] < 0) {
        if (fds[0] >= 0)
            a20_linux_close(t, fds[0]);
        if (fds[1] >= 0)
            a20_linux_close(t, fds[1]);
        return -1;
    }
    return 0;
}

/* pipe(2): personality pipe registered into the fd table. */
static inline int a20_linux_pipe(a20_linux_fdtable_t *t, int fds[2])
{
    a20_personality_pipe_t pipe;
    if (a20_status_is_err(a20_personality_pipe_create(&pipe)))
        return -1;
    fds[0] = a20_linux_fd_alloc(t, pipe.read_end);
    fds[1] = a20_linux_fd_alloc(t, pipe.write_end);
    if (fds[0] < 0 || fds[1] < 0) {
        a20_personality_pipe_close(&pipe);
        return -1;
    }
    return 0;
}

/* epoll-ish wait-many: watches @handles via one EventQ and returns the
 * number of fired events in @events (max @max_events). */
typedef struct a20_linux_epoll_event {
    a20_handle_t handle;
    uint64_t     events;
    uint64_t     user_data;
} a20_linux_epoll_event_t;

static inline int a20_linux_epoll_wait(const a20_handle_t *handles,
                                       uint32_t nhandles,
                                       a20_linux_epoll_event_t *events,
                                       uint32_t max_events,
                                       a20_time_t timeout)
{
    if (!handles || !events || nhandles == 0 || max_events == 0)
        return -1;
    a20_handle_t eq;
    if (a20_status_is_err(a20_event_queue_create(&eq)))
        return -1;
    for (uint32_t i = 0; i < nhandles; i++) {
        if (a20_status_is_err(a20_event_watch(
                eq, handles[i],
                A20_EVENT_MASK(A20_EVENT_MESSAGE_READY) |
                A20_EVENT_MASK(A20_EVENT_READABLE) |
                A20_EVENT_MASK(A20_EVENT_CLOSED) |
                A20_EVENT_MASK(A20_EVENT_PEER_CLOSED),
                (uint64_t)handles[i])))
            goto fail;
    }
    a20_event_t ev;
    a20_status_t r = a20_event_wait(eq, timeout, &ev);
    if (r < 0) {
        a20_hdl_close(eq);
        return (r == -A20_ERR_TIMED_OUT) ? 0 : -1;
    }
    if (r >= 1) {
        events[0].handle = ev.source;
        events[0].events = ev.events;
        events[0].user_data = ev.user_data;
        a20_hdl_close(eq);
        return 1;
    }
fail:
    a20_hdl_close(eq);
    return -1;
}

/* futex(2)-ish: wait until *addr != expected or timeout. */
static inline int a20_linux_futex_wait(uint32_t *addr, uint32_t expected,
                                       uint64_t timeout_ns)
{
    a20_status_t r = a20_futex_wait(addr, expected, timeout_ns);
    if (r == A20_OK)
        return 0;
    if (r == -A20_ERR_WOULD_BLOCK)
        return 0; /* value already changed: caller must re-check */
    if (r == -A20_ERR_TIMED_OUT)
        return -110; /* ETIMEDOUT */
    return -1;
}

static inline int a20_linux_futex_wake(uint32_t *addr, uint32_t count)
{
    uint32_t woken = 0;
    a20_status_t r = a20_futex_wake(addr, count, &woken);
    if (a20_status_is_err(r))
        return -1;
    return (int)woken;
}

#endif
