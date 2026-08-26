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
#include "sys/usercopy.h"
#include "core/kallsyms.h"
#include "core/trap.h"
#include "proc/proc.h"

#define TRACE_SLOTS 64
#define TRACE_SLOW_TICKS 6
#define TRACE_LINE_BUDGET 400000

typedef struct trace_slot {
    task_t *task;
    const linux_syscall_entry_t *entry;
    uint64_t args[3];
    uint64_t start_tick;
} trace_slot_t;

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
    if (g_trace_scanner_started)
        return;
    g_trace_scanner_started = 1;
    /* Best effort: without the kthread we still get entry and exit lines,
     * only the slow-in-flight reports are lost. */
    proc_alloc(syscall_trace_slow_scanner);
}

static void trace_init(void)
{
    const char *args = bootargs_get();
    const char *key = args ? strstr(args, "trace=") : NULL;
    g_trace_ready = 1;
    if (!key)
        return;
    key += 6;
    size_t i = 0;
    while (key[i] && key[i] != ' ' && key[i] != ',' &&
           i < sizeof(g_trace_prefix) - 1) {
        g_trace_prefix[i] = key[i];
        i++;
    }
    g_trace_prefix[i] = '\0';
    if (!g_trace_prefix[0])
        return;
    printf("[TRACE] enabled for comm prefix '%s'\n", g_trace_prefix);
}

static int trace_match(task_t *t)
{
    if (!g_trace_ready)
        trace_init();
    if (!g_trace_prefix[0] || !t)
        return 0;
    /* Follow the whole thread group: glib workers rename themselves via
     * PR_SET_NAME right after clone, so a pure comm-prefix match would drop
     * them exactly when they start doing interesting work. */
    if (g_trace_tgid && t->tgid == g_trace_tgid) {
        trace_start_scanner();
        return 1;
    }
    const char *name = t->name;
    if (!name[0])
        return 0;
    if (strncmp(name, g_trace_prefix, strlen(g_trace_prefix)) != 0)
        return 0;
    g_trace_tgid = t->tgid;
    trace_start_scanner();
    return 1;
}

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
    trace_slot_t *s = trace_slot_for(t);
    if (!s)
        return;
    s->task = t;
    s->entry = entry;
    s->args[0] = args->arg[0];
    s->args[1] = args->arg[1];
    s->args[2] = args->arg[2];
    s->start_tick = trace_now();
    if (g_trace_lines >= TRACE_LINE_BUDGET)
        return;
    g_trace_lines++;
    char path[56];
    path[0] = '\0';
    if ((entry->nr == SYS_openat || entry->nr == SYS_readlinkat ||
         entry->nr == SYS_statx || entry->nr == SYS_fstatat) &&
        args->arg[1])
        user_strncpy(path, (const char *)args->arg[1],
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
    if (!trace_match(t))
        return;
    trace_slot_t *s = trace_slot_for(t);
    if (s && s->entry == entry)
        s->task = NULL;
    if (g_trace_lines >= TRACE_LINE_BUDGET)
        return;
    g_trace_lines++;
    printf("[TRACE] %d(%s) %s = %ld\n", t->pid, t->name,
           entry->name ? entry->name : "?", (long)ret);
}

void syscall_trace_slow_scanner(void)
{
    task_t *self = proc_current();
    proc_set_name(self, "syscall-trace");
    for (;;) {
        proc_sleep_until(trace_now() + TICKS_PER_SEC / 2);
        uint64_t now = trace_now();
        for (int i = 0; i < TRACE_SLOTS; i++) {
            trace_slot_t *s = &g_trace_slots[i];
            if (!s->task || !s->entry || s->start_tick == 0)
                continue;
            /* A SIGKILLed task exits without an exit-trace; its slot would
             * otherwise report garbage from freed task memory forever. */
            task_t *live = proc_find_get(s->task->pid);
            if (live != s->task) {
                if (live)
                    proc_put(live);
                s->task = NULL;
                s->entry = NULL;
                continue;
            }
            proc_put(live);
            uint64_t age = now - s->start_tick;
            if (age >= TRACE_SLOW_TICKS &&
                g_trace_lines < TRACE_LINE_BUDGET) {
                g_trace_lines++;
                printf("[TRACE-SLOW] pid=%d(%s) stuck in %s for %llu ticks "
                       "(args %llx %llx %llx)\n",
                       s->task->pid, s->task->name,
                       s->entry->name ? s->entry->name : "?",
                       (unsigned long long)age,
                       (unsigned long long)s->args[0],
                       (unsigned long long)s->args[1],
                       (unsigned long long)s->args[2]);
                trap_context_t *tctx = s->task->trap_ctx;
                if (tctx) {
                    struct backtrace_frame frames[16];
                    int n = arch_unwind_frames(TRAP_CTX_FP(tctx), frames, 16);
                    printf("[TRACE-SLOW] backtrace pid=%d:\n", s->task->pid);
                    kallsyms_print(TRAP_CTX_RA(tctx));
                    printf("\n");
                    for (int fi = 0; fi < n; fi++) {
                        printf("  [#%d] ", fi);
                        kallsyms_print(frames[fi].pc);
                        printf("\n");
                    }
                }
            }
        }
    }
}
