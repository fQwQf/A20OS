/*
 * Host-test arch/cc.h for tools/test-tcp-cubic-host.c.
 *
 * The kernel's own arch/cc.h (kernel/net/lwip_port/arch/cc.h) includes
 * core/types.h, core/stdio.h, core/panic.h and core/random.h -- kernel headers
 * that cannot be compiled by a host cc.  This stand-in provides only what the
 * CUBIC translation unit needs from it: the lwIP scalar typedefs and the
 * handful of feature macros.
 *
 * The typedefs here are the SAME ones the test file declares for itself, and
 * they are deliberately identical to the kernel's (kernel/net/lwip_port/arch/
 * cc.h:22-30), so the widths the algorithm sees under test are the widths it
 * sees in the kernel.  If that ever stops being true the test is measuring the
 * wrong thing, so the widths are spelled out rather than inherited.
 */
#ifndef A20_TCP_CUBIC_TEST_ARCH_CC_H
#define A20_TCP_CUBIC_TEST_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   u8_t;
typedef int8_t    s8_t;
typedef uint16_t  u16_t;
typedef int16_t   s16_t;
typedef uint32_t  u32_t;
typedef int32_t   s32_t;
typedef uint64_t  u64_t;
typedef int64_t   s64_t;
typedef uintptr_t mem_ptr_t;

#define LWIP_NO_STDDEF_H    1
#define LWIP_NO_STDINT_H    1
#define LWIP_NO_INTTYPES_H  1
#define LWIP_NO_LIMITS_H    1
#define LWIP_NO_CTYPE_H     1
/* Not 1: lwIP's arch.h must be allowed to reach <unistd.h> on this host.  See
 * the SSIZE_MAX note below. */
#undef  LWIP_NO_UNISTD_H
#define LWIP_NO_UNISTD_H    0

/* lwIP's arch.h only consults BYTE_ORDER to pick its own; the host libc's
 * endian.h already defines it, so redefine it here and the host value is
 * correct for this machine anyway.  Guarded rather than unconditional because
 * including <stdlib.h> first (the test does) pulls in <endian.h>. */
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

#define LWIP_ERR_T int

/* lwIP's arch.h (lines 188-202) does `typedef int ssize_t` unless SSIZE_MAX is
 * already defined -- a fallback for hosted systems lacking <sys/types.h>.  A
 * host always has it, so including <limits.h> for SSIZE_MAX makes arch.h take
 * its <unistd.h> branch instead and not redeclare ssize_t as int on top of the
 * libc's long.  Without this the test does not compile. */
#include <limits.h>
#include <unistd.h>

#endif
