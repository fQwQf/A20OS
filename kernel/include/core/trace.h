#ifndef A20_CORE_TRACE_H
#define A20_CORE_TRACE_H
#include "core/types.h"
#ifndef CONFIG_A20_TRACE
#define CONFIG_A20_TRACE 0
#endif
#ifndef CONFIG_A20_TRACE_RECORDS
#define CONFIG_A20_TRACE_RECORDS 128
#endif
#ifndef CONFIG_A20_TRACE_MASK
#define CONFIG_A20_TRACE_MASK 63
#endif
#ifndef CONFIG_A20_TRACE_BOOT
#define CONFIG_A20_TRACE_BOOT 0
#endif
#if CONFIG_A20_TRACE_RECORDS < 16 || CONFIG_A20_TRACE_RECORDS > 1024
#error "DIAG_TRACE_RECORDS must be 16..1024 per CPU"
#endif
#if CONFIG_A20_TRACE_MASK < 1 || CONFIG_A20_TRACE_MASK > 63
#error "DIAG_TRACE_MASK must be 1..63"
#endif
/* Event category is the high byte (a mask bit), low byte is begin=1,
 * end=2, or instant=3. Numeric payloads never contain kernel pointers. */
enum a20_trace_category { A20_TRACE_BOOT = 1, A20_TRACE_DRIVER = 2,
    A20_TRACE_IRQ = 4, A20_TRACE_IO = 8, A20_TRACE_SCHED = 16, A20_TRACE_USER = 32 };
typedef struct a20_trace_token { uint64_t generation, id, ticks; unsigned category; } a20_trace_token_t;
#if CONFIG_A20_TRACE
void a20_trace_init(void);
void a20_trace_event(unsigned event, uint64_t id, uint64_t a, uint64_t b);
a20_trace_token_t a20_trace_begin(unsigned category, uint64_t a, uint64_t b);
void a20_trace_end(a20_trace_token_t token, int64_t result);
/* Process context. start [mask], stop/freeze, reset, mark <number>. */
int a20_trace_control(const char *command, size_t length);
int a20_trace_format(char *buffer, size_t capacity);
#else
#define a20_trace_init() ((void)0)
#define a20_trace_event(...) ((void)0)
#define a20_trace_begin(...) ((a20_trace_token_t){0})
#define a20_trace_end(token, ...) ((void)sizeof(token))
#endif
#endif
