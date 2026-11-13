#include "core/klog.h"
#include "drivers/char/uart.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/types.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/random.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "core/bootargs.h"

int klog_level = CONFIG_KLOG_DEFAULT_LEVEL;

/* Runtime overrides from a20.log.*; compile-time defaults otherwise. */
static int klog_runtime_console = CONFIG_KLOG_CONSOLE;
static int klog_runtime_persistent =
    (CONFIG_KLOG_ENABLED && CONFIG_KLOG_PERSISTENT) ? 1 : 0;
static int klog_runtime_mode = CONFIG_KLOG_MODE;
static uint64_t klog_runtime_max_bytes = CONFIG_KLOG_MAX_BYTES;

static char   klog_buf[KLOG_BUF_SIZE];
static size_t klog_head __attribute__((section(".data"))) = 0;
static size_t klog_tail __attribute__((section(".data"))) = 0;
static size_t klog_used __attribute__((section(".data"))) = 0;
static spinlock_t klog_lock = SPINLOCK_INIT;

/* Structured ring.  Guarded by klog_lock as well: the text and structured
 * views must advance together or a reader cannot tell which bytes belong to
 * which record. */
static klog_record_t klog_recs[KLOG_REC_COUNT];
static uint32_t klog_rec_tail __attribute__((section(".data"))) = 0;
static uint32_t klog_rec_count __attribute__((section(".data"))) = 0;
static uint32_t klog_rec_seq __attribute__((section(".data"))) = 0;

static uint8_t klog_boot_id_bytes[16] __attribute__((section(".data")));
static char klog_boot_id_str[33] __attribute__((section(".data")));
static int klog_boot_id_ready __attribute__((section(".data"))) = 0;

/* One console writer at a time.  Each character costs a polled MMIO write,
 * and the framebuffer is presented as one full-screen frame, so without this
 * two CPUs emitting concurrently splice their lines mid-word.  The ring has
 * its own lock: a contended console must never make another CPU spin with
 * interrupts disabled while one CPU paints.  Shared with vprintf() in
 * kernel/core/printf.c. */
spinlock_t klog_console_lock = SPINLOCK_INIT;

/* A broken driver can raise thousands of identical warning/error interrupts
 * per second.  The ring and JSONL keep every record, but painting each one on
 * a text console makes the machine unusable.  Cap the console copy at a small
 * burst per second and print one summary when the window closes. */
#define KLOG_CONSOLE_WARN_BURST 4U
static uint64_t klog_warn_window __attribute__((section(".data")));
static unsigned klog_warn_emitted __attribute__((section(".data")));
static unsigned klog_warn_suppressed __attribute__((section(".data")));

/* Defined in kernel/core/printf.c, which owns the other console entry point;
 * declared here rather than in a shared header so both console writers stay
 * serialized by one lock. */
void console_emit_locked(const char *s, size_t len);

static void klog_boot_id_init(void) {
    if (klog_boot_id_ready)
        return;
    random_fill(klog_boot_id_bytes, sizeof(klog_boot_id_bytes));
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof(klog_boot_id_bytes); i++) {
        klog_boot_id_str[i * 2] = hex[klog_boot_id_bytes[i] >> 4];
        klog_boot_id_str[i * 2 + 1] = hex[klog_boot_id_bytes[i] & 0xf];
    }
    klog_boot_id_str[32] = '\0';
    klog_boot_id_ready = 1;
}

void klog_init(void) {
    memset(klog_buf, 0, sizeof(klog_buf));
    klog_head = 0;
    klog_tail = 0;
    klog_used = 0;
    memset(klog_recs, 0, sizeof(klog_recs));
    klog_rec_tail = 0;
    klog_rec_count = 0;
    klog_rec_seq = 0;
    klog_boot_id_ready = 0;
    klog_boot_id_str[0] = '\0';
    klog_boot_id_init();
    spin_init(&klog_lock);
    spin_init(&klog_console_lock);
    klog_warn_window = 0;
    klog_warn_emitted = 0;
    klog_warn_suppressed = 0;
}

const char *klog_boot_id(void) {
    if (!klog_boot_id_ready)
        klog_boot_id_init();
    return klog_boot_id_str;
}

static void klog_append_text(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        klog_buf[klog_head] = s[i];
        klog_head = (klog_head + 1) % KLOG_BUF_SIZE;
        if (klog_used < KLOG_BUF_SIZE) {
            klog_used++;
        } else {
            klog_tail = (klog_tail + 1) % KLOG_BUF_SIZE;
        }
    }
}

/* Pull the subsystem out of the conventional "[ERR] [NVME] ..." framing.
 * Keeping the tag parsing here means drivers do not need a second call-site
 * argument just to be searchable in the JSONL. */
#if CONFIG_KLOG_ENABLED
static void klog_parse_facility(const char *msg, size_t len, char *out) {
    size_t i = 0;
    if (len >= 6 && memcmp(msg, "[ERR] ", 6) == 0)
        i = 6;
    else if (len >= 7 && memcmp(msg, "[WARN] ", 7) == 0)
        i = 7;
    if (i < len && msg[i] == '[') {
        size_t start = i + 1;
        size_t end = start;
        while (end < len && msg[end] != ']' &&
               end - start < KLOG_REC_FAC_MAX - 1)
            end++;
        if (end < len && msg[end] == ']' && end > start) {
            memcpy(out, msg + start, end - start);
            out[end - start] = '\0';
            return;
        }
    }
    memcpy(out, "kernel", 7);
}

static void klog_append_record(int level, const char *msg, size_t len) {
    uint64_t mono[2] = {0, 0};
    uint64_t rt[2] = {0, 0};
    timekeeping_get_monotonic(mono);
    timekeeping_get_realtime(rt);

    uint32_t idx;
    if (klog_rec_count < KLOG_REC_COUNT) {
        idx = (klog_rec_tail + klog_rec_count) % KLOG_REC_COUNT;
        klog_rec_count++;
    } else {
        idx = klog_rec_tail;
        klog_rec_tail = (klog_rec_tail + 1) % KLOG_REC_COUNT;
    }

    klog_record_t *rec = &klog_recs[idx];
    memset(rec, 0, sizeof(*rec));
    rec->mono_ns = mono[0] * 1000000000ULL + mono[1];
    rec->realtime_s = rt[0];
    rec->seq = klog_rec_seq++;
    rec->level = (uint8_t)level;
    klog_parse_facility(msg, len, rec->facility);
    rec->facility_len = (uint8_t)strlen(rec->facility);
    size_t copy = len < KLOG_REC_MSG_MAX - 1 ? len : KLOG_REC_MSG_MAX - 1;
    memcpy(rec->msg, msg, copy);
    rec->msg[copy] = '\0';
    rec->msg_len = (uint16_t)copy;
}
#endif /* CONFIG_KLOG_ENABLED */

static void klog_buffer_text(int level, const char *msg, size_t len) {
    if (!msg || !len)
        return;
#if !CONFIG_KLOG_ENABLED
    (void)level;
#endif
    uint64_t ring_flags = spin_lock_irqsave(&klog_lock);
    klog_append_text(msg, len);
#if CONFIG_KLOG_ENABLED
    klog_append_record(level, msg, len);
#endif
    spin_unlock_irqrestore(&klog_lock, ring_flags);
}

void klog_emit_text(int level, const char *msg, size_t len) {
    if (!msg || !len)
        return;
    klog_buffer_text(level, msg, len);
    if (!klog_runtime_console)
        return;
    /* Console output is best effort.  If another CPU is already painting,
     * drop this line from the screen instead of waiting with interrupts
     * disabled: the record is already in the ring and the persistent writer
     * will put it on disk.  This is what keeps a slow GOP scanout from
     * looking like an SMP deadlock. */
    uint64_t flags;
    if (!spin_trylock_irqsave(&klog_console_lock, &flags))
        return;
    if (level >= KLOG_WARN) {
        uint64_t now = timer_get_ticks();
        if (now) {
            if (!klog_warn_window ||
                now - klog_warn_window >= MS_TO_TICKS(1000)) {
                unsigned suppressed = klog_warn_suppressed;
                klog_warn_window = now;
                klog_warn_emitted = 0;
                klog_warn_suppressed = 0;
                if (suppressed) {
                    char note[192];
                    int n = snprintf(note, sizeof(note),
                        "[LOG] console rate limit: %u warn/err lines "
                        "suppressed; full records remain in the ring/JSONL\n",
                        suppressed);
                    if (n > 0) {
                        size_t note_len = (size_t)n < sizeof(note)
                                              ? (size_t)n : sizeof(note) - 1;
                        console_emit_locked(note, note_len);
                    }
                }
            }
            if (klog_warn_emitted >= KLOG_CONSOLE_WARN_BURST) {
                klog_warn_suppressed++;
                spin_unlock_irqrestore(&klog_console_lock, flags);
                return;
            }
            klog_warn_emitted++;
        }
    }
    console_emit_locked(msg, len);
    spin_unlock_irqrestore(&klog_console_lock, flags);
}

static int klog_vwrite(int level, const char *fmt, va_list args) {
    char msg[1024];
    int len = vsnprintf(msg, sizeof(msg), fmt, args);
    if (len < 0)
        len = 0;
    if ((size_t)len >= sizeof(msg))
        len = (int)sizeof(msg) - 1;
    klog_emit_text(level, msg, (size_t)len);
    return len;
}

void klog_write(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    klog_vwrite(KLOG_INFO, fmt, args);
    va_end(args);
}

int klog_write_ex(int level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int len = klog_vwrite(level, fmt, args);
    va_end(args);
    return len;
}

int klog_record_only(int level, const char *fmt, ...) {
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (len < 0)
        len = 0;
    if ((size_t)len >= sizeof(msg))
        len = (int)sizeof(msg) - 1;
    klog_buffer_text(level, msg, (size_t)len);
    return len;
}

/* Raw write used by /dev/kmsg: appends bytes verbatim to the ring and the
 * console, without adding a level or a newline. */
void klog_write_raw(const char *buf, size_t len) {
    klog_emit_text(KLOG_INFO, buf, len);
}

int klog_read(char *buf, size_t size, size_t *pos) {
    uint64_t flags = spin_lock_irqsave(&klog_lock);
    if (!pos || *pos >= klog_used) {
        spin_unlock_irqrestore(&klog_lock, flags);
        return 0;
    }
    size_t avail = klog_used - *pos;
    size_t to_read = avail < size ? avail : size;

    for (size_t i = 0; i < to_read; i++) {
        size_t idx = (klog_tail + *pos + i) % KLOG_BUF_SIZE;
        buf[i] = klog_buf[idx];
    }
    *pos += to_read;
    spin_unlock_irqrestore(&klog_lock, flags);
    return (int)to_read;
}

void klog_clear(void) {
    uint64_t flags = spin_lock_irqsave(&klog_lock);
    klog_head = 0;
    klog_tail = 0;
    klog_used = 0;
    spin_unlock_irqrestore(&klog_lock, flags);
}

size_t klog_len(void) {
    uint64_t flags = spin_lock_irqsave(&klog_lock);
    size_t used = klog_used;
    spin_unlock_irqrestore(&klog_lock, flags);
    return used;
}

uint32_t klog_record_oldest(void) {
    uint64_t flags = spin_lock_irqsave(&klog_lock);
    uint32_t oldest = klog_rec_count ? klog_recs[klog_rec_tail].seq : 0;
    spin_unlock_irqrestore(&klog_lock, flags);
    return oldest;
}

uint32_t klog_record_next(void) {
    uint64_t flags = spin_lock_irqsave(&klog_lock);
    uint32_t next = klog_rec_seq;
    spin_unlock_irqrestore(&klog_lock, flags);
    return next;
}

int klog_record_read(uint32_t *cursor, klog_record_t *out) {
    if (!cursor || !out)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&klog_lock);
    if (!klog_rec_count) {
        spin_unlock_irqrestore(&klog_lock, flags);
        return 0;
    }
    uint32_t oldest = klog_recs[klog_rec_tail].seq;
    if (*cursor < oldest)
        *cursor = oldest;
    uint32_t offset = *cursor - oldest;
    if (offset >= klog_rec_count) {
        spin_unlock_irqrestore(&klog_lock, flags);
        return 0;
    }
    uint32_t idx = (klog_rec_tail + offset) % KLOG_REC_COUNT;
    *out = klog_recs[idx];
    (*cursor)++;
    spin_unlock_irqrestore(&klog_lock, flags);
    return 1;
}

/* Records worth putting on the final screen: every warning/error plus the
 * subsystem lines that answer "did the keyboard, the log and the controller
 * come up?".  Everything else is still in the ring/JSONL; this is a summary,
 * not a second log. */
static int klog_report_record(const klog_record_t *rec) {
    if (rec->level >= KLOG_WARN)
        return 1;
    static const char *const facilities[] = {
        "XHCI", "PS2", "USB", "USB-HID", "LOG", "NVME", "I2C-HID", "INPUT",
    };
    for (unsigned i = 0; i < sizeof(facilities) / sizeof(facilities[0]); i++) {
        size_t len = strlen(facilities[i]);
        if (rec->facility_len == len &&
            memcmp(rec->facility, facilities[i], len) == 0)
            return 1;
    }
    return 0;
}

void klog_boot_report(void) {
    uint32_t first = klog_record_oldest();
    uint32_t end = klog_record_next();
    klog_record_t rec;
    unsigned total = 0;
    int ps2_any = 0, ps2_ok = 0, ps2_fail = 0;
    int fatal_seen = 0;
    char fatal_identity[KLOG_REC_MSG_MAX] = {0};

    for (uint32_t cursor = first; cursor < end; ) {
        if (!klog_record_read(&cursor, &rec))
            break;
        if (klog_report_record(&rec))
            total++;
        if (strcmp(rec.facility, "PS2") == 0) {
            ps2_any = 1;
            if (strstr(rec.msg, "[PS2] module init ok"))
                ps2_ok = 1;
            if (strstr(rec.msg, "[PS2] controller init failed"))
                ps2_fail = 1;
        }
        if (strstr(rec.msg, "[XHCI] FATAL")) {
            fatal_seen = 1;
            if (!fatal_identity[0] && strstr(rec.msg, "[XHCI] FATAL identity"))
                snprintf(fatal_identity, sizeof(fatal_identity), "%s", rec.msg);
        }
    }

    const unsigned max_lines = 48;
    unsigned skip = total > max_lines ? total - max_lines : 0;

    /* Assemble the whole report before touching the console.  Emitting it one
     * line at a time lets the shell's echo, or another CPU, land in the middle
     * of the block -- exactly the "o" in the middle of the PS/2 lines seen in
     * the first run.  One klog_write_raw() holds the console lock for the
     * complete block. */
    static char report[16384];
    size_t used = 0;
#define REPORT_APPEND(...)                                                   \
    do {                                                                     \
        if (used < sizeof(report)) {                                         \
            int n = snprintf(report + used, sizeof(report) - used,           \
                             __VA_ARGS__);                                   \
            if (n > 0)                                                       \
                used += (size_t)n < sizeof(report) - used                    \
                            ? (size_t)n : sizeof(report) - used - 1;         \
        }                                                                    \
    } while (0)

    REPORT_APPEND("\n===== A20OS BOOT REPORT =====\n");
    REPORT_APPEND("boot: %s\n", klog_boot_id());
    if (klog_disk_ready() && klog_disk_dir()[0])
        REPORT_APPEND("log: persistent at %s\n", klog_disk_dir());
    else
        REPORT_APPEND("log: NOT persistent (no writable log surface)\n");
    REPORT_APPEND("showing last %u of %u diagnostic records\n",
                  total > max_lines ? max_lines : total, total);

    unsigned shown = 0;
    for (uint32_t cursor = first; cursor < end; ) {
        if (!klog_record_read(&cursor, &rec))
            break;
        if (!klog_report_record(&rec))
            continue;
        if (shown++ < skip)
            continue;
        REPORT_APPEND("%s", rec.msg);
        if (rec.msg_len && rec.msg[rec.msg_len - 1] != '\n')
            REPORT_APPEND("\n");
    }
    REPORT_APPEND("summary: log=%s ps2=%s xhci_fatal=%s\n",
                  klog_disk_ready() ? "persistent" : "none",
                  ps2_ok ? "driver-ok" :
                  ps2_fail ? "driver-fail" :
                  ps2_any ? "board-only" : "no-records",
                  fatal_seen ? "yes" : "no");
    if (fatal_identity[0]) {
        REPORT_APPEND("key: %s", fatal_identity);
        if (fatal_identity[strlen(fatal_identity) - 1] != '\n')
            REPORT_APPEND("\n");
    }
    REPORT_APPEND("===== END BOOT REPORT =====\n");
#undef REPORT_APPEND
    klog_write_raw(report, used);
}

int klog_has_fatal(void) {
    uint32_t cursor = klog_record_oldest();
    uint32_t end = klog_record_next();
    klog_record_t rec;
    while (cursor < end) {
        if (!klog_record_read(&cursor, &rec))
            break;
        if (strstr(rec.msg, "[XHCI] FATAL") ||
            strstr(rec.msg, "controller disabled after command timeout"))
            return 1;
    }
    return 0;
}

static const char *klog_arg_value(const char *args, const char *key) {
    if (!args || !key)
        return NULL;
    size_t klen = strlen(key);
    const char *p = args;
    while ((p = strstr(p, key))) {
        /* The key carries its trailing '=', so only the token start needs a
         * boundary; the value follows immediately. */
        if (p == args || p[-1] == ' ')
            return p + klen;
        p += klen;
    }
    return NULL;
}

static int klog_parse_bool(const char *value, int fallback) {
    if (!value || !*value)
        return fallback;
    return value[0] == '0' ? 0 : 1;
}

static uint64_t klog_parse_u64(const char *value, uint64_t fallback) {
    if (!value || !*value)
        return fallback;
    uint64_t out = 0;
    for (const char *p = value; *p >= '0' && *p <= '9'; p++)
        out = out * 10U + (uint64_t)(*p - '0');
    return out ? out : fallback;
}

void klog_configure(void) {
    const char *args = bootargs_get();
    if (!args)
        return;

    const char *v = klog_arg_value(args, "a20.log.level=");
    if (v) {
        if (strncmp(v, "debug", 5) == 0) klog_level = KLOG_DEBUG;
        else if (strncmp(v, "info", 4) == 0) klog_level = KLOG_INFO;
        else if (strncmp(v, "warn", 4) == 0) klog_level = KLOG_WARN;
        else if (strncmp(v, "err", 3) == 0) klog_level = KLOG_ERR;
    }
    v = klog_arg_value(args, "a20.log.console=");
    if (v) klog_runtime_console = klog_parse_bool(v, klog_runtime_console);
    v = klog_arg_value(args, "a20.log.persistent=");
    if (v) klog_runtime_persistent = klog_parse_bool(v, klog_runtime_persistent);
    v = klog_arg_value(args, "a20.log.max_bytes=");
    if (v) klog_runtime_max_bytes = klog_parse_u64(v, klog_runtime_max_bytes);
    v = klog_arg_value(args, "a20.log.mode=");
    if (v) {
        if (strncmp(v, "auto", 4) == 0) klog_runtime_mode = KLOG_MODE_AUTO;
        else if (strncmp(v, "partition", 9) == 0) klog_runtime_mode = KLOG_MODE_PARTITION;
        else if (strncmp(v, "rootfs", 6) == 0) klog_runtime_mode = KLOG_MODE_ROOTFS;
        else if (strncmp(v, "ram", 3) == 0) klog_runtime_mode = KLOG_MODE_RAM;
        else if (strncmp(v, "esp", 3) == 0) klog_runtime_mode = KLOG_MODE_ESP;
    }
    v = klog_arg_value(args, "a20.log=");
    if (v && v[0] == '0') {
        klog_runtime_console = 0;
        klog_runtime_persistent = 0;
    }
    if (!CONFIG_KLOG_ENABLED) {
        klog_runtime_console = 0;
        klog_runtime_persistent = 0;
    }
}

int klog_console_enabled(void) { return klog_runtime_console; }
int klog_persistent_enabled(void) {
    return CONFIG_KLOG_ENABLED && klog_runtime_persistent;
}
int klog_mode(void) { return klog_runtime_mode; }
uint64_t klog_max_bytes(void) { return klog_runtime_max_bytes; }

/* Link-time fallbacks for profiles that do not compile the persistent writer
 * (MCU, no block stack).  A strong definition in kernel/core/klog_disk.c
 * overrides these.  Keeping them here means boot_advance() and the shutdown
 * paths need no #ifdef of their own. */
__attribute__((weak)) int klog_disk_attach(const char *dir) { (void)dir; return -EOPNOTSUPP; }
__attribute__((weak)) int klog_disk_ready(void) { return 0; }
__attribute__((weak)) const char *klog_disk_dir(void) { return ""; }
__attribute__((weak)) void klog_disk_sync(void) {}
__attribute__((weak)) void klog_disk_poll(void) {}
__attribute__((weak)) void klog_disk_panic(void) {}
__attribute__((weak)) void klog_disk_mark_ready(void) {}
__attribute__((weak)) void klog_disk_mark_clean(void) {}
__attribute__((weak)) void klog_disk_thread(void) {}
