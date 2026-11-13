#ifndef A20_RTW_THREAD_CALL_H
#define A20_RTW_THREAD_CALL_H
#include <stdint.h>
#include <stdbool.h>
typedef int boolean_t;
typedef void *thread_call_param_t;
typedef void (*thread_call_func_t)(thread_call_param_t, thread_call_param_t);
typedef struct a20_thread_call *thread_call_t;
thread_call_t thread_call_allocate(thread_call_func_t, thread_call_param_t);
boolean_t thread_call_enter(thread_call_t);
boolean_t thread_call_enter_delayed(thread_call_t, uint64_t);
boolean_t thread_call_cancel(thread_call_t);
boolean_t thread_call_cancel_wait(thread_call_t);
boolean_t thread_call_free(thread_call_t);
#endif
