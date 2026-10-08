#ifndef A20_LWIP_HOST_TEST_CC_H
#define A20_LWIP_HOST_TEST_CC_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef BYTE_ORDER
#define BYTE_ORDER 1234
#endif
typedef uint8_t u8_t;
typedef int8_t s8_t;
typedef uint16_t u16_t;
typedef int16_t s16_t;
typedef uint32_t u32_t;
typedef int32_t s32_t;
typedef uint64_t u64_t;
typedef int64_t s64_t;
typedef uintptr_t mem_ptr_t;

#define LWIP_HAVE_INT64 1
#define LWIP_ERR_T int
#define CONFIG_NET_LANES 4
#define LWIP_HEAP_LOCK()   a20_lwip_heap_lock()
#define LWIP_HEAP_UNLOCK() a20_lwip_heap_unlock()
typedef uint64_t sys_prot_t;
void a20_lwip_heap_lock(void);
void a20_lwip_heap_unlock(void);
#define LWIP_PLATFORM_DIAG(x) do { fprintf(stderr, x); } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { \
    fprintf(stderr, "lwIP assertion failed: %s\n", (x)); abort(); \
} while (0)
#define LWIP_ERROR(message, expression, handler) do { \
    if (!(expression)) { fprintf(stderr, "%s\n", (message)); handler; } \
} while (0)
#define LWIP_PROVIDE_ERRNO 1
#define LWIP_RAND() ((u32_t)rand())

#endif
