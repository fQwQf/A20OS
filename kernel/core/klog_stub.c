#include "core/klog.h"
#include "core/stdio.h"
#include "core/lock.h"
#include "core/bootargs.h"
#include "core/string.h"
/* printf/panic still need a console when the logging subsystem is omitted. */
int klog_level = CONFIG_KLOG_DEFAULT_LEVEL;
spinlock_t klog_console_lock = SPINLOCK_INIT;
static int console = CONFIG_KLOG_CONSOLE;
extern void console_emit_locked(const char *, size_t);
#if CONFIG_A20_DIAG_BOOT || CONFIG_A20_TRACE
#include "core/random.h"
static char diagnostic_boot_id[33];
void klog_init(void) {
    spin_init(&klog_console_lock);
    uint8_t bytes[16];
    random_fill(bytes, sizeof(bytes));
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof(bytes); i++) {
        diagnostic_boot_id[2*i] = hex[bytes[i] >> 4];
        diagnostic_boot_id[2*i+1] = hex[bytes[i] & 15];
    }
}
#else
void klog_init(void) { spin_init(&klog_console_lock); }
#endif
void klog_configure(void) {
    const char *args = bootargs_get();
    if (!args) return;
    const char *keys[] = {"a20.log.console=0", "a20.log=0"};
    for (unsigned i = 0; i < 2; i++) {
        const char *p = args;
        size_t length = strlen(keys[i]);
        while ((p = strstr(p, keys[i]))) {
            if ((p == args || p[-1] == ' ') && (!p[length] || p[length] == ' ')) console = 0;
            p += length;
        }
    }
}
int klog_console_enabled(void) { return console; }
void klog_emit_text(int level, const char *msg, size_t length) {
    (void)level;
    uint64_t flags;
    if (console && spin_trylock_irqsave(&klog_console_lock, &flags)) {
        console_emit_locked(msg, length);
        spin_unlock_irqrestore(&klog_console_lock, flags);
    }
}
void klog_write(const char *format, ...) { (void)format; }
int klog_write_ex(int level, const char *format, ...) { (void)level; (void)format; return 0; }
int klog_record_only(int level, const char *format, ...) { (void)level; (void)format; return 0; }
void klog_write_raw(const char *buf, size_t length) { klog_emit_text(KLOG_INFO, buf, length); }
int klog_read(char *buf, size_t size, size_t *pos) { (void)buf; (void)size; (void)pos; return 0; }
void klog_clear(void) {}
size_t klog_len(void) { return 0; }
const char *klog_boot_id(void) {
#if CONFIG_A20_DIAG_BOOT || CONFIG_A20_TRACE
    return diagnostic_boot_id;
#else
    return "";
#endif
}
int klog_persistent_enabled(void) { return 0; }
int klog_mode(void) { return KLOG_MODE_AUTO; }
uint64_t klog_max_bytes(void) { return 0; }
uint32_t klog_record_oldest(void) { return 0; }
uint32_t klog_record_next(void) { return 0; }
int klog_record_read(uint32_t *cursor, klog_record_t *out) { (void)cursor; (void)out; return 0; }
int klog_has_errors(void) { return 0; }

__attribute__((weak)) int klog_disk_attach(const char *dir) { (void)dir; return -EOPNOTSUPP; }
__attribute__((weak)) int klog_disk_ready(void) { return 0; }
__attribute__((weak)) const char *klog_disk_storage(void) { return "none"; }
__attribute__((weak)) const char *klog_disk_health(void) { return "unavailable"; }
__attribute__((weak)) const char *klog_disk_dir(void) { return ""; }
__attribute__((weak)) void klog_disk_sync(void) {}
__attribute__((weak)) void klog_disk_poll(void) {}
__attribute__((weak)) void klog_disk_panic(void) {}
__attribute__((weak)) void klog_disk_mark_ready(void) {}
__attribute__((weak)) void klog_disk_mark_clean(void) {}
__attribute__((weak)) void klog_disk_thread(void) {}
