/* Bootarg-gated Linux-syscall tracer for guest diagnosis.
 *
 * Boot with `-append "trace=<comm-prefix>"`; every syscall of tasks whose
 * name starts with the prefix is logged to serial (entry line + return
 * value).  Syscalls still in flight after TRACE_SLOW_TICKS ticks are
 * reported as "slow" by a dedicated kthread, which is how hangs are
 * located (the last "enter" without a matching return is the blocker).
 * Output budget is capped so a chatty task cannot flood the console. */
#include "syscall_internal.h"
#include "abi/linux/syscall_entry.h"
#include "core/bootargs.h"
#include "core/timer.h"
#include "core/lock.h"
#include "sys/usercopy.h"
#include "core/kallsyms.h"
#include "core/trap.h"
#include "proc/proc.h"

#define TRACE_SLOTS 64
#define TRACE_SLOW_TICKS 6
#define TRACE_LINE_BUDGET 400000

typedef struct trace_slot {
    task_t *task;
    /* A slot holds no reference on task, and a SIGKILLed task can be freed
     * while its slot is still occupied.  The pid is a plain integer, so the
     * scanner can revalidate through it without first dereferencing the
     * possibly freed task. */
    int pid;
    const linux_syscall_entry_t *entry;
    uint64_t args[3];
    uint64_t start_tick;
    uint64_t generation;
} trace_slot_t;

static spinlock_t g_trace_lock = SPINLOCK_INIT;
static uint64_t g_trace_generation;
static char g_trace_prefix[16];
static int g_trace_ready;
static int g_trace_tgid;
static int g_trace_scanner_started;
static trace_slot_t g_trace_slots[TRACE_SLOTS];
static uint64_t g_trace_lines;

static uint64_t trace_now(void)
{
    return timer_get_ticks();
}

void syscall_trace_slow_scanner(void);

static void trace_start_scanner(void)
{
    int expected = 0;
    if (!__atomic_compare_exchange_n(&g_trace_scanner_started, &expected, 1, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return;
    /* Allocate outside the trace lock. A failed allocation may be retried. */
    if (proc_alloc(syscall_trace_slow_scanner) < 0)
        __atomic_store_n(&g_trace_scanner_started, 0, __ATOMIC_RELEASE);
}

static void trace_init(void)
{
    int announce = 0;
    uint64_t flags = spin_lock_irqsave(&g_trace_lock);
    if (!__atomic_load_n(&g_trace_ready, __ATOMIC_RELAXED)) {
        const char *args = bootargs_get();
        const char *key = args ? strstr(args, "trace=") : NULL;
        if (key) {
            key += 6;
            size_t i = 0;
            while (key[i] && key[i] != ' ' && key[i] != ',' &&
                   i < sizeof(g_trace_prefix) - 1) {
                g_trace_prefix[i] = key[i];
                i++;
            }
            g_trace_prefix[i] = '\0';
            announce = g_trace_prefix[0] != '\0';
        }
        /* Readers may use the immutable prefix only after it is complete. */
        __atomic_store_n(&g_trace_ready, 1, __ATOMIC_RELEASE);
    }
    spin_unlock_irqrestore(&g_trace_lock, flags);
    if (announce)
        printf("[TRACE] enabled for comm prefix '%s'\n", g_trace_prefix);
}

static int trace_take_line(void)
{
    uint64_t flags = spin_lock_irqsave(&g_trace_lock);
    int allowed = g_trace_lines < TRACE_LINE_BUDGET;
    if (allowed) g_trace_lines++;
    spin_unlock_irqrestore(&g_trace_lock, flags);
    return allowed;
}

static int trace_match(task_t *t)
{
    if (!__atomic_load_n(&g_trace_ready, __ATOMIC_ACQUIRE))
        trace_init();
    if (!g_trace_prefix[0] || !t)
        return 0;
    /* Follow the whole thread group: glib workers rename themselves via
     * PR_SET_NAME right after clone, so a pure comm-prefix match would drop
     * them exactly when they start doing interesting work. */
    int group = __atomic_load_n(&g_trace_tgid, __ATOMIC_RELAXED);
    if (group && t->tgid == group) {
        trace_start_scanner();
        return 1;
    }
    const char *name = t->name;
    if (!name[0])
        return 0;
    if (strncmp(name, g_trace_prefix, strlen(g_trace_prefix)) != 0)
        return 0;
    __atomic_store_n(&g_trace_tgid, t->tgid, __ATOMIC_RELAXED);
    trace_start_scanner();
    return 1;
}

/* g_trace_lock held; the returned slot never escapes that critical section. */
static trace_slot_t *trace_slot_for(task_t *t)
{
    trace_slot_t *free_slot = NULL;
    for (int i = 0; i < TRACE_SLOTS; i++) {
        trace_slot_t *s = &g_trace_slots[i];
        if (s->task == t)
            return s;
        if (!s->task && !free_slot)
            free_slot = s;
    }
    return free_slot;
}

void syscall_trace_enter(task_t *t, const linux_syscall_entry_t *entry,
                         const linux_syscall_args_t *args)
{
    if (!trace_match(t))
        return;
    uint64_t flags = spin_lock_irqsave(&g_trace_lock);
    trace_slot_t *s = trace_slot_for(t);
    if (!s) {
        spin_unlock_irqrestore(&g_trace_lock, flags);
        return;
    }
    s->task = t;
    s->pid = t->pid;
    s->entry = entry;
    s->args[0] = args->arg[0];
    s->args[1] = args->arg[1];
    s->args[2] = args->arg[2];
    s->start_tick = trace_now();
    s->generation = ++g_trace_generation;
    spin_unlock_irqrestore(&g_trace_lock, flags);
    if (!trace_take_line()) return;
    char path[56];
    path[0] = '\0';
    if ((entry->nr == SYS_openat || entry->nr == SYS_readlinkat ||
         entry->nr == SYS_statx || entry->nr == SYS_fstatat) &&
        args->arg[1])
        user_strncpy(path, (const char *)(uintptr_t)args->arg[1],
                     sizeof(path) - 1);
    if (path[0])
        printf("[TRACE] %d(%s) %s(%llx \"%s\")\n", t->pid, t->name,
               entry->name ? entry->name : "?",
               (unsigned long long)args->arg[0], path);
    else
        printf("[TRACE] %d(%s) %s(%llx %llx %llx)\n", t->pid, t->name,
               entry->name ? entry->name : "?",
               (unsigned long long)args->arg[0],
               (unsigned long long)args->arg[1],
               (unsigned long long)args->arg[2]);
}

void syscall_trace_exit(task_t *t, const linux_syscall_entry_t *entry,
                        int64_t ret)
{
    if (!t) return;
    int matched = trace_match(t);
    if (!g_trace_prefix[0]) return;
    uint64_t flags = spin_lock_irqsave(&g_trace_lock);
    trace_slot_t *s = trace_slot_for(t);
    int tracked = s && s->task == t && s->entry == entry;
    /* An exec/name change or another matching thread group must not leave
     * this completed entry pending merely because trace_match changed. */
    if (tracked) memset(s, 0, sizeof(*s));
    spin_unlock_irqrestore(&g_trace_lock, flags);
    if ((!tracked && !matched) || !trace_take_line()) return;
    printf("[TRACE] %d(%s) %s = %ld\n", t->pid, t->name,
           entry->name ? entry->name : "?", (long)ret);
}

static void trace_scan_slow_once(uint64_t now)
{
    for (int i = 0; i < TRACE_SLOTS; i++) {
        uint64_t flags = spin_lock_irqsave(&g_trace_lock);
        trace_slot_t snapshot = g_trace_slots[i];
        spin_unlock_irqrestore(&g_trace_lock, flags);
        if (!snapshot.task || !snapshot.entry)
            continue;
        /* The timestamp can be newer than this pass's 'now' when a different
         * CPU enters a syscall after the pass begins. Do not underflow age. */
        if (now < snapshot.start_tick || now - snapshot.start_tick < TRACE_SLOW_TICKS)
            continue;

        /* A slot borrows its task pointer. Pin by pid before dereferencing;
         * never use a pointer loaded from the mutable slot after unlocking. */
        task_t *live = proc_find_get(snapshot.pid);
        flags = spin_lock_irqsave(&g_trace_lock);
        int current = g_trace_slots[i].generation == snapshot.generation &&
                      g_trace_slots[i].task == snapshot.task;
        if (live != snapshot.task && current)
            memset(&g_trace_slots[i], 0, sizeof(g_trace_slots[i]));
        spin_unlock_irqrestore(&g_trace_lock, flags);
        if (live != snapshot.task || !current) {
            if (live) proc_put(live);
            continue;
        }
        if (trace_take_line()) {
            printf("[TRACE-SLOW] pid=%d(%s) stuck in %s for %llu ticks "
                   "(args %llx %llx %llx)\n",
                   live->pid, live->name,
                   snapshot.entry->name ? snapshot.entry->name : "?",
                   (unsigned long long)(now - snapshot.start_tick),
                   (unsigned long long)snapshot.args[0],
                   (unsigned long long)snapshot.args[1],
                   (unsigned long long)snapshot.args[2]);
            trap_context_t *tctx = live->trap_ctx;
            if (tctx) {
                struct backtrace_frame frames[16];
                int n = arch_unwind_frames(TRAP_CTX_FP(tctx), frames, 16);
                printf("[TRACE-SLOW] backtrace pid=%d:\n", live->pid);
                kallsyms_print(TRAP_CTX_RA(tctx));
                printf("\n");
                for (int fi = 0; fi < n; fi++) {
                    printf("  [#%d] ", fi);
                    kallsyms_print(frames[fi].pc);
                    printf("\n");
                }
            }
        }
        /* This reference also retains the saved kernel stack/trap frame. */
        proc_put(live);
    }
}

void syscall_trace_slow_scanner(void)
{
    task_t *self = proc_current();
    proc_set_name(self, "syscall-trace");
    for (;;) {
        proc_sleep_until(trace_now() + TICKS_PER_SEC / 2);
        trace_scan_slow_once(trace_now());
    }
}
