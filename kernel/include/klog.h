#ifndef _KLOG_H
#define _KLOG_H

#include "types.h"

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

#endif /* _KLOG_H */
