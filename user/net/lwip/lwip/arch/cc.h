#ifndef A20_NETD_ARCH_CC_H
#define A20_NETD_ARCH_CC_H

/* Userspace (Native ABI + liba20c) lwIP port. */
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>

typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uint64_t  u64_t;
typedef int64_t   s64_t;
typedef uintptr_t mem_ptr_t;
typedef ptrdiff_t ptrdiff_t;

#define LWIP_NO_STDDEF_H    1
#define LWIP_NO_STDINT_H    1
#define LWIP_NO_INTTYPES_H  1
#define LWIP_NO_LIMITS_H    1
#define LWIP_NO_CTYPE_H     1
#define LWIP_NO_UNISTD_H    1

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_HAVE_INT64 1
#define LWIP_ERR_T int

#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "u"
#define S32_F "d"
#define X32_F "x"
#define SZT_F "lu"

extern void a20_netd_printf(const char *fmt, ...);

#define LWIP_PLATFORM_DIAG(x) do { a20_netd_printf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { a20_netd_printf("lwIP assert: %s\n", x); \
                                     __asm__ volatile("ebreak"); for(;;); } while (0)

#define LWIP_ERROR(message, expression, handler) do { \
    if (!(expression)) { LWIP_PLATFORM_DIAG(("%s\n", message)); handler; } \
} while (0)

#define LWIP_PROVIDE_ERRNO 1

#define SSIZE_MAX ((ssize_t)((~0UL) >> 1))
typedef uint64_t sys_prot_t;

#endif
