#include "core/trace.h"
#include "core/cpu.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/report.h"
#include "core/timer.h"
#include "core/string.h"
#include "core/errno.h"
#include "core/klog.h"
#include "mm/slab.h"

#if CONFIG_NR_CPUS * CONFIG_A20_TRACE_RECORDS > 4096
#error "trace capacity exceeds 4096 total events (procfs bounded export)"
#endif
#define CATEGORIES 6
#define BUCKETS 32
/* No allocation, formatting, file I/O or task lookup in an event producer.
 * Per-CPU leaf locks disable local IRQs, including nested IRQ producers.
 * NMI-like contexts must not call this API. */
typedef struct trace_event { uint64_t seq, ticks, id, a, b; unsigned event, cpu; } trace_event_t;
typedef struct trace_latency { uint64_t count, errors, total, maximum, buckets[BUCKETS]; } trace_latency_t;
typedef struct trace_cpu {
    spinlock_t lock;
    uint64_t seq, overwritten, rejected, clock_regressions;
    unsigned count, cursor;
    trace_latency_t latency[CATEGORIES];
    trace_event_t events[CONFIG_A20_TRACE_RECORDS];
} trace_cpu_t;
static trace_cpu_t cpus[CONFIG_NR_CPUS];
static mutex_t control_lock = MUTEX_INIT;
static unsigned enabled, mask = CONFIG_A20_TRACE_MASK;
static uint64_t generation, next_id, unsupported_cpu, start_ticks, stop_ticks;
static int initialized;
static spinlock_t metadata_lock = SPINLOCK_INIT;
static unsigned category_index(unsigned category) {
    unsigned i = 0;
    while (category > 1) { category >>= 1; i++; }
    return i;
}
static int selected(unsigned category) {
    return initialized && __atomic_load_n(&enabled, __ATOMIC_ACQUIRE) &&
        (__atomic_load_n(&mask, __ATOMIC_RELAXED) & category);
}
static void append(trace_cpu_t *cpu, unsigned number, unsigned event,
                   uint64_t id, uint64_t a, uint64_t b, uint64_t now) {
    if (cpu->count == CONFIG_A20_TRACE_RECORDS) cpu->overwritten++;
    else cpu->count++;
    cpu->events[cpu->cursor] = (trace_event_t){++cpu->seq, now, id, a, b, event, number};
    cpu->cursor = (cpu->cursor + 1) % CONFIG_A20_TRACE_RECORDS;
}
/* Reading the generation before taking the lock prevents delayed producers
 * from writing an event into a restarted collection. IRQ-safe CPU identity. */
static trace_cpu_t *enter(unsigned category, uint64_t epoch, uint64_t *flags, unsigned *number) {
    if (!selected(category)) return NULL;
    preempt_disable();
    *number = arch_current_cpu_id();
    if (*number >= CONFIG_NR_CPUS) {
        uint64_t f = spin_lock_irqsave(&metadata_lock);
        if (selected(category) && __atomic_load_n(&generation, __ATOMIC_ACQUIRE) == epoch) unsupported_cpu++;
        spin_unlock_irqrestore(&metadata_lock, f);
        preempt_enable(); return NULL;
    }
    trace_cpu_t *cpu = &cpus[*number];
    *flags = spin_lock_irqsave(&cpu->lock);
    if (!selected(category) || __atomic_load_n(&generation, __ATOMIC_ACQUIRE) != epoch) {
        /* Frozen snapshots cannot change, including their loss counters. */
        if (selected(category)) cpu->rejected++;
        spin_unlock_irqrestore(&cpu->lock, *flags); preempt_enable(); return NULL;
    }
    return cpu;
}
static void leave(trace_cpu_t *cpu, uint64_t flags) {
    spin_unlock_irqrestore(&cpu->lock, flags); preempt_enable();
}
void a20_trace_event(unsigned event, uint64_t id, uint64_t a, uint64_t b) {
    unsigned category = event >> 8;
    if (!category || category > 32 || (category & (category - 1)) || !selected(category)) return;
    uint64_t epoch = __atomic_load_n(&generation, __ATOMIC_ACQUIRE), flags;
    unsigned number;
    trace_cpu_t *cpu = enter(category, epoch, &flags, &number);
    if (!cpu) return;
    append(cpu, number, event, id, a, b, timer_get_ticks());
    leave(cpu, flags);
}
a20_trace_token_t a20_trace_begin(unsigned category, uint64_t a, uint64_t b) {
    a20_trace_token_t token = {0};
    if (!category || category > 32 || (category & (category - 1)) || !selected(category)) return token;
    token.generation = __atomic_load_n(&generation, __ATOMIC_ACQUIRE);
    uint64_t flags; unsigned number;
    trace_cpu_t *cpu = enter(category, token.generation, &flags, &number);
    if (!cpu) return (a20_trace_token_t){0};
    token.category = category;
    token.id = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
    token.ticks = timer_get_ticks();
    append(cpu, number, (category << 8) | 1, token.id, a, b, token.ticks);
    leave(cpu, flags);
    return token;
}
void a20_trace_end(a20_trace_token_t token, int64_t result) {
    if (!token.id || !token.category || token.category > 32 ||
        (token.category & (token.category - 1))) return;
    uint64_t flags; unsigned number;
    trace_cpu_t *cpu = enter(token.category, token.generation, &flags, &number);
    if (!cpu) return;
    uint64_t now = timer_get_ticks();
    if (now < token.ticks) { cpu->clock_regressions++; leave(cpu, flags); return; }
    uint64_t elapsed = now - token.ticks;
    trace_latency_t *latency = &cpu->latency[category_index(token.category)];
    unsigned bucket = 0; uint64_t size = elapsed;
    while (size > 1 && bucket < BUCKETS - 1) { size >>= 1; bucket++; }
    latency->count++; latency->errors += result < 0; latency->total += elapsed;
    if (elapsed > latency->maximum) latency->maximum = elapsed;
    latency->buckets[bucket]++;
    append(cpu, number, (token.category << 8) | 2, token.id, (uint64_t)result, elapsed, now);
    leave(cpu, flags);
}
static void drain(void) {
    uint64_t f = spin_lock_irqsave(&metadata_lock);
    spin_unlock_irqrestore(&metadata_lock, f);
    for (unsigned n = 0; n < CONFIG_NR_CPUS; n++) {
        uint64_t flags = spin_lock_irqsave(&cpus[n].lock);
        spin_unlock_irqrestore(&cpus[n].lock, flags);
    }
}
static void reset(void) {
    for (unsigned n = 0; n < CONFIG_NR_CPUS; n++) {
        uint64_t flags = spin_lock_irqsave(&cpus[n].lock);
        /* Do not replace a live lock. */
        cpus[n].seq = cpus[n].overwritten = cpus[n].rejected = cpus[n].clock_regressions = 0;
        cpus[n].count = cpus[n].cursor = 0;
        memset(cpus[n].latency, 0, sizeof(cpus[n].latency));
        spin_unlock_irqrestore(&cpus[n].lock, flags);
    }
    uint64_t f = spin_lock_irqsave(&metadata_lock);
    unsupported_cpu = 0;
    spin_unlock_irqrestore(&metadata_lock, f);
    start_ticks = stop_ticks = 0;
    uint64_t epoch = generation + 1;
    __atomic_store_n(&generation, epoch ? epoch : 1, __ATOMIC_RELEASE);
}
void a20_trace_init(void) {
    for (unsigned n = 0; n < CONFIG_NR_CPUS; n++) spin_init(&cpus[n].lock);
    initialized = 1;
#if CONFIG_A20_TRACE_BOOT
    reset(); start_ticks = timer_get_ticks();
    __atomic_store_n(&enabled, 1, __ATOMIC_RELEASE);
#endif
}
static int decimal(const char *p, unsigned *out) {
    if (!*p) return -EINVAL;
    unsigned n = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9' || n > (0xffffffffU - (*p - '0')) / 10) return -EINVAL;
        n = n * 10 + (*p - '0');
    }
    *out = n; return 0;
}
int a20_trace_control(const char *command, size_t length) {
    if (!initialized || !command || !length || length >= 32 || memchr(command, 0, length)) return -EINVAL;
    char text[32]; memcpy(text, command, length);
    while (length && (text[length - 1] == '\n' || text[length - 1] == '\r')) length--;
    text[length] = 0;
    unsigned requested = __atomic_load_n(&mask, __ATOMIC_RELAXED), marker = 0;
    int start = !strcmp(text, "start");
    int mark = !strncmp(text, "mark ", 5);
    if (!strncmp(text, "start ", 6)) {
        if (decimal(text + 6, &requested) < 0 || !requested || requested > 63) return -EINVAL;
        start = 1;
    }
    if (mark && decimal(text + 5, &marker) < 0) return -EINVAL;
    mutex_lock(&control_lock);
    int ret = 0;
    if (start || !strcmp(text, "reset")) {
        if (enabled) ret = -EBUSY;
        else {
            reset(); __atomic_store_n(&mask, requested, __ATOMIC_RELAXED);
            if (start) { start_ticks = timer_get_ticks(); __atomic_store_n(&enabled, 1, __ATOMIC_RELEASE); }
        }
    } else if (!strcmp(text, "stop") || !strcmp(text, "freeze")) {
        if (enabled) {
            __atomic_store_n(&enabled, 0, __ATOMIC_RELEASE); drain();
            stop_ticks = timer_get_ticks();
        }
    } else if (mark) {
        if (!selected(A20_TRACE_USER)) ret = -EAGAIN;
        else a20_trace_event((A20_TRACE_USER << 8) | 3, marker, 0, 0);
    } else ret = -EINVAL;
    mutex_unlock(&control_lock);
    return ret;
}
/* Copy under leaf locks, render after release. Stopped snapshots are stable;
 * an active snapshot is a sequence of per-CPU cuts, never a global instant. */
int a20_trace_format(char *buffer, size_t capacity) {
    if (!buffer || !capacity) return -EINVAL;
    trace_cpu_t *copy = kmalloc(sizeof(cpus));
    if (!copy) return -ENOMEM;
    report_writer_t w = {.buffer = buffer, .capacity = capacity};
    mutex_lock(&control_lock);
    unsigned active = enabled;
    uint64_t epoch = generation, begin = start_ticks, end = stop_ticks;
    uint64_t f = spin_lock_irqsave(&metadata_lock);
    uint64_t unsupported = unsupported_cpu;
    spin_unlock_irqrestore(&metadata_lock, f);
    unsigned filter = mask;
    for (unsigned n = 0; n < CONFIG_NR_CPUS; n++) {
        uint64_t flags = spin_lock_irqsave(&cpus[n].lock);
        memcpy(&copy[n], &cpus[n], sizeof(trace_cpu_t));
        spin_unlock_irqrestore(&cpus[n].lock, flags);
    }
    mutex_unlock(&control_lock);
    report_printf(&w, "{\"schema\":1,\"kind\":\"trace\",\"boot\":"); report_json_string(&w, klog_boot_id());
    report_printf(&w, ",\"generation\":%llu,\"active\":%u,\"mask\":%u,\"cpus\":%u,\"capacity_per_cpu\":%u,\"ticks_per_second\":%llu,\"start_ticks\":%llu,\"stop_ticks\":%llu,\"unsupported_cpu\":%llu,\"clock\":\"arch_timer;cross_cpu_order_not_guaranteed\"}\n",
                  (unsigned long long)epoch, active, filter, CONFIG_NR_CPUS, CONFIG_A20_TRACE_RECORDS,
                  (unsigned long long)TICKS_PER_SEC, (unsigned long long)begin, (unsigned long long)end,
                  (unsigned long long)unsupported);
    unsigned events = 0;
    for (unsigned n = 0; n < CONFIG_NR_CPUS; n++) {
        trace_cpu_t *cpu = &copy[n];
        report_printf(&w, "{\"kind\":\"cpu\",\"cpu\":%u,\"retained\":%u,\"overwritten\":%llu,\"rejected\":%llu,\"clock_regressions\":%llu}\n",
                      n, cpu->count, (unsigned long long)cpu->overwritten,
                      (unsigned long long)cpu->rejected, (unsigned long long)cpu->clock_regressions);
        for (unsigned c = 0; c < CATEGORIES; c++) {
            trace_latency_t *l = &cpu->latency[c];
            if (!l->count) continue;
            report_printf(&w, "{\"kind\":\"latency\",\"cpu\":%u,\"category\":%u,\"samples\":%llu,\"errors\":%llu,\"elapsed_ticks\":%llu,\"max_ticks\":%llu,\"log2_buckets\":[",
                          n, 1U << c, (unsigned long long)l->count, (unsigned long long)l->errors, (unsigned long long)l->total, (unsigned long long)l->maximum);
            for (unsigned b = 0; b < BUCKETS; b++) report_printf(&w, "%s%llu", b ? "," : "", (unsigned long long)l->buckets[b]);
            report_printf(&w, "]}\n");
        }
        for (unsigned i = 0; i < cpu->count; i++) {
            trace_event_t *event = &cpu->events[(cpu->cursor + CONFIG_A20_TRACE_RECORDS - cpu->count + i) % CONFIG_A20_TRACE_RECORDS];
            report_printf(&w, "{\"kind\":\"event\",\"cpu\":%u,\"seq\":%llu,\"ticks\":%llu,\"event\":%u,\"id\":%llu,\"a\":%llu,\"b\":%llu}\n",
                          n, (unsigned long long)event->seq, (unsigned long long)event->ticks, event->event,
                          (unsigned long long)event->id, (unsigned long long)event->a, (unsigned long long)event->b);
            events++;
        }
    }
    report_printf(&w, "{\"kind\":\"end\",\"events\":%u,\"complete\":true}\n", events);
    kfree(copy);
    return w.error == -ENOSPC ? (int)(capacity - 1) : w.error ? w.error : (int)w.length;
}
