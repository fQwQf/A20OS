#include "proc/posix_timer.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "proc/signal.h"

/*
 * POSIX interval-timer table — the C side.
 *
 * The two tables and every transition on them live in
 * kernel/proc/posix_timer_state.rs.  This file supplies what Rust cannot do
 * itself: the tick source, the wall/monotonic clock reads, the scheduler
 * deadline publication, task references, the task metric reads and signal
 * delivery.
 */

extern int a20_posix_timer_clock_supported(int clockid);
extern int a20_posix_timer_create(int owner_pid, int signo, int target_tid,
                                  int clockid);
extern int a20_posix_timer_delete(int owner_pid, int id);
extern int a20_posix_timer_get_time(int owner_pid, int id, uint64_t out[4]);
extern int a20_posix_timer_set_time(int owner_pid, int id, const uint64_t ts[4],
                                    int flags);
extern int a20_posix_timer_getoverrun(int owner_pid, int id);
extern void a20_posix_timer_tick(void);
extern int a20_posix_itimer_set(void *task, int which, uint64_t value_ticks,
                                uint64_t interval_ticks);
extern int a20_posix_itimer_get(void *task, int which, uint64_t out[2]);
extern void a20_posix_itimer_cpu_tick(void *cur);

uint64_t a20_posix_timer_ticks_per_sec(void) {
    return TICKS_PER_SEC;
}

uint64_t a20_posix_timer_now_ticks(void) {
    return timer_get_ticks();
}

void a20_posix_timer_now_wall(uint64_t ts[2]) {
    timekeeping_get_realtime(ts);
}

void a20_posix_timer_now_monotonic(uint64_t ts[2]) {
    timekeeping_get_monotonic(ts);
}

void a20_posix_timer_set_deadline(uint64_t next) {
    sched_set_posix_deadline(next);
}

void *a20_posix_timer_task_ref(int pid) {
    return proc_find_get(pid);
}

void *a20_posix_timer_task_clone_ref(void *task) {
    return proc_get((task_t *)task);
}

void a20_posix_timer_task_put(void *task) {
    proc_put((task_t *)task);
}

int a20_posix_timer_task_gone(void *task) {
    task_t *t = task;
    int state = __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
    return state == PROC_UNUSED || state == PROC_ZOMBIE;
}

uint64_t a20_posix_timer_task_metric(void *task, int which) {
    task_t *t = task;
    /* ITIMER_VIRTUAL counts user ticks, ITIMER_PROF user + system. */
    return which == 0 ? t->utime_ticks : t->utime_ticks + t->stime_ticks;
}

int a20_posix_timer_signal_pending(void *task, int signo, int thread_scoped) {
    return signal_signo_pending_scoped(task, signo, thread_scoped);
}

void a20_posix_timer_send(int owner_pid, int signo) {
    (void)signal_send(owner_pid, signo);
}

void a20_posix_timer_send_task(void *task, int signo) {
    (void)signal_send_task(task, signo);
}

int posix_timer_clock_supported(int clockid) {
    return a20_posix_timer_clock_supported(clockid);
}

int posix_timer_create(int owner_pid, int signo, int target_tid, int clockid) {
    return a20_posix_timer_create(owner_pid, signo, target_tid, clockid);
}

int posix_timer_delete(int owner_pid, int id) {
    return a20_posix_timer_delete(owner_pid, id);
}

int posix_timer_get_time(int owner_pid, int id, uint64_t out[4]) {
    return a20_posix_timer_get_time(owner_pid, id, out);
}

int posix_timer_set_time(int owner_pid, int id, const uint64_t ts[4],
                         int flags) {
    return a20_posix_timer_set_time(owner_pid, id, ts, flags);
}

int posix_timer_getoverrun(int owner_pid, int id) {
    return a20_posix_timer_getoverrun(owner_pid, id);
}

void posix_timer_tick(void) {
    a20_posix_timer_tick();
}

int posix_itimer_set(task_t *t, int which, uint64_t value_ticks,
                     uint64_t interval_ticks) {
    return a20_posix_itimer_set(t, which, value_ticks, interval_ticks);
}

int posix_itimer_get(task_t *t, int which, uint64_t out[2]) {
    return a20_posix_itimer_get(t, which, out);
}

void posix_itimer_cpu_tick(task_t *cur) {
    a20_posix_itimer_cpu_tick(cur);
}
