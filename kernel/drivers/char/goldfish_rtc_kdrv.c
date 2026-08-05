/*
 * goldfish_rtc_kdrv — kernel placement of the dual-placement goldfish RTC
 * driver (docs/hybrid-kernel/04-dual-placement.md).
 *
 * The device protocol is shared verbatim with the user-space driver
 * (user/svc/rtcd.c) via kernel/include/drivers/dual/goldfish_rtc.h; only
 * the shell differs.  This kernel shell currently provides a boot-time
 * probe/read (proving kernel-mode runtime of the same source); wiring the
 * shared core into the kernel timekeeping/alarm path is the next step and
 * must not happen while the udriver window is also user-owned by default.
 */
#define DRV_ENV_KERNEL 1
#include "drivers/dual/goldfish_rtc.h"
#include "drivers/char/goldfish_rtc_kdrv.h"
#include "core/klog.h"

static uint64_t g_grtc_base;

int goldfish_rtc_kdrv_probe(void)
{
    g_grtc_base = grtc_map();
    if (!g_grtc_base)
        return -1;
    uint64_t ns = grtc_read_ns(g_grtc_base);
    printf("[GRTC] kernel-placement probe: now=%lu ns\n",
           (unsigned long)ns);
    return 0;
}

uint64_t goldfish_rtc_kdrv_read_ns(void)
{
    if (!g_grtc_base)
        return 0;
    return grtc_read_ns(g_grtc_base);
}
