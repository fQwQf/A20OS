#if !defined(A20_BSD_PORTABLE_LP64)
#include_next <machine/endian.h>
#else
#ifndef A20_BSD_ENDIAN_H
#define A20_BSD_ENDIAN_H
#include <sys/_types.h>
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error The network LP64 personality currently supports little endian targets
#endif
#define _LITTLE_ENDIAN 1234
#define _BIG_ENDIAN 4321
#define _PDP_ENDIAN 3412
#define _BYTE_ORDER _LITTLE_ENDIAN
#define _QUAD_HIGHWORD 1
#define _QUAD_LOWWORD 0
#if __BSD_VISIBLE
#define LITTLE_ENDIAN _LITTLE_ENDIAN
#define BIG_ENDIAN _BIG_ENDIAN
#define PDP_ENDIAN _PDP_ENDIAN
#define BYTE_ORDER _BYTE_ORDER
#endif
#define __bswap16(x) __builtin_bswap16((__uint16_t)(x))
#define __bswap32(x) __builtin_bswap32((__uint32_t)(x))
#define __bswap64(x) __builtin_bswap64((__uint64_t)(x))
#define __ntohl(x) __bswap32(x)
#define __ntohs(x) __bswap16(x)
#define __htonl(x) __bswap32(x)
#define __htons(x) __bswap16(x)
#endif
#endif
