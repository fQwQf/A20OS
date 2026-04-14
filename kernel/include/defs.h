#ifndef _DEFS_H
#define _DEFS_H

#include "types.h"

#define ALWAYS_INLINE __attribute__((always_inline)) inline static
#define NORETURN __attribute__((noreturn))
#define PACKED __attribute__((packed))
#define ALIGNED(n) __attribute__((aligned(n)))

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ROUND_UP(a, b) (((a) + (b) - 1) & ~((b) - 1))
#define ROUND_DOWN(a, b) ((a) & ~((b) - 1))
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))
#define ALIGN_UP(x, a) ROUND_UP(x, a)
#define ALIGN_DOWN(x, a) ROUND_DOWN(x, a)

#define BIT(n) (1UL << (n))
#define BITS(h, l) (((1UL << ((h) - (l) + 1)) - 1) << (l))

/* Memory barriers */
#define mb()  __asm__ __volatile__("fence iorw,iorw" ::: "memory")
#define rmb() __asm__ __volatile__("fence ir,ir" ::: "memory")
#define wmb() __asm__ __volatile__("fence ow,ow" ::: "memory")

/* CSR read/write helpers */
static inline uint64_t csr_read(uint64_t csr) {
    uint64_t val;
    __asm__ __volatile__("csrr %0, %1" : "=r"(val) : "i"(csr));
    return val;
}
static inline void csr_write(uint64_t csr, uint64_t val) {
    __asm__ __volatile__("csrw %0, %1" :: "i"(csr), "r"(val));
}
static inline void csr_set(uint64_t csr, uint64_t val) {
    __asm__ __volatile__("csrs %0, %1" :: "i"(csr), "r"(val));
}
static inline void csr_clear(uint64_t csr, uint64_t val) {
    __asm__ __volatile__("csrc %0, %1" :: "i"(csr), "r"(val));
}

/* Named CSR accessors */
#define r_sstatus()    ({ uint64_t x; __asm__ volatile("csrr %0, sstatus" : "=r"(x)); x; })
#define w_sstatus(x)   __asm__ volatile("csrw sstatus, %0" :: "r"((uint64_t)(x)))
#define r_sie()        ({ uint64_t x; __asm__ volatile("csrr %0, sie" : "=r"(x)); x; })
#define w_sie(x)       __asm__ volatile("csrw sie, %0" :: "r"((uint64_t)(x)))

#define SIE_SSIE       (1L << 1)
#define SIE_STIE       (1L << 5)
#define SIE_SEIE       (1L << 9)
#define r_stvec()      ({ uint64_t x; __asm__ volatile("csrr %0, stvec" : "=r"(x)); x; })
#define w_stvec(x)     __asm__ volatile("csrw stvec, %0" :: "r"((uint64_t)(x)))
#define r_scause()     ({ uint64_t x; __asm__ volatile("csrr %0, scause" : "=r"(x)); x; })
#define r_sepc()       ({ uint64_t x; __asm__ volatile("csrr %0, sepc" : "=r"(x)); x; })
#define w_sepc(x)      __asm__ volatile("csrw sepc, %0" :: "r"((uint64_t)(x)))
#define r_stval()      ({ uint64_t x; __asm__ volatile("csrr %0, stval" : "=r"(x)); x; })
#define r_sscratch()   ({ uint64_t x; __asm__ volatile("csrr %0, sscratch" : "=r"(x)); x; })
#define w_sscratch(x)  __asm__ volatile("csrw sscratch, %0" :: "r"((uint64_t)(x)))
#define r_satp()       ({ uint64_t x; __asm__ volatile("csrr %0, satp" : "=r"(x)); x; })
#define w_satp(x)      __asm__ volatile("csrw satp, %0" :: "r"((uint64_t)(x)))
#define r_sip()        ({ uint64_t x; __asm__ volatile("csrr %0, sip" : "=r"(x)); x; })
#define w_sip(x)       __asm__ volatile("csrw sip, %0" :: "r"((uint64_t)(x)))

/* scause exception codes */
#define IRQ_S_SOFT       1
#define IRQ_S_TIMER      5
#define IRQ_S_EXT        9
#define CAUSE_ECALL_U    8
#define MAKE_SATP(pgdir) ((0x8UL << 60) | ((uint64_t)(uintptr_t)(pgdir) >> 12))

#endif /* _DEFS_H */
