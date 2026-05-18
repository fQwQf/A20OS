#ifndef _A20_ATOMIC_H
#define _A20_ATOMIC_H

#include <stdint.h>

#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s)
{
    int old;
    __asm__ volatile(
        "1: lr.w %0, (%2)\n"
        "   bne %0, %3, 1f\n"
        "   sc.w %0, %4, (%2)\n"
        "   bnez %0, 1b\n"
        "1:\n"
        : "=&r"(old)
        : "r"(p), "r"(p), "r"(t), "r"(s)
        : "memory");
    return old;
}

#define a_cas_p a_cas_p
static inline void *a_cas_p(volatile void *p, void *t, void *s)
{
    void *old;
    __asm__ volatile(
        "1: lr.d %0, (%2)\n"
        "   bne %0, %3, 1f\n"
        "   sc.d %0, %4, (%2)\n"
        "   bnez %0, 1b\n"
        "1:\n"
        : "=&r"(old)
        : "r"(p), "r"(p), "r"(t), "r"(s)
        : "memory");
    return old;
}

#define a_fetch_add a_fetch_add
static inline int a_fetch_add(volatile int *p, int v)
{
    int old;
    __asm__ volatile(
        "1: lr.w %0, (%1)\n"
        "   addw %0, %0, %2\n"
        "   sc.w %0, %0, (%1)\n"
        "   bnez %0, 1b\n"
        : "=&r"(old)
        : "r"(p), "r"(v)
        : "memory");
    return old - v;
}

#define a_inc a_inc
static inline void a_inc(volatile int *p)
{
    a_fetch_add(p, 1);
}

#define a_dec a_dec
static inline void a_dec(volatile int *p)
{
    a_fetch_add(p, -1);
}

#define a_and a_and
static inline void a_and(volatile int *p, int v)
{
    int old;
    do { old = *p; } while (a_cas(p, old, old & v) != old);
}

#define a_or a_or
static inline void a_or(volatile int *p, int v)
{
    int old;
    do { old = *p; } while (a_cas(p, old, old | v) != old);
}

#define a_store a_store
static inline void a_store(volatile int *p, int v)
{
    __asm__ volatile("sw %1, (%0)" : : "r"(p), "r"(v) : "memory");
}

#define a_barrier a_barrier
static inline void a_barrier()
{
    __asm__ volatile("fence rw,rw" : : : "memory");
}

#define a_spin a_barrier

#endif
