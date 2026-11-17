#ifndef A20_BOOT_REPORT_H
#define A20_BOOT_REPORT_H
#include "core/types.h"
#ifndef CONFIG_BOOT_REPORT
#define CONFIG_BOOT_REPORT 0
#endif
#define KLOG_REPORT_OFF     0
#define KLOG_REPORT_ERROR   1
#define KLOG_REPORT_ALWAYS  2
#ifndef CONFIG_KLOG_REPORT
#define CONFIG_KLOG_REPORT KLOG_REPORT_OFF
#endif
#ifndef CONFIG_KLOG_REPORT_WINDOW_MS
#define CONFIG_KLOG_REPORT_WINDOW_MS 10000
#endif
#ifndef CONFIG_KLOG_REPORT_SETTLE_MS
#define CONFIG_KLOG_REPORT_SETTLE_MS 500
#endif
#ifndef CONFIG_KLOG_REPORT_MAX_RECORDS
#define CONFIG_KLOG_REPORT_MAX_RECORDS 48
#endif
#ifndef CONFIG_KLOG_REPORT_CONTEXT
#define CONFIG_KLOG_REPORT_CONTEXT 1
#endif
/* Keep formatting within the fixed procfs snapshot capacity on every board. */
#define KLOG_REPORT_RECORDS 48U
#if CONFIG_KLOG_REPORT_WINDOW_MS < 0 || CONFIG_KLOG_REPORT_WINDOW_MS > 120000
#error "LOG_REPORT_WINDOW_MS must be 0..120000"
#endif
#if CONFIG_KLOG_REPORT_SETTLE_MS < 0 || CONFIG_KLOG_REPORT_SETTLE_MS > 10000
#error "LOG_REPORT_SETTLE_MS must be 0..10000"
#endif
#if CONFIG_KLOG_REPORT_MAX_RECORDS < 0 || CONFIG_KLOG_REPORT_MAX_RECORDS > KLOG_REPORT_RECORDS
#error "LOG_REPORT_MAX_RECORDS must be 0..48"
#endif
#if CONFIG_KLOG_REPORT_CONTEXT != 0 && CONFIG_KLOG_REPORT_CONTEXT != 1
#error "LOG_REPORT_CONTEXT must be 0|1"
#endif
#define KLOG_BOOT_REPORT_SIZE (16U * 1024U)


#if CONFIG_BOOT_REPORT
void boot_report_configure(void);
void boot_report_init_prepared(const char *path, int pid);
void boot_report_start(void);
int boot_report_format(char *buf, size_t capacity);
#else
static inline void boot_report_configure(void) {}
static inline void boot_report_init_prepared(const char *path, int pid) { (void)path; (void)pid; }
static inline void boot_report_start(void) {}
#endif
#endif
