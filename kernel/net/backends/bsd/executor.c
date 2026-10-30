/* Serialized process-context access to the single FreeBSD protocol domain.
 * The core is entered by one detached task only. Device interrupts and timer
 * ticks wake this task; they never execute BSD code or touch its TLS. */
#include "executor.h"
#include "runtime/memory.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/sync.h"
#include "core/lock_counters.h"
#include "proc/proc.h"

extern void *a20_bsd_port_tls_create(void);
extern int a20_bsd_port_tls_bind(void *context);
extern int a20_bsd_port_tls_unbind(void *context);
extern int a20_bsd_port_tls_destroy(void *context);
extern int a20_bsd_port_boot_config(size_t byte_budget);
extern int a20_bsd_ff_freebsd_init(void);
extern void a20_bsd_port_memory_snapshot(struct bsd_memory_stats *out);

typedef struct bsd_job {
    struct bsd_job *next;
    a20_bsd_executor_job_t run;
    void *argument;
    int result;
    completion_t done;
} bsd_job_t;

static spinlock_t executor_lock = SPINLOCK_INIT;
static wait_queue_t executor_work = WAIT_QUEUE_INIT;
static completion_t executor_started = COMPLETION_INIT;
static bsd_job_t *job_head, **job_tail = &job_head;
static task_t *executor_task;
static void *executor_tls;
static int executor_state; /* 0=off, 1=starting, 2=ready, 3=failed */
static int executor_start_result;
static unsigned executor_work_seq;
static a20_bsd_executor_progress_t progress_callback;
static void *progress_argument;

int a20_bsd_executor_ready(void)
{
    return __atomic_load_n(&executor_state, __ATOMIC_ACQUIRE) == 2;
}

int a20_bsd_executor_is_current(void)
{
    return proc_current() && proc_current() ==
        __atomic_load_n(&executor_task, __ATOMIC_ACQUIRE);
}

void a20_bsd_executor_set_progress(a20_bsd_executor_progress_t progress,
                                   void *argument)
{
    if (a20_bsd_executor_ready()) return;
    progress_argument = argument;
    progress_callback = progress;
}

void a20_bsd_executor_notify(void)
{
    __atomic_add_fetch(&executor_work_seq, 1, __ATOMIC_RELEASE);
    wait_queue_wake_one(&executor_work, 0, PROC_WAKE_EVENT);
}

static bsd_job_t *take_job(void)
{
    uint64_t flags = spin_lock_irqsave(&executor_lock);
    bsd_job_t *job = job_head;
    if (job) {
        job_head = job->next;
        if (!job_head) job_tail = &job_head;
    }
    spin_unlock_irqrestore(&executor_lock, flags);
    return job;
}

static void executor_main(void)
{
    proc_set_name(proc_current(), "bsd-net");
    __atomic_store_n(&executor_task, proc_current(), __ATOMIC_RELEASE);
    int result = 0;
    executor_tls = a20_bsd_port_tls_create();
    if (!executor_tls) {
        result = -ENOMEM;
        goto failed;
    }
    result = a20_bsd_port_tls_bind(executor_tls);
    if (result < 0) goto failed;
    struct bsd_memory_stats memory;
    a20_bsd_port_memory_snapshot(&memory);
    result = a20_bsd_port_boot_config(memory.limit);
    if (result > 0) result = -result;
    if (result < 0) goto unbind;
    result = a20_bsd_ff_freebsd_init();
    if (result > 0) result = -result;
    if (result < 0) goto unbind;

    __atomic_store_n(&executor_state, 2, __ATOMIC_RELEASE);
    complete_all(&executor_started);
    printf("[NET] BSD executor ready (pid=%d)\n", proc_current()->pid);

    for (;;) {
        unsigned observed_seq = __atomic_load_n(&executor_work_seq, __ATOMIC_ACQUIRE);
        bsd_job_t *job;
        while ((job = take_job()) != NULL) {
            job->result = job->run(job->argument);
            /* The requester owns its stack job after this call. The worker does
             * not dereference it again, including during its next loop pass. */
            complete(&job->done);
        }
        a20_bsd_executor_progress_t progress = progress_callback;
        if (progress) progress(progress_argument);

        proc_wait_token_t token = proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0);
        wait_queue_entry_t entry = {0};
        uint64_t flags = spin_lock_irqsave(&executor_lock);
        bool linked = !job_head && observed_seq ==
            __atomic_load_n(&executor_work_seq, __ATOMIC_ACQUIRE) &&
            wait_queue_link(&executor_work, &entry, token, 0);
        spin_unlock_irqrestore(&executor_lock, flags);
        if (linked)
            proc_park_commit(token);
        else
            (void)proc_park_cancel(token);
        wait_queue_unlink(&executor_work, &entry);
        proc_park_finish(token);
    }

unbind:
    (void)a20_bsd_port_tls_unbind(executor_tls);
failed:
    if (executor_tls) {
        (void)a20_bsd_port_tls_destroy(executor_tls);
        executor_tls = NULL;
    }
    executor_start_result = result < 0 ? result : -EIO;
    __atomic_store_n(&executor_state, 3, __ATOMIC_RELEASE);
    complete_all(&executor_started);
}

int a20_bsd_executor_start(void)
{
    static int registered;
    if (!registered) {
        lock_counters_register(&executor_lock, "bsd");
        registered = 1;
    }
    uint64_t flags = spin_lock_irqsave(&executor_lock);
    int state = executor_state;
    if (state == 0) {
        executor_state = 1;
        executor_start_result = 0;
        completion_init(&executor_started);
    }
    spin_unlock_irqrestore(&executor_lock, flags);
    if (state == 2) return 0;
    if (state == 3) return executor_start_result;
    if (state == 1) {
        wait_for_completion(&executor_started);
        return a20_bsd_executor_ready() ? 0 : executor_start_result;
    }

    int pid = proc_alloc_detached(executor_main);
    if (pid < 0) {
        flags = spin_lock_irqsave(&executor_lock);
        executor_state = 3;
        executor_start_result = pid;
        spin_unlock_irqrestore(&executor_lock, flags);
        complete_all(&executor_started);
        return pid;
    }
    wait_for_completion(&executor_started);
    return a20_bsd_executor_ready() ? 0 : executor_start_result;
}

int a20_bsd_executor_call(a20_bsd_executor_job_t run, void *argument)
{
    if (!run) return -EINVAL;
    if (a20_bsd_executor_is_current()) return run(argument);
    if (!a20_bsd_executor_ready() || !proc_current()) return -ENETDOWN;

    bsd_job_t job = {
        .run = run,
        .argument = argument,
        .done = COMPLETION_INIT,
    };
    uint64_t flags = spin_lock_irqsave(&executor_lock);
    if (executor_state != 2) {
        spin_unlock_irqrestore(&executor_lock, flags);
        return -ENETDOWN;
    }
    job.next = NULL;
    *job_tail = &job;
    job_tail = &job.next;
    spin_unlock_irqrestore(&executor_lock, flags);
    a20_bsd_executor_notify();
    wait_for_completion(&job.done);
    return job.result;
}
