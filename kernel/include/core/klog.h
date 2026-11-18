#ifndef _KLOG_H
#define _KLOG_H

#include "core/types.h"

/* Kernel ring buffer log — readable by dmesg syscall.
 *
 * There are two views of the same stream:
 *   - a byte ring for /dev/kmsg and dmesg, unchanged since before;
 *   - a fixed-size structured ring (timestamp, level, facility, message) that
 *     the persistent writer turns into JSONL on the "A20OS log" partition.
 * Both are appended under the same lock, so their ordering cannot diverge.
 */

#ifndef CONFIG_KLOG_BUF_SIZE
#define CONFIG_KLOG_BUF_SIZE (64 * 1024)
#endif
#define KLOG_BUF_SIZE CONFIG_KLOG_BUF_SIZE

/* Structured records kept for the persistent writer.  1024 records cover a
 * verbose boot (the shell gate currently produces ~400 lines) with room for a
 * driver probe burst; the disk writer drains them every second. */
#ifndef CONFIG_KLOG_REC_COUNT
#ifdef CONFIG_MCU
/* Keep the structured view inside the MCU SRAM budget as well as the byte ring. */
#define CONFIG_KLOG_REC_COUNT 2
#else
#define CONFIG_KLOG_REC_COUNT 1024
#endif
#endif
#define KLOG_REC_COUNT CONFIG_KLOG_REC_COUNT
#define KLOG_REC_MSG_MAX  240
#define KLOG_REC_FAC_MAX  16

/* Build-time defaults, set from the instance [log] section by the Makefile.
 * a20.log.* command-line arguments override the runtime subset. */
#ifndef CONFIG_KLOG_ENABLED
#define CONFIG_KLOG_ENABLED 1
#endif
#ifndef CONFIG_KLOG_DEFAULT_LEVEL
#define CONFIG_KLOG_DEFAULT_LEVEL 1   /* KLOG_INFO */
#endif
#ifndef CONFIG_KLOG_CONSOLE
#define CONFIG_KLOG_CONSOLE 1
#endif
#ifndef CONFIG_KLOG_SESSION
#define CONFIG_KLOG_SESSION 1
#endif
#ifndef CONFIG_KLOG_MAX_BYTES
#define CONFIG_KLOG_MAX_BYTES (8U * 1024U * 1024U)
#endif
#ifndef CONFIG_KLOG_MODE
#define CONFIG_KLOG_MODE 0
#endif
#define KLOG_MODE_AUTO      0
#define KLOG_MODE_PARTITION 1
#define KLOG_MODE_ROOTFS    2
#define KLOG_MODE_ESP       4
#define KLOG_MODE_RAM       3
typedef struct klog_record {
    uint64_t mono_ns;
    uint64_t realtime_s;
    uint32_t seq;
    uint8_t  level;
    uint8_t  facility_len;
    uint16_t msg_len;
    char     facility[KLOG_REC_FAC_MAX];
    char     msg[KLOG_REC_MSG_MAX];
} klog_record_t;

void klog_init(void);
void klog_write(const char *fmt, ...);
int  klog_write_ex(int level, const char *fmt, ...);
/* Retain diagnostics in both rings/JSONL without repainting the console. */
int  klog_record_only(int level, const char *fmt, ...);
void klog_write_raw(const char *buf, size_t len);
int  klog_read(char *buf, size_t size, size_t *pos);
void klog_clear(void);
size_t klog_len(void);

/* Emit already-formatted bytes at @level.  printf() and klog_write_ex() both
 * end here, so a line cannot appear on the console but be missing from the
 * structured ring. */
void klog_emit_text(int level, const char *msg, size_t len);

/* Resolve a20.log.* command-line overrides once bootargs_init() has run. */
void klog_configure(void);
int klog_console_enabled(void);
int klog_persistent_enabled(void);
int klog_mode(void);
uint64_t klog_max_bytes(void);

/* Per-boot identity.  Unlike /proc/sys/kernel/boot_id (which is derived from
 * the uptime counter), this is random and stable for the whole boot. */
const char *klog_boot_id(void);

/* Structured records.  klog_record_read() copies the record at *cursor and
 * advances it; it returns 1 when a record was copied, 0 when the caller is
 * current, and clamps a cursor that fell out of the ring to the oldest record
 * instead of silently skipping. */
int klog_record_read(uint32_t *cursor, klog_record_t *out);
uint32_t klog_record_oldest(void);
uint32_t klog_record_next(void);

/* Event producers supply an explicit subsystem; existing printf/kerr callers
 * remain supported. Snapshots expose owned copies, never logging internals. */
int klog_event_write(int level, const char *facility, const char *format, ...);
#if CONFIG_KLOG_ENABLED
#define klog_event(...) klog_event_write(__VA_ARGS__)
#else
/* A call expression can be ignored under -Wunused-value, while the dead
 * producer branch keeps argument checking without evaluating arguments. */
static inline int klog_event_omitted(void) { return 0; }
#define klog_event(...) (0 ? klog_event_write(__VA_ARGS__) : klog_event_omitted())
#endif
int klog_has_errors(void);
typedef struct klog_snapshot {
    uint32_t first, end, retained;
    unsigned warnings, errors, matching, shown;
} klog_snapshot_t;
void klog_snapshot_select(klog_snapshot_t *snapshot, klog_record_t *records,
                          unsigned capacity, int context);
void klog_console_replay(const char *buffer, size_t length);

/* Persistent writer (kernel/core/klog_disk.c).  The mount code calls attach()
 * once the "A20OS log" partition is mounted; attach() is the only function
 * that resolves a path, and it runs before chroot. */
#ifndef CONFIG_KLOG_PERSISTENT
#define CONFIG_KLOG_PERSISTENT 0
#endif
int  klog_disk_attach(const char *dir);
int  klog_disk_ready(void);
/* Ready means a writable sink exists, including RAM. Storage describes its
 * actual backing; it is not a claim that every pending write is durable. */
const char *klog_disk_storage(void);
const char *klog_disk_health(void);
const char *klog_disk_dir(void);
void klog_disk_sync(void);
void klog_disk_poll(void);
void klog_disk_panic(void);
void klog_disk_mark_ready(void);
void klog_disk_mark_clean(void);
void klog_disk_thread(void);

/* Log levels */
#define KLOG_DEBUG  0
#define KLOG_INFO   1
#define KLOG_WARN   2
#define KLOG_ERR    3

extern int klog_level;

#if CONFIG_KLOG_ENABLED
#define klog(lvl, ...) do { if ((lvl) >= klog_level) klog_write_ex((lvl), __VA_ARGS__); } while(0)
#else
/* Keep arguments type-checked and locals referenced, but emit no call or
 * argument evaluation in the disabled build. */
#define klog(lvl, ...) do { if (0) klog_write_ex((lvl), __VA_ARGS__); } while (0)
#endif
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
