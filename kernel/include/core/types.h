#ifndef _TYPES_H
#define _TYPES_H

typedef __UINT8_TYPE__     uint8_t;
typedef __UINT16_TYPE__    uint16_t;
typedef __UINT32_TYPE__    uint32_t;
typedef __UINT64_TYPE__    uint64_t;
typedef __INT8_TYPE__      int8_t;
typedef __INT16_TYPE__     int16_t;
typedef __INT32_TYPE__     int32_t;
typedef __INT64_TYPE__     int64_t;
typedef __SIZE_TYPE__      size_t;
typedef __PTRDIFF_TYPE__   ssize_t;
typedef __UINTPTR_TYPE__   uintptr_t;
typedef __INTPTR_TYPE__    intptr_t;

#if defined(CONFIG_32BIT)
typedef uint32_t           reg_t;
typedef uint32_t           pte_t;
typedef uint32_t           paddr_t;
typedef uint32_t           vaddr_t;
#elif defined(CONFIG_64BIT)
typedef uint64_t           reg_t;
typedef uint64_t           pte_t;
typedef uint64_t           paddr_t;
typedef uint64_t           vaddr_t;
#else
typedef __UINTPTR_TYPE__   reg_t;
typedef __UINTPTR_TYPE__   pte_t;
typedef __UINTPTR_TYPE__   paddr_t;
typedef __UINTPTR_TYPE__   vaddr_t;
#endif

typedef pte_t pt_root_t;

typedef _Bool bool;
#define true  1
#define false 0
#define NULL  ((void *)0)

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)-1)
#endif

/* Process states */
typedef enum {
    PROC_UNUSED = 0,
    PROC_READY,
    PROC_RUNNING,
    PROC_BLOCKED,
    PROC_STOPPED,
    PROC_ZOMBIE
} proc_state_t;

#endif /* _TYPES_H */
