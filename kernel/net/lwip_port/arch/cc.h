#ifndef A20_LWIP_ARCH_CC_H
#define A20_LWIP_ARCH_CC_H

#include "core/types.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/random.h"

#define LWIP_NO_STDDEF_H    1
#define LWIP_NO_STDINT_H    1
#define LWIP_NO_INTTYPES_H  1
#define LWIP_NO_LIMITS_H    1
#define LWIP_NO_CTYPE_H     1
#define LWIP_NO_UNISTD_H    1

#define BYTE_ORDER LITTLE_ENDIAN

typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uint64_t  u64_t;
typedef int64_t   s64_t;
typedef uintptr_t mem_ptr_t;
typedef intptr_t  ptrdiff_t;

#define LWIP_HAVE_INT64 1
#define LWIP_ERR_T int
#define INT_MAX 2147483647
#define SSIZE_MAX ((ssize_t)((~0UL) >> 1))

#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

#define LWIP_PLATFORM_DIAG(x) do { printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) panic("lwIP assertion failed: %s", x)

#define LWIP_ERROR(message, expression, handler) do { \
    if (!(expression)) { \
        LWIP_PLATFORM_DIAG(("%s\n", message)); \
        handler; \
    } \
} while (0)

#define LWIP_PROVIDE_ERRNO 1

#endif /* A20_LWIP_ARCH_CC_H */

typedef uint64_t sys_prot_t;
