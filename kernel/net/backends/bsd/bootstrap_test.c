/* Dedicated bootstrap diagnostic. It halts before user init and does not claim
 * to provide a selectable BSD NIC/socket backend or Internet connectivity. */
#include "core/stdio.h"
#include "core/panic.h"
#include "proc/proc.h"
#include "runtime/memory.h"
extern void *a20_bsd_port_tls_create(void);
extern int a20_bsd_port_tls_bind(void *);
extern int a20_bsd_port_boot_config(size_t);
extern int a20_bsd_ff_freebsd_init(void);
extern int a20_bsd_port_loopback_probe(void);
extern void a20_bsd_port_memory_snapshot(struct bsd_memory_stats *);
void a20_bsd_bootstrap_test(void)
{
    void *context = a20_bsd_port_tls_create();
    if (!context || a20_bsd_port_tls_bind(context)) panic("BSD bootstrap TLS binding failed");
    struct bsd_memory_stats memory;
    a20_bsd_port_memory_snapshot(&memory);
    if (a20_bsd_port_boot_config(memory.limit)) panic("BSD bootstrap memory configuration failed");
    printf("BSD_BOOTSTRAP_KVM_BEGIN pid=%d\n", proc_current()->pid);
    int result = a20_bsd_ff_freebsd_init();
    if (result) panic("BSD SYSINIT failed: %d", result);
    result = a20_bsd_port_loopback_probe();
    if (result) panic("BSD loopback failed: %d", result);
    a20_bsd_port_memory_snapshot(&memory);
    printf("BSD_BOOTSTRAP_KVM_PASS backing=%lu peak=%lu limit=%lu\n",
           (unsigned long)memory.used, (unsigned long)memory.peak, (unsigned long)memory.limit);
    /* The initialized BSD domain owns its UMA caches and pending callouts for
     * the remaining diagnostic lifetime. No attempt to unload a live stack. */
    for (;;) proc_yield();
}
