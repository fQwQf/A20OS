/*
 * fscompat/compat.c — the user-space runtime implementation for kernel disk
 * filesystem sources.
 *
 * Works together with the shadowing headers in user/svc/fscompat/ so that the
 * ext4/isofs/ntfs sources in kernel/fs/diskfs/ compile as-is into the
 * user-space FS host (ufsd). This file supplies the kernel-facility
 * equivalents that those sources reference:
 *   - string/memory routines and printf formatting (emitted via the log sink)
 *   - kmalloc family -> malloc
 *   - copy_from_user/copy_to_user -> identity copies in the same address space
 *   - vnode/vfile reference count helpers (release semantics match the kernel:
 *     call the release op at zero)
 *   - panic/klog_write, proc_current, page_cache stubs
 */
#include <stdint.h>
/* Shadowing headers take priority: this file's own kernel-style includes all
 * resolve to the fscompat versions. */
#include "core/types.h"
#include "core/sync.h"
#include "core/klog.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/string.h"
#include "mm/mm.h"
#include "proc/proc.h"
#include "fs/vfs.h"
#include "fs/file.h"

/* ------------------------------------------------------------------ */
/* Heap and strings (self-hosted in the -nostdlib environment; the allocator
 * reuses liba20rt) */
/* ------------------------------------------------------------------ */

extern void *a20_malloc(uint64_t size);
extern void a20_free(void *ptr);
extern void *a20_realloc(void *ptr, uint64_t size);

void *malloc(size_t size)
{
    return a20_malloc(size);
}

void free(void *ptr)
{
    a20_free(ptr);
}

void *realloc(void *ptr, size_t size)
{
    return a20_realloc(ptr, size);
}

/* memcpy/memset/memmove are provided by liba20rt/a20_compiler_rt.c */

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    return n ? (int)(unsigned char)*a - (int)(unsigned char)*b : 0;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    char *d = dst;
    while (n && (*d++ = *src++))
        n--;
    while (n--)
        *d++ = '\0';
    return dst;
}

static int ic(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a && ic(*a) == ic(*b)) {
        a++;
        b++;
    }
    return ic((unsigned char)*a) - ic((unsigned char)*b);
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

/* ------------------------------------------------------------------ */
/* Log sink: the host routes kernel-style logs into its own channel */
/* ------------------------------------------------------------------ */

void (*fscompat_log_sink)(const char *line);

int klog_level = KLOG_DEBUG;

static void emit(const char *fmt, va_list ap)
{
    if (!fscompat_log_sink)
        return;
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    fscompat_log_sink(buf);
}

void klog_write(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(fmt, ap);
    va_end(ap);
}

void panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    emit(fmt, ap);
    va_end(ap);
    for (;;)
        ;
}

/* ------------------------------------------------------------------ */
/* printf family */
/* ------------------------------------------------------------------ */

static char *fmt_out(char *p, char *end, const char *s, size_t n)
{
    while (n-- && p < end)
        *p++ = *s++;
    return p;
}

static char *fmt_num(char *p, char *end, uint64_t v, int base, int neg,
                     int width, char pad, int upper, int signed_)
{
    char tmp[24];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    uint64_t uv = v;
    if (signed_ && neg)
        uv = (uint64_t)(-(int64_t)v);
    do {
        tmp[i++] = digits[uv % (unsigned)base];
        uv /= (unsigned)base;
    } while (uv);
    if (neg)
        tmp[i++] = '-';
    while (i < width && p < end)
        tmp[i++] = pad;
    /* written in reverse order */
    while (i-- && p < end)
        *p++ = tmp[i];
    return p;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    char *p = buf;
    char *end = buf + size - 1;

#define EMIT_STR(s)                                                    \
    do {                                                               \
        const char *_s = (s);                                          \
        while (*_s && p < end)                                        \
            *p++ = *_s++;                                             \
    } while (0)

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (p < end)
                *p++ = *fmt;
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0, lcount = 0, zflag = 0;
        if (*fmt == '0') {
            zflag = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        if (zflag && width)
            pad = '0';
        while (*fmt == 'l') {
            lcount++;
            fmt++;
        }
        if (*fmt == 'z') {
            lcount = 1;
            fmt++;
        }

        switch (*fmt) {
        case '%':
            if (p < end)
                *p++ = '%';
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            while (width > 1 && p < end) {
                *p++ = ' ';
                width--;
            }
            if (p < end)
                *p++ = c;
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            EMIT_STR(s);
            break;
        }
        case 'd':
        case 'i': {
            long long v = lcount >= 2 ? va_arg(ap, long long)
                          : lcount == 1 ? va_arg(ap, long)
                                        : va_arg(ap, int);
            p = fmt_num(p, end, (uint64_t)v, 10, v < 0, width, pad, 0, 1);
            break;
        }
        case 'u': {
            unsigned long long v =
                lcount >= 2 ? va_arg(ap, unsigned long long)
                : lcount == 1 ? va_arg(ap, unsigned long)
                              : va_arg(ap, unsigned int);
            p = fmt_num(p, end, v, 10, 0, width, pad, 0, 0);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long long v =
                lcount >= 2 ? va_arg(ap, unsigned long long)
                : lcount == 1 ? va_arg(ap, unsigned long)
                              : va_arg(ap, unsigned int);
            p = fmt_num(p, end, v, 16, 0, width, pad, *fmt == 'X', 0);
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            EMIT_STR("0x");
            p = fmt_num(p, end, v, 16, 0, sizeof(uintptr_t) * 2, '0', 0, 0);
            break;
        }
        default:
            if (p < end)
                *p++ = '%';
            if (p < end && *fmt)
                *p++ = *fmt;
            break;
        }
    }
#undef EMIT_STR
    *p = '\0';
    return (int)(p - buf);
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

void vprintf(const char *fmt, va_list ap)
{
    emit(fmt, ap);
}

void printf(const char *fmt, ...)
{
    if (fscompat_log_sink)
        fscompat_log_sink("[P] printf reached");
    return;
}

int puts(const char *s)
{
    if (fscompat_log_sink)
        fscompat_log_sink(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Memory and user copies */
/* ------------------------------------------------------------------ */

/* No kernel command line in a userspace service: there is no DTB and no
 * firmware, so the crash-injection knob has nothing to read and the journal
 * runs with it disarmed. */
const char *bootargs_get(void)
{
    return NULL;
}

void *kmalloc(size_t size)
{
    return malloc(size ? size : 1);
}

void *kmalloc_atomic(size_t size)
{
    return kmalloc(size);
}

void *kcalloc(size_t nmemb, size_t size)
{
    void *p = kmalloc(nmemb * size);
    if (p)
        memset(p, 0, nmemb * size);
    return p;
}

void kfree(void *ptr)
{
    free(ptr);
}

void *krealloc(void *ptr, size_t new_size)
{
    return realloc(ptr, new_size ? new_size : 1);
}

long copy_from_user(void *dst, const void *src, size_t n)
{
    memcpy(dst, src, n);
    return 0;
}

long copy_to_user(void *dst, const void *src, size_t n)
{
    memcpy(dst, src, n);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Task context */
/* ------------------------------------------------------------------ */

static task_t g_fscompat_task = {
    .pid = 1,
    .cred = { .uid = 0, .euid = 0, .suid = 0, .fsuid = 0,
              .gid = 0, .egid = 0, .sgid = 0, .fsgid = 0 },
};

task_t *proc_current(void)
{
    return &g_fscompat_task;
}

/* ------------------------------------------------------------------ */
/* vnode / vfile reference counts (release contract matches the kernel) */
/* ------------------------------------------------------------------ */

void vnode_ref_init(vnode_t *vn, int refs)
{
    if (!vn)
        return;
    vn->cache_pages = NULL;
    vn->cache_dirty_pages = NULL;
    refcount_set(&vn->ref_count, refs);
}

void vnode_get(vnode_t *vn)
{
    if (vn)
        refcount_inc(&vn->ref_count);
}

void vnode_put(vnode_t *vn)
{
    if (!vn)
        return;
    if (refcount_read(vn ? &vn->ref_count : 0) <= 0)
        return;
    if (refcount_dec_and_test(&vn->ref_count)) {
        if (vn->ops && vn->ops->release)
            vn->ops->release(vn);
    }
}

static uint64_t g_vfile_identity;

vfile_t *vfile_alloc(void)
{
    vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!vf)
        return NULL;
    memset(vf, 0, sizeof(*vf));
    vf->identity = ++g_vfile_identity;
    mutex_init(&vf->offset_lock);
    vf->lease = 37 /* F_UNLCK */;
    return vf;
}

void vfile_ref_init(vfile_t *vf, int refs)
{
    if (vf)
        refcount_set(&vf->ref_count, refs);
}

void page_cache_discard_unlinked(vnode_t *vn)
{
    (void)vn;
}

/* ------------------------------------------------------------------ */
/* Host equivalents of kernel facilities (filled in at link time) */
/* ------------------------------------------------------------------ */

int vnode_ref_read(vnode_t *vn)
{
    return vn ? refcount_read(&vn->ref_count) : 0;
}

void vfile_free(vfile_t *vf)
{
    kfree(vf);
}

void timekeeping_get_realtime(uint64_t ts[2])
{
    /* The host has no RTC read channel; timestamps are not critical to FS
     * metadata correctness, so the epoch is fixed */
    ts[0] = 0; /* seconds */
    ts[1] = 0; /* nanoseconds */
}

void vfs_drop_time_meta_identity(mount_t *mnt, uint64_t ino)
{
    (void)mnt;
    (void)ino;
}

void vfs_drop_time_meta_mount(mount_t *mnt)
{
    (void)mnt;
}

uint32_t g_a20_perf_enabled;

/* perf counters are a no-op in the host; only the storage needed for linking
 * is provided */
#include "core/perf.h"
a20_perf_cpu_counters_t g_a20_perf_percpu[A20_PERF_MAX_CPUS];
