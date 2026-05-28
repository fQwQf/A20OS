#ifndef _KLOG_H
#define _KLOG_H

#include "core/types.h"

/* Kernel ring buffer log — readable by dmesg syscall */

#define KLOG_BUF_SIZE   (64 * 1024)     /* 64KB ring buffer */

void klog_init(void);
void klog_write(const char *fmt, ...);
int  klog_read(char *buf, size_t size, size_t *pos);
void klog_clear(void);
size_t klog_len(void);

/* Log levels */
#define KLOG_DEBUG  0
#define KLOG_INFO   1
#define KLOG_WARN   2
#define KLOG_ERR    3

extern int klog_level;

#define klog(lvl, ...) do { if ((lvl) >= klog_level) klog_write(__VA_ARGS__); } while(0)
#define kdebug(...)  klog(KLOG_DEBUG, __VA_ARGS__)
#define kinfo(...)   klog(KLOG_INFO,  __VA_ARGS__)
#define kwarn(...)   klog(KLOG_WARN,  "[WARN] " __VA_ARGS__)
#define kerr(...)    klog(KLOG_ERR,   "[ERR] "  __VA_ARGS__)

/*
 * Compile-time trace switches for very noisy debugging paths.  They default
 * to off because several of these paths sit in scheduler, syscall, trap, or
 * spinlock hot paths.
 */
#ifndef CONFIG_DEBUG_LOCKS
#define CONFIG_DEBUG_LOCKS 0
#endif
#ifndef CONFIG_DEBUG_TRAP_TRACE
#define CONFIG_DEBUG_TRAP_TRACE 0
#endif
#ifndef CONFIG_DEBUG_SYSCALL_TRACE
#define CONFIG_DEBUG_SYSCALL_TRACE 0
#endif
#ifndef CONFIG_DEBUG_SCHED_TRACE
#define CONFIG_DEBUG_SCHED_TRACE 0
#endif
#ifndef CONFIG_DEBUG_FD_TRACE
#define CONFIG_DEBUG_FD_TRACE 0
#endif
#ifndef CONFIG_DEBUG_VFS_TRACE
#define CONFIG_DEBUG_VFS_TRACE 0
#endif
#ifndef CONFIG_DEBUG_NET_TRACE
#define CONFIG_DEBUG_NET_TRACE 0
#endif
#ifndef CONFIG_DEBUG_MM_TRACE
#define CONFIG_DEBUG_MM_TRACE 0
#endif
#ifndef CONFIG_DEBUG_EXIT_TRACE
#define CONFIG_DEBUG_EXIT_TRACE 0
#endif

void printf(const char *fmt, ...);

#if CONFIG_DEBUG_TRAP_TRACE
#define ktrace_trap(...) printf(__VA_ARGS__)
#else
#define ktrace_trap(...) do {} while (0)
#endif

#if CONFIG_DEBUG_SYSCALL_TRACE
#define ktrace_syscall(...) printf(__VA_ARGS__)
#else
#define ktrace_syscall(...) do {} while (0)
#endif

#if CONFIG_DEBUG_SCHED_TRACE
#define ktrace_sched(...) printf(__VA_ARGS__)
#else
#define ktrace_sched(...) do {} while (0)
#endif

#if CONFIG_DEBUG_FD_TRACE
#define ktrace_fd(...) printf(__VA_ARGS__)
#else
#define ktrace_fd(...) do {} while (0)
#endif

#if CONFIG_DEBUG_VFS_TRACE
#define ktrace_vfs(...) printf(__VA_ARGS__)
#else
#define ktrace_vfs(...) do {} while (0)
#endif

#if CONFIG_DEBUG_NET_TRACE
#define ktrace_net(...) printf(__VA_ARGS__)
#else
#define ktrace_net(...) do {} while (0)
#endif

#if CONFIG_DEBUG_MM_TRACE
#define ktrace_mm(...) printf(__VA_ARGS__)
#else
#define ktrace_mm(...) do {} while (0)
#endif

#if CONFIG_DEBUG_EXIT_TRACE
#define ktrace_exit(...) printf(__VA_ARGS__)
#else
#define ktrace_exit(...) do {} while (0)
#endif

#endif /* _KLOG_H */
