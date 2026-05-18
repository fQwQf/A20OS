#ifndef _A20_PTHREAD_ARCH_H
#define _A20_PTHREAD_ARCH_H

static inline uintptr_t __get_tp()
{
    uintptr_t tp;
    __asm__ volatile("mv %0, tp" : "=r"(tp));
    return tp;
}

#define MC_PC pc
#define MD_CNT 1

#endif
