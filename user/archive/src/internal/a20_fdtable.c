/*
 * musl A20 fd table — POSIX fd ↔ A20 handle mapping.
 *
 * musl assumes small-integer fd (0=stdin, 1=stdout, 2=stderr) while
 * A20 uses opaque handle table indices. This module bridges the gap.
 *
 * See startup.md §3 for the design rationale.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>

/* ---- A20 handle type ---- */
typedef uint32_t a20_handle_t;
#define A20_HANDLE_NULL  ((a20_handle_t)0xFFFFFFFF)

/* ---- fd table constants ---- */
#define A20_FD_INIT_SIZE   32
#define A20_FD_MAX_SIZE    1024

/* O_* flags we care about */
#define A20_O_RDONLY   0x0000
#define A20_O_WRONLY   0x0001
#define A20_O_RDWR     0x0002
#define A20_O_ACCMODE  0x0003
#define A20_O_CLOEXEC  0x80000

/* off_t */
typedef int64_t a20_off_t;

/* Simple spinlock for user-space (freestanding, no pthread dependency) */
typedef volatile int a20_spinlock_t;

static inline void a20_spin_lock(a20_spinlock_t *lk)
{
    while (__builtin_expect(__atomic_test_and_set(lk, __ATOMIC_ACQUIRE), 1))
        ;
}

static inline void a20_spin_unlock(a20_spinlock_t *lk)
{
    __atomic_clear(lk, __ATOMIC_RELEASE);
}

/* ---- fd entry ---- */
struct a20_fd_entry {
    a20_handle_t  handle;       /* A20 handle; A20_HANDLE_NULL = free slot */
    uint32_t      flags;        /* O_RDONLY / O_WRONLY / O_RDWR */
    a20_off_t     pos;          /* per-fd file offset (for lseek semantics) */
    uint32_t      fd_flags;     /* FD_CLOEXEC etc */
};

/* ---- globals ---- */
static struct a20_fd_entry *__fd_table;
static int                   __fd_table_size;   /* current capacity */
static int                   __fd_table_used;    /* high-water mark hint */
static a20_spinlock_t        __fd_lock = 0;

/* ---- bare allocator for pre-malloc init ---- */
/*
 * __bare_alloc: bump allocator on a pre-registered arena.
 * Called before malloc is available (during __fd_table_init).
 */
static uint8_t *__bare_heap;
static size_t   __bare_heap_pos;
static size_t   __bare_heap_cap;

/* Called by crt0 / __libc_init before any fd operation. */
void __a20_bare_arena_init(void *base, size_t cap)
{
    __bare_heap     = (uint8_t *)base;
    __bare_heap_pos = 0;
    __bare_heap_cap = cap;
}

static void *__bare_alloc(size_t n)
{
    /* align to 16 bytes */
    n = (n + 15) & ~(size_t)15;
    if (__bare_heap_pos + n > __bare_heap_cap)
        return NULL;
    void *p = __bare_heap + __bare_heap_pos;
    __bare_heap_pos += n;
    return p;
}

/* ---- fd table operations ---- */

/*
 * __fd_table_init — initialise fd 0/1/2 from start_info handles.
 *
 * @si_stdin/stdout/stderr: handle values from a20_start_info_t.
 * @arena_base/cap: caller-provided memory for the initial table.
 */
void __fd_table_init(a20_handle_t si_stdin,
                     a20_handle_t si_stdout,
                     a20_handle_t si_stderr,
                     void *arena_base, size_t arena_cap)
{
    __a20_bare_arena_init(arena_base, arena_cap);

    __fd_table = (struct a20_fd_entry *)__bare_alloc(
        A20_FD_INIT_SIZE * sizeof(struct a20_fd_entry));
    if (!__fd_table) return; /* cannot happen with adequate arena */

    memset(__fd_table, 0, A20_FD_INIT_SIZE * sizeof(struct a20_fd_entry));
    __fd_table_size = A20_FD_INIT_SIZE;
    __fd_table_used = 3;

    /* fd 0 = stdin */
    __fd_table[0].handle = si_stdin;
    __fd_table[0].flags  = A20_O_RDONLY;

    /* fd 1 = stdout */
    __fd_table[1].handle = si_stdout;
    __fd_table[1].flags  = A20_O_WRONLY;

    /* fd 2 = stderr */
    __fd_table[2].handle = si_stderr;
    __fd_table[2].flags  = A20_O_WRONLY;
}

/*
 * __fd_alloc — allocate the smallest available fd for a given handle.
 * Returns fd on success, -1 with errno=EMFILE on failure.
 */
int __fd_alloc(a20_handle_t handle, uint32_t flags)
{
    if (handle == A20_HANDLE_NULL) {
        errno = EBADF;
        return -1;
    }

    a20_spin_lock(&__fd_lock);

    /* scan for first free slot */
    for (int i = 0; i < __fd_table_size; i++) {
        if (__fd_table[i].handle == A20_HANDLE_NULL) {
            __fd_table[i].handle   = handle;
            __fd_table[i].flags    = flags & A20_O_ACCMODE;
            __fd_table[i].pos      = 0;
            __fd_table[i].fd_flags = (flags & A20_O_CLOEXEC) ? A20_O_CLOEXEC : 0;
            if (i >= __fd_table_used)
                __fd_table_used = i + 1;
            a20_spin_unlock(&__fd_lock);
            return i;
        }
    }

    /* try to grow (if malloc available) */
    /* TODO: once malloc is up, realloc the table */

    a20_spin_unlock(&__fd_lock);
    errno = EMFILE;
    return -1;
}

/*
 * __fd_to_handle — convert fd to A20 handle.
 * Returns A20_HANDLE_NULL if fd is out of range or free.
 */
a20_handle_t __fd_to_handle(int fd)
{
    if (fd < 0 || fd >= __fd_table_size)
        return A20_HANDLE_NULL;
    return __fd_table[fd].handle;
}

/*
 * __fd_to_entry — get fd entry pointer (for pos/fd_flags access).
 * Returns NULL if fd is invalid.
 */
struct a20_fd_entry *__fd_to_entry(int fd)
{
    if (fd < 0 || fd >= __fd_table_size)
        return NULL;
    if (__fd_table[fd].handle == A20_HANDLE_NULL)
        return NULL;
    return &__fd_table[fd];
}

/*
 * __fd_free — release an fd slot (does NOT close the underlying handle).
 * Returns 0 on success, -1 with errno=EBADF on invalid fd.
 */
int __fd_free(int fd)
{
    a20_spin_lock(&__fd_lock);

    if (fd < 0 || fd >= __fd_table_size ||
        __fd_table[fd].handle == A20_HANDLE_NULL) {
        a20_spin_unlock(&__fd_lock);
        errno = EBADF;
        return -1;
    }

    __fd_table[fd].handle   = A20_HANDLE_NULL;
    __fd_table[fd].flags    = 0;
    __fd_table[fd].pos      = 0;
    __fd_table[fd].fd_flags = 0;

    a20_spin_unlock(&__fd_lock);
    return 0;
}

/*
 * __fd_dup — duplicate fd to a specific target (for dup2 semantics).
 * Returns new_fd on success, -1 on failure.
 */
int __fd_dup(int old_fd, int new_fd)
{
    a20_spin_lock(&__fd_lock);

    if (old_fd < 0 || old_fd >= __fd_table_size ||
        __fd_table[old_fd].handle == A20_HANDLE_NULL) {
        a20_spin_unlock(&__fd_lock);
        errno = EBADF;
        return -1;
    }

    /* grow table if needed */
    if (new_fd >= __fd_table_size) {
        /* TODO: grow */
        a20_spin_unlock(&__fd_lock);
        errno = EBADF;
        return -1;
    }

    /* if new_fd is occupied, the caller should have closed the handle */
    __fd_table[new_fd].handle   = __fd_table[old_fd].handle;
    __fd_table[new_fd].flags    = __fd_table[old_fd].flags;
    __fd_table[new_fd].pos      = __fd_table[old_fd].pos;
    __fd_table[new_fd].fd_flags = 0; /* dup2 clears CLOEXEC by default */

    if (new_fd >= __fd_table_used)
        __fd_table_used = new_fd + 1;

    a20_spin_unlock(&__fd_lock);
    return new_fd;
}

/*
 * __fd_get_cloexec — get close-on-exec flag.
 */
int __fd_get_cloexec(int fd)
{
    if (fd < 0 || fd >= __fd_table_size)
        return 0;
    return (__fd_table[fd].fd_flags & A20_O_CLOEXEC) ? 1 : 0;
}

/*
 * __fd_set_cloexec — set/clear close-on-exec flag.
 */
int __fd_set_cloexec(int fd, int cloexec)
{
    a20_spin_lock(&__fd_lock);
    if (fd < 0 || fd >= __fd_table_size ||
        __fd_table[fd].handle == A20_HANDLE_NULL) {
        a20_spin_unlock(&__fd_lock);
        errno = EBADF;
        return -1;
    }
    if (cloexec)
        __fd_table[fd].fd_flags |= A20_O_CLOEXEC;
    else
        __fd_table[fd].fd_flags &= ~A20_O_CLOEXEC;
    a20_spin_unlock(&__fd_lock);
    return 0;
}

/*
 * __fd_table_count — return number of active fds (for /proc-style queries).
 */
int __fd_table_count(void)
{
    int count = 0;
    a20_spin_lock(&__fd_lock);
    for (int i = 0; i < __fd_table_size; i++)
        if (__fd_table[i].handle != A20_HANDLE_NULL)
            count++;
    a20_spin_unlock(&__fd_lock);
    return count;
}

/*
 * __fd_close_on_exec — close all fds with CLOEXEC set.
 * Called during posix_spawn / task_spawn before the child starts.
 */
void __fd_close_on_exec(void)
{
    a20_spin_lock(&__fd_lock);
    for (int i = 0; i < __fd_table_size; i++) {
        if (__fd_table[i].handle != A20_HANDLE_NULL &&
            (__fd_table[i].fd_flags & A20_O_CLOEXEC)) {
            /* The actual handle_close syscall must be done by the caller
             * after we release the lock; we just clear the slot. */
            a20_handle_t h = __fd_table[i].handle;
            __fd_table[i].handle   = A20_HANDLE_NULL;
            __fd_table[i].flags    = 0;
            __fd_table[i].pos      = 0;
            __fd_table[i].fd_flags = 0;
            a20_spin_unlock(&__fd_lock);
            /* invoke handle_close outside the lock */
            extern long __syscall1(long, long);
            __syscall1(0x0100, (long)h); /* 0x0100 = __NR_a20_handle_close */
            a20_spin_lock(&__fd_lock);
        }
    }
    a20_spin_unlock(&__fd_lock);
}
