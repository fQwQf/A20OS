#include "core/timekeeping.h"
#include "core/defs.h"
#include "core/timer.h"
#include "core/lock.h"
#include "build_time.h"

static uint64_t g_boot_ticks;
static uint64_t g_realtime_base_ticks;
static uint64_t g_realtime_base_sec;
static uint64_t g_realtime_base_nsec;
static spinlock_t g_timekeeping_lock = SPINLOCK_INIT;

static void ticks_to_timespec(uint64_t ticks, uint64_t ts[2]) {
    ts[0] = ticks / TICKS_PER_SEC;
    ts[1] = (ticks % TICKS_PER_SEC) * 1000000000ULL / TICKS_PER_SEC;
}

void timekeeping_init(void) {
    g_boot_ticks = timer_get_ticks();
    timekeeping_set_realtime(A20_BUILD_UNIX_TIME, 0);
}

void timekeeping_get_monotonic(uint64_t ts[2]) {
    ticks_to_timespec(timer_get_ticks() - g_boot_ticks, ts);
}

void timekeeping_get_realtime(uint64_t ts[2]) {
    uint64_t flags = spin_lock_irqsave(&g_timekeeping_lock);
    uint64_t base_ticks = g_realtime_base_ticks;
    uint64_t base_sec = g_realtime_base_sec;
    uint64_t base_nsec = g_realtime_base_nsec;
    spin_unlock_irqrestore(&g_timekeeping_lock, flags);

    uint64_t delta[2];
    ticks_to_timespec(timer_get_ticks() - base_ticks, delta);
    ts[0] = base_sec + delta[0];
    ts[1] = base_nsec + delta[1];
    if (ts[1] >= 1000000000ULL) {
        ts[0] += ts[1] / 1000000000ULL;
        ts[1] %= 1000000000ULL;
    }
}

int timekeeping_set_realtime(uint64_t sec, uint64_t nsec) {
    if (nsec >= 1000000000ULL) {
        sec += nsec / 1000000000ULL;
        nsec %= 1000000000ULL;
    }
    uint64_t flags = spin_lock_irqsave(&g_timekeeping_lock);
    g_realtime_base_ticks = timer_get_ticks();
    g_realtime_base_sec = sec;
    g_realtime_base_nsec = nsec;
    spin_unlock_irqrestore(&g_timekeeping_lock, flags);
    return 0;
}
