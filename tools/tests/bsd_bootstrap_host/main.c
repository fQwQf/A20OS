/* Host substrate for executing the actual isolated BSD initialization image.
 * This does not supply any BSD socket/sysinit/UMA implementation. */
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/random.h>
#include <time.h>

extern void *port_host_alloc(size_t);
extern void port_host_free(void *);
extern void *port_host_current_task(void);
void *kmalloc_atomic(size_t n) { return port_host_alloc(n); }
void kfree(void *p) { port_host_free(p); }
void *proc_current(void) { return port_host_current_task(); }
__attribute__((noreturn)) void panic(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    abort();
}
void klog_write(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stdout, format, args);
    va_end(args);
}
void random_fill(void *destination, size_t length)
{
    unsigned char *p = destination;
    while (length) {
        ssize_t n = getrandom(p, length, 0);
        if (n < 0 && errno == EINTR) continue;
        assert(n > 0);
        p += n; length -= (size_t)n;
    }
}
static void clock_read(clockid_t clock, uint64_t out[2])
{
    struct timespec ts;
    assert(!clock_gettime(clock, &ts));
    out[0] = ts.tv_sec; out[1] = ts.tv_nsec;
}
void timekeeping_get_monotonic(uint64_t out[2]) { clock_read(CLOCK_MONOTONIC, out); }
void timekeeping_get_realtime(uint64_t out[2]) { clock_read(CLOCK_REALTIME, out); }
extern void *a20_bsd_port_tls_create(void);
extern int a20_bsd_port_tls_bind(void *);
extern int a20_bsd_port_boot_config(size_t);
extern int a20_bsd_ff_freebsd_init(void);
extern int a20_bsd_port_loopback_probe(void);
int main(void)
{
    setbuf(stdout, NULL);
    void *tls = a20_bsd_port_tls_create();
    assert(tls && !a20_bsd_port_tls_bind(tls));
    assert(!a20_bsd_port_boot_config(64UL * 1024 * 1024));
    puts("BSD_BOOTSTRAP_BEGIN");
    int result = a20_bsd_ff_freebsd_init();
    printf("BSD_BOOTSTRAP_RETURN %d\n", result);
    assert(result == 0);
    int loop = a20_bsd_port_loopback_probe();
    printf("BSD_LOOPBACK_RETURN %d\n", loop);
    assert(loop == 0);
    puts("BSD_BOOTSTRAP_HOST_PASS");
    return 0;
}
