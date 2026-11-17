/*
 * Persistent kernel log.
 *
 * The kernel ring buffer is volatile; a machine that stops before userspace
 * starts leaves nothing behind.  This file drains the structured ring into a
 * JSONL file on the dedicated "A20OS log" FAT32 partition, which any host OS
 * can read after the USB stick is removed.
 *
 * Design constraints, in order:
 *   1. It must not run in IRQ context.  klog_emit_text() only appends to the
 *      ring; this writer runs from a kernel thread (and from boot phase
 *      boundaries, which are process context).
 *   2. It must not depend on the distro rootfs.  The log partition is mounted
 *      by mount_persistent_log() before chroot; the open vfile is held across
 *      the chroot so path resolution never has to be repeated.
 *   3. It must be machine-readable.  One JSON object per line, with a header
 *      line for the boot identity.  A torn last line is simply invalid JSON and
 *      is discarded by the reader.
 *
 * What it cannot do: if the block driver itself is dead, or the failure happens
 * before the partition is mounted, no kernel log can be written.  That window
 * is covered by the UEFI loader's /A20OS/boot.log on the ESP and by the serial
 * console.  The documentation states this boundary rather than implying a
 * guarantee the storage stack cannot provide.
 */
#include "core/klog.h"
#include "core/bootargs.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "proc/proc.h"

#ifndef CONFIG_KLOG_PERSISTENT
#define CONFIG_KLOG_PERSISTENT 0
#endif

#if CONFIG_KLOG_PERSISTENT && CONFIG_KLOG_ENABLED

#define KLOG_DISK_CURRENT_BYTES 256U

static vfile_t *g_boot_vf;
static vfile_t *g_current_vf;
static mutex_t g_disk_lock;
static uint32_t g_cursor;
static uint64_t g_bytes;
static uint64_t g_last_fsync_tick;
static int g_ready;
static int g_full;
static const char *g_state = "booting";
static int g_diag_write;
static int g_diag_current;
static int g_io_error;
static const char *g_storage = "none";
static char g_dir[128];

static const char *klog_file_storage(const vfile_t *file) {
    if (!file || !file->vnode || !file->vnode->mnt) return "unknown";
    switch (file->vnode->mnt->type) {
    case FS_TYPE_RAMFS: return "ram";
    case FS_TYPE_FAT32: case FS_TYPE_EXT4: case FS_TYPE_NTFS: case FS_TYPE_LITTLEFS:
        return "persistent";
    default: return "unknown"; /* a proxy/virtual filesystem is not evidence of disk backing */
    }
}

static void klog_sync_file(vfile_t *file) {
    int result = vfs_fsync_vfile(file);
    if (result < 0) __atomic_store_n(&g_io_error, result, __ATOMIC_RELEASE);
}

/* The loader increments this counter before every handoff and passes it on
 * the kernel command line.  Recording it in CURRENT lets a host correlate the
 * loader's early-<count>.jsonl with the kernel's boot-<id>.jsonl even though
 * the kernel's random boot id is generated later. */
static unsigned klog_bootcount(void) {
    const char *args = bootargs_get();
    const char *p = args ? strstr(args, "a20.bootcount=") : NULL;
    if (!p)
        return 0;
    p += 14;
    unsigned value = 0;
    while (*p >= '0' && *p <= '9')
        value = value * 10U + (unsigned)(*p++ - '0');
    return value;
}

static const char *klog_level_name(int level) {
    switch (level) {
        case KLOG_DEBUG: return "debug";
        case KLOG_WARN:  return "warn";
        case KLOG_ERR:   return "err";
        default:         return "info";
    }
}

/* JSON string escaping.  Kernel messages are ASCII in practice, but a driver
 * can print a path or a device string; control bytes must not break the line
 * framing. */
static size_t json_escape(char *dst, size_t cap, const char *src, size_t len) {
    size_t out = 0;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len && out + 7 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            dst[out++] = '\\';
            dst[out++] = (char)c;
        } else if (c == '\n') {
            dst[out++] = '\\';
            dst[out++] = 'n';
        } else if (c == '\r') {
            dst[out++] = '\\';
            dst[out++] = 'r';
        } else if (c == '\t') {
            dst[out++] = '\\';
            dst[out++] = 't';
        } else if (c < 0x20) {
            dst[out++] = '\\';
            dst[out++] = 'u';
            dst[out++] = '0';
            dst[out++] = '0';
            dst[out++] = hex[c >> 4];
            dst[out++] = hex[c & 0xf];
        } else {
            dst[out++] = (char)c;
        }
    }
    return out;
}

static int write_text_once(const char *path, const char *text) {
    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fd;
    size_t len = strlen(text);
    int r = vfs_write(fd, text, len);
    if (r >= 0)
        (void)vfs_fsync(fd);
    vfs_close(fd);
    return r < 0 ? r : 0;
}

static void write_current(const char *state) {
    if (!g_current_vf)
        return;
    char buf[KLOG_DISK_CURRENT_BYTES];
    memset(buf, ' ', sizeof(buf));
    int n = snprintf(buf, sizeof(buf),
                     "boot=%s\nbootcount=%u\nseq=%u\nbytes=%llu\n"
                     "session=%d\nstate=%s\nstorage=%s\nwriter=%s\n",
                     klog_boot_id(), klog_bootcount(),
                     (unsigned)klog_record_next(),
                     (unsigned long long)g_bytes, CONFIG_KLOG_SESSION, state,
                     g_storage, klog_disk_health());
    if (n < 0)
        return;
    if ((size_t)n >= sizeof(buf))
        n = (int)sizeof(buf) - 1;
    /* snprintf's terminator is overwritten with padding: CURRENT is a fixed
     * slot, and a NUL byte in the middle makes the file look binary to tools
     * that do not stop at a text terminator. */
    for (size_t i = (size_t)n; i < sizeof(buf); i++)
        buf[i] = ' ';
    /* Always rewrite the whole slot: a shorter state such as "ready" must not
     * leave the tail of a longer "booting" behind. */
    n = (int)sizeof(buf);
    (void)vfs_lseek_vfile(g_current_vf, 0, SEEK_SET);
    int wr = vfs_write_file(g_current_vf, buf, (size_t)n);
    if (wr < 0 || wr != n) {
        __atomic_store_n(&g_io_error, wr < 0 ? wr : -EIO, __ATOMIC_RELEASE);
        if (!g_diag_current) kerr("[LOG] CURRENT write incomplete: %d/%d\n", wr, n);
        g_diag_current = 1;
    }
    klog_sync_file(g_current_vf);
}

static void klog_disk_drain(int force);

int klog_disk_attach(const char *dir) {
    if (!klog_persistent_enabled())
        return -EOPNOTSUPP;
    if (g_ready)
        return 0;
    if (!dir || strlen(dir) >= sizeof(g_dir))
        return -EINVAL;
    strncpy(g_dir, dir, sizeof(g_dir) - 1);
    g_dir[sizeof(g_dir) - 1] = '\0';

    char path[192];
    snprintf(path, sizeof(path), "%s/boot-%s.jsonl", g_dir, klog_boot_id());
    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return fd;
    g_boot_vf = vfs_get_file_ref(fd);
    vfs_close(fd);
    if (!g_boot_vf) {
        kerr("[LOG] boot file reference failed\n");
        return -ENOMEM;
    }
    g_storage = klog_file_storage(g_boot_vf);

    snprintf(path, sizeof(path), "%s/CURRENT", g_dir);
    fd = vfs_open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        g_current_vf = vfs_get_file_ref(fd);
        vfs_close(fd);
    }
    if (!g_current_vf)
        kerr("[LOG] CURRENT file reference failed\n");

    /* Explain the files to a person who pulls the stick and mounts it on a
     * machine that has never heard of A20OS. */
    static const char readme[] =
        "A20OS boot log (RAM sinks are volatile; check CURRENT storage)\n"
        "=========================\n"
        "\n"
        "boot-<id>.jsonl  one JSON object per line\n"
        "  v        schema version (1)\n"
        "  boot     random per-boot id; matches ESP /A20OS/boot.log\n"
        "  seq      record sequence inside this boot\n"
        "  mono_ns  nanoseconds since kernel start\n"
        "  rt       Unix seconds at record time (may be 0 before RTC is read)\n"
        "  lvl      debug | info | warn | err\n"
        "  src      subsystem tag, e.g. NVME, I2C-DW, INIT\n"
        "  msg      human-readable message\n"
        "\n"
        "CONFIG           build-time policy: session, mode, level, max_bytes\n"
        "CURRENT          boot id, last sequence, byte count, session, state\n"
        "  state=booting   kernel log is live\n"
        "  state=ready     userspace reached its ready point\n"
        "  state=clean     shutdown path ran\n"
        "\n"
        "A line without a closing brace is a torn write from a power cut;\n"
        "discard that line and read the rest.\n"
        "\n"
        "The UEFI loader also appends to /A20OS/boot.log on the ESP.  Read that\n"
        "first when the screen never lit up: it records the last handoff stage\n"
        "the firmware completed.\n";
    snprintf(path, sizeof(path), "%s/README.txt", g_dir);
    (void)write_text_once(path, readme);

    /* A tiny machine-readable policy file for userspace (the session logger)
     * and host tools.  CURRENT carries the live boot state; CONFIG carries the
     * build-time policy that does not change during the boot. */
    char config[160];
    snprintf(config, sizeof(config),
             "schema=1\nsession=%d\nmode=%d\nlevel=%d\nmax_bytes=%llu\n",
             CONFIG_KLOG_SESSION, CONFIG_KLOG_MODE,
             CONFIG_KLOG_DEFAULT_LEVEL,
             (unsigned long long)CONFIG_KLOG_MAX_BYTES);
    snprintf(path, sizeof(path), "%s/CONFIG", g_dir);
    (void)write_text_once(path, config);

    mutex_init(&g_disk_lock);
    g_cursor = klog_record_oldest();
    g_bytes = 0;
    g_full = 0;
    g_state = "booting";
    g_ready = 1;
    write_current(g_state);
    /* Flush the records that were already in the ring before the block
     * device and filesystem became available.  Waiting for the 1 Hz thread
     * would leave a window where a panic after attach still loses the early
     * boot history, and the whole point of attaching here is evidence. */
    klog_disk_drain(1);
    return 0;
}

int klog_disk_ready(void) {
    return g_ready;
}

const char *klog_disk_dir(void) {
    return g_ready ? g_dir : "";
}

const char *klog_disk_storage(void) { return g_ready ? g_storage : "none"; }
const char *klog_disk_health(void) {
    if (!g_ready) return "unavailable";
    if (__atomic_load_n(&g_io_error, __ATOMIC_ACQUIRE)) return "io-error";
    if (__atomic_load_n(&g_full, __ATOMIC_ACQUIRE)) return "capped";
    if (!g_current_vf) return "metadata-unavailable";
    return "ready";
}

static int flush_batch(const char *buf, size_t len) {
    if (!len)
        return 0;
    if (g_bytes + len > klog_max_bytes()) {
        static const char tail[] =
            "{\"v\":1,\"lvl\":\"warn\",\"src\":\"A20LOG\","
            "\"msg\":\"log size cap reached; further records dropped\"}\n";
        (void)vfs_write_file(g_boot_vf, tail, sizeof(tail) - 1);
        g_full = 1;
        return 0;
    }
    (void)vfs_lseek_vfile(g_boot_vf, 0, SEEK_END);
    int r = vfs_write_file(g_boot_vf, buf, len);
    if (r > 0)
        g_bytes += (uint64_t)r;
    if (r < 0 || (size_t)r != len) {
        __atomic_store_n(&g_io_error, r < 0 ? r : -EIO, __ATOMIC_RELEASE);
        if (!g_diag_write) kerr("[LOG] journal write incomplete: %d/%lu\n", r, (unsigned long)len);
        g_diag_write = 1;
    }
    return r < 0 ? r : 0;
}

static void klog_disk_drain_locked(int force) {
    if (!g_ready || g_full)
        return;

    char line[2048];
    char batch[4096];
    size_t used = 0;
    int urgent = 0;
    klog_record_t rec;
    while (klog_record_read(&g_cursor, &rec)) {
        if (rec.level >= KLOG_WARN)
            urgent = 1;
        char msg[KLOG_REC_MSG_MAX * 6 + 1];
        size_t mlen = json_escape(msg, sizeof(msg), rec.msg, rec.msg_len);
        msg[mlen] = '\0';
        char fac[KLOG_REC_FAC_MAX * 6 + 1];
        size_t flen = json_escape(fac, sizeof(fac), rec.facility,
                                  rec.facility_len);
        fac[flen] = '\0';
        int n = snprintf(line, sizeof(line),
                         "{\"v\":1,\"boot\":\"%s\",\"seq\":%u,"
                         "\"mono_ns\":%llu,\"rt\":%llu,\"lvl\":\"%s\","
                         "\"src\":\"%s\",\"msg\":\"%s\"}\n",
                         klog_boot_id(), (unsigned)rec.seq,
                         (unsigned long long)rec.mono_ns,
                         (unsigned long long)rec.realtime_s,
                         klog_level_name(rec.level), fac, msg);
        if (n <= 0)
            continue;
        if ((size_t)n >= sizeof(line)) {
            /* snprintf truncated; keep the line valid by closing it here. */
            size_t cut = sizeof(line) - 4;
            line[cut] = '"';
            line[cut + 1] = '}';
            line[cut + 2] = '\n';
            line[cut + 3] = '\0';
            n = (int)(cut + 3);
        }
        if (used + (size_t)n > sizeof(batch)) {
            (void)flush_batch(batch, used);
            used = 0;
            if (g_full)
                break;
        }
        memcpy(batch + used, line, (size_t)n);
        used += (size_t)n;
    }
    if (used)
        (void)flush_batch(batch, used);

    /* Writing into the block cache is cheap; fsync is what makes a USB stick
     * stall.  The 1 Hz thread therefore drains records every second but only
     * commits them every five seconds, unless a warning/error arrived or a
     * boot phase/state transition asked for a forced sync.  A power cut can
     * lose at most the uncommitted tail. */
    uint64_t now = timer_get_ticks();
    int due = force || urgent || !g_last_fsync_tick ||
              now - g_last_fsync_tick >= MS_TO_TICKS(5000);
    if (due) {
        klog_sync_file(g_boot_vf);
        write_current(g_state);
        g_last_fsync_tick = now;
    }
}

static void klog_disk_drain(int force) {
    if (!g_ready || g_full)
        return;
    mutex_lock(&g_disk_lock);
    klog_disk_drain_locked(force);
    mutex_unlock(&g_disk_lock);
}

void klog_disk_sync(void) {
    klog_disk_drain(1);
}

void klog_disk_panic(void) {
    if (!g_ready)
        return;
    /* Best effort only: the panic may be inside the block or VFS stack, so a
     * failed trylock must not turn the panic into a deadlock. */
    if (!mutex_trylock(&g_disk_lock))
        return;
    klog_disk_drain_locked(1);
    g_state = "panic";
    write_current(g_state);
    klog_sync_file(g_boot_vf);
    mutex_unlock(&g_disk_lock);
}

void klog_disk_poll(void) {
    klog_disk_drain(0);
}

void klog_disk_mark_ready(void) {
    if (!g_ready)
        return;
    klog_disk_sync();
    mutex_lock(&g_disk_lock);
    g_state = "ready";
    write_current(g_state);
    klog_sync_file(g_boot_vf);
    mutex_unlock(&g_disk_lock);
}

void klog_disk_mark_clean(void) {
    if (!g_ready)
        return;
    klog_disk_sync();
    mutex_lock(&g_disk_lock);
    g_state = "clean";
    write_current(g_state);
    klog_sync_file(g_boot_vf);
    mutex_unlock(&g_disk_lock);
}

void klog_disk_thread(void) {
    for (;;) {
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(1000));
        klog_disk_poll();
    }
}

#else /* !CONFIG_KLOG_PERSISTENT || !CONFIG_KLOG_ENABLED */

int klog_disk_attach(const char *dir) { (void)dir; return -EOPNOTSUPP; }
int klog_disk_ready(void) { return 0; }
const char *klog_disk_storage(void) { return "none"; }
const char *klog_disk_health(void) { return "unavailable"; }
void klog_disk_sync(void) {}
void klog_disk_poll(void) {}
void klog_disk_panic(void) {}
void klog_disk_mark_ready(void) {}
void klog_disk_mark_clean(void) {}
void klog_disk_thread(void) {}

#endif
