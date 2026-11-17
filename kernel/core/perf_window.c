#include "core/perf.h"
#include "core/sync.h"
#include "core/string.h"
#include "core/timer.h"
#include "core/random.h"
#include "core/errno.h"
#include "sys/syscall.h"
#include "proc/proc.h"

uint64_t g_a20_perf_generation;
uint64_t g_a20_perf_excluded_syscalls;
static mutex_t window_lock = MUTEX_INIT;
static a20_perf_window_t window;

void a20_perf_init(void) {
    if (window.boot_token) return;
    random_fill(&window.boot_token, sizeof(window.boot_token));
    if (!window.boot_token) window.boot_token = 1;
}

static void drain_writers(void) {
    for (;;) {
        unsigned pending = 0;
        for (unsigned cpu = 0; cpu < A20_PERF_MAX_CPUS; cpu++)
            pending |= __atomic_load_n(&g_a20_perf_percpu[cpu].writers, __ATOMIC_SEQ_CST);
        if (!pending) return;
        /* The mutex serializes controls, not writers. No IRQ/spin lock is
         * held while allowing an already-entered writer to finish. */
        proc_sleep_until(timer_get_ticks() + 1);
    }
}

static void reset_window(void) {
    drain_writers();
    uint64_t generation = __atomic_load_n(&g_a20_perf_generation, __ATOMIC_RELAXED) + 1;
    if (!generation) generation = 1;
    __atomic_store_n(&g_a20_perf_generation, generation, __ATOMIC_RELEASE);
    a20_perf_reset();
    syscall_profile_reset();
    __atomic_store_n(&g_a20_perf_excluded_syscalls, 0, __ATOMIC_RELAXED);
    window.generation = generation;
    window.start_ticks = window.stop_ticks = window.elapsed_ticks = 0;
    window.valid = 0;
}

int a20_perf_control(const char *command, size_t length) {
    char text[16];
    if (!command || !length || length >= sizeof(text)) return -EINVAL;
    memcpy(text, command, length);
    if (text[length - 1] == '\n') length--;
    if (length && text[length - 1] == '\r') length--;
    text[length] = 0;
    /* Embedded NUL, whitespace suffixes and partial tokens are errors. */
    if (strlen(text) != length) return -EINVAL;
    int start = !strcmp(text, "start") || !strcmp(text, "1");
    int stop = !strcmp(text, "stop") || !strcmp(text, "0");
    int reset = !strcmp(text, "reset");
    if (!start && !stop && !reset) return -EINVAL;
    mutex_lock(&window_lock);
    int result = 0;
    unsigned enabled = __atomic_load_n(&g_a20_perf_enabled, __ATOMIC_ACQUIRE);
    if ((start || reset) && enabled) result = -EBUSY;
    else if (start) {
        reset_window();
        window.start_ticks = timer_get_ticks();
        window.valid = 1;
        __atomic_store_n(&g_a20_perf_enabled, 1, __ATOMIC_SEQ_CST);
    } else if (stop && enabled) {
        __atomic_store_n(&g_a20_perf_enabled, 0, __ATOMIC_SEQ_CST);
        drain_writers();
        window.stop_ticks = timer_get_ticks();
        window.elapsed_ticks = window.stop_ticks >= window.start_ticks ?
                               window.stop_ticks - window.start_ticks : 0;
    } else if (reset) reset_window();
    mutex_unlock(&window_lock);
    return result;
}

void a20_perf_snapshot_begin(a20_perf_window_t *out) {
    mutex_lock(&window_lock);
    *out = window;
    out->enabled = __atomic_load_n(&g_a20_perf_enabled, __ATOMIC_ACQUIRE);
    out->excluded_syscalls = __atomic_load_n(&g_a20_perf_excluded_syscalls, __ATOMIC_RELAXED);
    if (out->enabled) {
        uint64_t now = timer_get_ticks();
        out->elapsed_ticks = now >= out->start_ticks ? now - out->start_ticks : 0;
    }
}
void a20_perf_snapshot_end(void) { mutex_unlock(&window_lock); }
