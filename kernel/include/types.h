#ifndef _TYPES_H
#define _TYPES_H

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long      uint64_t;
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long        int64_t;
typedef unsigned long      size_t;
typedef long               ssize_t;
typedef unsigned long      uintptr_t;
typedef long               intptr_t;
typedef unsigned long      paddr_t;
typedef unsigned long      vaddr_t;

typedef _Bool bool;
#define true  1
#define false 0
#define NULL  ((void *)0)

/* Process states */
typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_ZOMBIE
} proc_state_t;

#endif /* _TYPES_H */
