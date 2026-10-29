#ifndef BSD_HOST_TEST_HOOKS_H
#define BSD_HOST_TEST_HOOKS_H
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
extern _Atomic size_t backing_bytes, backing_count;
extern _Atomic int fail_next;
void host_canary_set(uintptr_t value);
uintptr_t host_canary_get(void);
#endif
