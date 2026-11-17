#include "core/boot_report.h"
#include "core/report.h"
#include "core/diagnostics.h"
#include "core/klog.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/lock.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "core/bootargs.h"
#include "core/version.h"
#include "boot/context.h"
#include "drivers/core/driver_core.h"
#include "proc/proc.h"
#include "mm/slab.h"
static int klog_runtime_report = CONFIG_KLOG_REPORT;
static unsigned klog_runtime_report_window_ms = CONFIG_KLOG_REPORT_WINDOW_MS;
static unsigned klog_runtime_report_settle_ms = CONFIG_KLOG_REPORT_SETTLE_MS;
static unsigned klog_runtime_report_max_records = CONFIG_KLOG_REPORT_MAX_RECORDS;
static unsigned klog_runtime_report_context = CONFIG_KLOG_REPORT_CONTEXT;
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

/* Bounded decimal tokens: accept zero, reject signs, suffixes and overflow.
 * Invalid runtime overrides preserve the configured build-time default. */
static unsigned klog_parse_report_uint(const char *value, unsigned fallback, unsigned limit) {
    if (!value || *value < '0' || *value > '9') return fallback;
    unsigned out = 0;
    const char *p = value;
    for (; *p >= '0' && *p <= '9'; p++) {
        unsigned digit = (unsigned)(*p - '0');
        if (out > limit / 10 || (out == limit / 10 && digit > limit % 10))
            return fallback;
        out = out * 10 + digit;
    }
    return !*p || *p == ' ' ? out : fallback;
}


void boot_report_configure(void) {
    const char *args = bootargs_get();
    if (!args) return;
    const char *v = klog_arg_value(args, "a20.log.report=");
    if (v) {
        if (strncmp(v, "off", 3) == 0 && (!v[3] || v[3] == ' ')) klog_runtime_report = KLOG_REPORT_OFF;
        else if (strncmp(v, "error", 5) == 0 && (!v[5] || v[5] == ' ')) klog_runtime_report = KLOG_REPORT_ERROR;
        else if (strncmp(v, "always", 6) == 0 && (!v[6] || v[6] == ' ')) klog_runtime_report = KLOG_REPORT_ALWAYS;
    }
    klog_runtime_report_window_ms = klog_parse_report_uint(
        klog_arg_value(args, "a20.log.report_window_ms="), klog_runtime_report_window_ms, 120000);
    klog_runtime_report_settle_ms = klog_parse_report_uint(
        klog_arg_value(args, "a20.log.report_settle_ms="), klog_runtime_report_settle_ms, 10000);
    klog_runtime_report_max_records = klog_parse_report_uint(
        klog_arg_value(args, "a20.log.report_max_records="), klog_runtime_report_max_records, KLOG_REPORT_RECORDS);
    klog_runtime_report_context = klog_parse_report_uint(
        klog_arg_value(args, "a20.log.report_context="), klog_runtime_report_context, 1);

}
static int boot_report_mode(void) { return klog_runtime_report; }
static unsigned boot_report_window_ms(void) { return klog_runtime_report_window_ms; }
static unsigned boot_report_settle_ms(void) { return klog_runtime_report_settle_ms; }
static char init_path[128];
static int init_pid;
static spinlock_t state_lock = SPINLOCK_INIT;
void boot_report_init_prepared(const char *path, int pid) {
    uint64_t flags = spin_lock_irqsave(&state_lock);
    snprintf(init_path, sizeof(init_path), "%s", path ? path : "unknown");
    init_pid = pid;
    spin_unlock_irqrestore(&state_lock, flags);
}
typedef struct boot_snapshot {
    klog_snapshot_t log;
    char init_path[128];
    int init_pid;
    klog_record_t records[];
} boot_snapshot_t;
#define REPORT_APPEND(...) report_printf(writer, __VA_ARGS__)
static int boot_section(report_writer_t *writer, const void *data) {
    const boot_snapshot_t *snap = data;
    uint64_t mono[2];
    timekeeping_get_monotonic(mono);
    const boot_context_t *boot = boot_context_get();
    static const char *const phases[] = {"reset", "handoff", "early", "memory", "devices", "ready"};
    static const char *const protocols[] = {"static", "fdt", "multiboot", "uefi"};
    REPORT_APPEND("schema: 1 (live snapshot)\nboot: %s\n", klog_boot_id());
    static const char *const modes[] = {"off", "error", "always"};
    REPORT_APPEND("policy: report=%s window_ms=%u settle_ms=%u max_records=%u context=%s\n",
                  modes[boot_report_mode()], boot_report_window_ms(), boot_report_settle_ms(),
                  klog_runtime_report_max_records, klog_runtime_report_context ? "on" : "off");
    REPORT_APPEND("kernel: %s %s\nboard: %s\n", A20OS_SYSNAME, A20OS_VERSION,
                  current_board && current_board->name ? current_board->name : "unknown");
    REPORT_APPEND("uptime: %llu.%03llu s kernel_phase=%s handoff=%s\n",
                  (unsigned long long)mono[0], (unsigned long long)(mono[1] / 1000000),
                  boot && (unsigned)boot->phase < 6 ? phases[boot->phase] : "unknown",
                  boot && (unsigned)boot->protocol < 4 ? protocols[boot->protocol] : "unknown");
    if (snap->init_pid)
        REPORT_APPEND("init: prepared pid=%d image=%s (application readiness not inferred)\n",
                      snap->init_pid, snap->init_path);
    else REPORT_APPEND("init: not prepared\n");
    return 0;
}
static int storage_section(report_writer_t *writer, const void *data) {
    (void)data;
    const char *storage = klog_disk_storage();
    const char *directory = klog_disk_dir();
    REPORT_APPEND("log: %s%s%s\n", storage, directory[0] ? " at " : "", directory);
    if (strcmp(storage, "ram") == 0)
        REPORT_APPEND("retention: volatile; lost on reboot or power loss\n");
    REPORT_APPEND("writer: %s\n", klog_disk_health());
    return 0;
}
static int records_section(report_writer_t *writer, const void *data) {
    const boot_snapshot_t *snap = data;
    REPORT_APPEND("records: retained=%u seq=[%u,%u) overwritten=%u\n",
                  snap->log.retained, snap->log.first, snap->log.end, snap->log.first);
    REPORT_APPEND("diagnostics: shown=%u matching=%u omitted=%u (errors prioritized)\n",
                  snap->log.shown, snap->log.matching, snap->log.matching - snap->log.shown);
    for (unsigned i = 0; i < snap->log.shown; i++) {
        const klog_record_t *rec = &snap->records[i];
        REPORT_APPEND("[%llu.%03llu] %s %s: %s",
                      (unsigned long long)(rec->mono_ns / 1000000000ULL),
                      (unsigned long long)((rec->mono_ns / 1000000ULL) % 1000),
                      rec->level >= KLOG_ERR ? "error" : rec->level >= KLOG_WARN ? "warn" : "info",
                      rec->facility, rec->msg);
        if (!rec->msg_len || rec->msg[rec->msg_len - 1] != '\n') REPORT_APPEND("\n");
    }
    REPORT_APPEND("summary: log=%s warnings=%u errors=%u (retained records only)\n",
                  klog_disk_storage(), snap->log.warnings, snap->log.errors);
    return 0;
}
#undef REPORT_APPEND
/* Append a provider here (and select its source in the Makefile when optional).
 * Providers own formatting; collectors own their locking and snapshots. */
#if CONFIG_A20_DIAG_BOOT
static int porting_section(report_writer_t *writer, const void *data) {
    (void)data;
    return a20_diag_summary(writer);
}
#endif
static const report_section_t sections[] = {
#if CONFIG_A20_DIAG_BOOT
    {"porting", porting_section},
#endif
    {"boot", boot_section}, {"storage", storage_section}, {"records", records_section},
};
int boot_report_format(char *buf, size_t capacity) {
    if (!buf || capacity < KLOG_BOOT_REPORT_SIZE) return -EINVAL;
    size_t bytes = sizeof(boot_snapshot_t) + klog_runtime_report_max_records * sizeof(klog_record_t);
    boot_snapshot_t *snap = kmalloc(bytes);
    if (!snap) return -ENOMEM;
    memset(snap, 0, bytes);
    uint64_t flags = spin_lock_irqsave(&state_lock);
    snap->init_pid = init_pid;
    memcpy(snap->init_path, init_path, sizeof(init_path));
    spin_unlock_irqrestore(&state_lock, flags);
    klog_snapshot_select(&snap->log, snap->records, klog_runtime_report_max_records, klog_runtime_report_context);
    int result;
    for (;;) {
        report_writer_t writer = { .buffer = buf, .capacity = capacity };
        report_printf(&writer, "===== A20OS BOOT REPORT =====\n");
        result = report_sections(&writer, sections, sizeof(sections) / sizeof(sections[0]), snap);
        if (result >= 0) {
            report_printf(&writer, "===== END BOOT REPORT =====\n");
            result = writer.error ? writer.error : (int)writer.length;
        }
        /* Optional providers may consume more of the screen report budget.
         * Keep a complete report with explicit omitted counts; the underlying
         * log and machine-readable provider snapshots remain available. */
        if (result != -ENOSPC || !snap->log.shown) break;
        snap->log.shown /= 2;
    }
    kfree(snap);
    return result;
}
static void boot_report_print(void) {
    char *buf = kmalloc(KLOG_BOOT_REPORT_SIZE);
    if (!buf) return;
    int length = boot_report_format(buf, KLOG_BOOT_REPORT_SIZE);
    if (length > 0) klog_console_replay(buf, (size_t)length);
    kfree(buf);
}
static void boot_report_thread(void) {
    proc_set_name(proc_current(), "bootreport");
    uint64_t deadline = timer_get_ticks() + MS_TO_TICKS(boot_report_window_ms());
    uint64_t step = MS_TO_TICKS(500);
    if (!step) step = 1;
    int errors;
    while (!(errors = klog_has_errors())) {
        uint64_t now = timer_get_ticks();
        if (now >= deadline) break;
        proc_sleep_until(now + (deadline - now < step ? deadline - now : step));
    }
    if (errors && boot_report_settle_ms())
        proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(boot_report_settle_ms()));
    if (boot_report_mode() == KLOG_REPORT_ALWAYS || errors)
        boot_report_print();
    proc_exit(0);
}
void boot_report_start(void) {
    if (boot_report_mode() != KLOG_REPORT_OFF && klog_console_enabled() &&
        proc_alloc(boot_report_thread) < 0)
        kerr("[LOG] failed to spawn boot report thread\n");
}
