#ifndef _STDINT_H
#define _STDINT_H

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed short       int16_t;
typedef unsigned short     uint16_t;
typedef signed int         int32_t;
typedef unsigned int       uint32_t;
typedef signed long long   int64_t;
typedef unsigned long long uint64_t;
typedef long               intptr_t;
typedef unsigned long      uintptr_t;

/* Limits used by the shared SDK on both narrow and wide targets. */
#define UINT64_MAX ((uint64_t)-1)
#define UINTPTR_MAX ((uintptr_t)-1)

#endif
