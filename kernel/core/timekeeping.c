#include "core/timekeeping.h"
#include "core/defs.h"
#include "core/timer.h"
#include "core/lock.h"
#include "core/klog.h"
#include "mm/vdso.h"
#include "build_time.h"

static uint64_t g_boot_ticks;
static uint64_t g_boot_cycles;
static uint64_t g_realtime_base_ticks;
static uint64_t g_realtime_base_cycles;
static uint64_t g_realtime_base_sec;
static uint64_t g_realtime_base_nsec;
/* Bumped on every discontinuous realtime-clock set; timerfd
 * TFD_TIMER_CANCEL_ON_SET compares against the generation at arm time. */
static uint64_t g_realtime_set_gen;
/* Writer-writer mutual exclusion only.  Readers take no lock: the realtime
 * anchor is guarded by a seqlock (g_rt_seq odd while a writer is inside),
 * so every clock_gettime/gettimeofday in the kernel skips the lock convoy
 * the previous spinlock created on multi-core.  On 32-bit targets the u64
 * fields can tear, so readers additionally re-read the trio and retry on
 * any mismatch — a torn read can never be accepted. */
static volatile uint32_t g_rt_seq;
static spinlock_t g_timekeeping_lock = SPINLOCK_INIT;

#if defined(CONFIG_X86_64)
/* Wall clock source on x86_64.  Every other architecture seeds the wall clock
 * from the build-time constant and never consults hardware; only x86_64 has a
 * readable RTC, and only it can end up here.
 *
 * The CMOS RTC (MC146818) is reached through a loadable cmos-rtc.a20drv
 * package bound to the board's platform device, so it cannot be read at
 * timekeeping_init() time: this function runs before driver_core_init() and
 * long before the early DriverStore is activated in init_kthread.  The seed
 * therefore still starts from the build timestamp and the driver replaces it
 * through timekeeping_wallclock_set_hw() when it binds.  That is why the
 * fallback below is announced rather than silent: a boot that ends up with no
 * RTC bound is indistinguishable, in the wall clock, from a boot that kept the
 * seed, and only this line plus the absence of the driver's own line tells
 * them apart. */
static int g_wallclock_from_hw;

void timekeeping_wallclock_set_hw(uint64_t sec)
{
    __atomic_store_n(&g_wallclock_from_hw, 1, __ATOMIC_RELEASE);
    timekeeping_set_realtime(sec, 0);
    klog_write("[TIME] wallclock: hardware RTC adopted, unix=%llu\n",
               (unsigned long long)sec);
}

int timekeeping_wallclock_from_hw(void)
{
    return __atomic_load_n(&g_wallclock_from_hw, __ATOMIC_ACQUIRE);
}
#endif

static void ticks_to_timespec(uint64_t ticks, uint64_t ts[2]) {
    ts[0] = ticks / TICKS_PER_SEC;
    ts[1] = (ticks % TICKS_PER_SEC) * 1000000000ULL / TICKS_PER_SEC;
}

void timekeeping_init(void) {
    g_boot_ticks = timer_get_ticks();
    g_boot_cycles = arch_vdso_counter();
#if defined(CONFIG_X86_64)
    klog_write("[TIME] wallclock: no RTC readable yet, seed from build time "
               "unix=%llu (the CMOS RTC driver replaces it when it binds)\n",
               (unsigned long long)A20_BUILD_UNIX_TIME);
#endif
    timekeeping_set_realtime(A20_BUILD_UNIX_TIME, 0);
}

/* The vDSO image/vvar need the frame allocator, so they are set up after
 * mm_init rather than here (kernel/main.c call order). */
void timekeeping_vdso_init(void) {
    vdso_init(g_boot_cycles, arch_vdso_counter_freq());
    vdso_sync_realtime(g_realtime_base_sec, g_realtime_base_nsec,
                       g_realtime_base_cycles);
}

void timekeeping_get_monotonic(uint64_t ts[2]) {
    ticks_to_timespec(timer_get_ticks() - g_boot_ticks, ts);
}

void timekeeping_get_realtime(uint64_t ts[2]) {
    uint32_t seq;
    uint64_t base_ticks, base_sec, base_nsec;
    do {
        seq = __atomic_load_n(&g_rt_seq, __ATOMIC_ACQUIRE);
        base_ticks = __atomic_load_n(&g_realtime_base_ticks, __ATOMIC_RELAXED);
        base_sec   = __atomic_load_n(&g_realtime_base_sec, __ATOMIC_RELAXED);
        base_nsec  = __atomic_load_n(&g_realtime_base_nsec, __ATOMIC_RELAXED);
    } while ((seq & 1u) ||
             seq != __atomic_load_n(&g_rt_seq, __ATOMIC_ACQUIRE) ||
             base_ticks != __atomic_load_n(&g_realtime_base_ticks,
                                           __ATOMIC_RELAXED) ||
             base_sec   != __atomic_load_n(&g_realtime_base_sec,
                                           __ATOMIC_RELAXED) ||
             base_nsec  != __atomic_load_n(&g_realtime_base_nsec,
                                           __ATOMIC_RELAXED));

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
    __atomic_add_fetch(&g_rt_seq, 1, __ATOMIC_ACQUIRE);
    g_realtime_base_ticks = timer_get_ticks();
    g_realtime_base_cycles = arch_vdso_counter();
    g_realtime_base_sec = sec;
    g_realtime_base_nsec = nsec;
    g_realtime_set_gen++;
    __atomic_add_fetch(&g_rt_seq, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_timekeeping_lock, flags);
    /* Keep the vDSO realtime anchor in sync (seqlock on the reader side);
     * pass the recorded cycle so both paths agree bit for bit. */
    vdso_sync_realtime(sec, nsec, g_realtime_base_cycles);
    return 0;
}

uint64_t timekeeping_realtime_set_generation(void) {
    return __atomic_load_n(&g_realtime_set_gen, __ATOMIC_RELAXED);
}
