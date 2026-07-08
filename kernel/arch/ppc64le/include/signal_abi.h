#ifndef _ARCH_PPC64LE_SIGNAL_ABI_H
#define _ARCH_PPC64LE_SIGNAL_ABI_H

#include "core/types.h"
#include "trap_frame.h"

#define ARCH_SIGSET_SIZE sizeof(arch_sigset_t)

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int _sifields[29];
} __attribute__((aligned(16))) arch_siginfo_t;

typedef struct arch_sigaltstack {
    void   *ss_sp;
    int     ss_flags;
    size_t  ss_size;
} arch_sigaltstack_t;

typedef arch_sigaltstack_t arch_stack_t;

typedef struct {
    uint64_t bits[1];
} arch_sigset_t;

typedef struct {
    uintptr_t handler;
    uint64_t  flags;
    uint64_t  mask;
} arch_user_sigaction_t;

typedef struct arch_ucontext {
    uint64_t          uc_flags;
    uintptr_t         uc_link;
    arch_stack_t      uc_stack;
    arch_sigset_t     uc_sigmask;
    ARCH_UCONTEXT_PAD_FIELDS
    arch_sigcontext_t uc_mcontext;
} __attribute__((aligned(16))) arch_ucontext_t;

typedef struct {
    uint64_t        flag;
    arch_ucontext_t uc;
    arch_siginfo_t  info;
    uint64_t        arch_extra;
    uint32_t        tramp[2];
} __attribute__((aligned(16))) arch_sig_rt_frame_t;

static inline size_t arch_sigframe_flag_offset(void) { return __builtin_offsetof(arch_sig_rt_frame_t, flag); }
static inline size_t arch_sigframe_uc_offset(void) { return __builtin_offsetof(arch_sig_rt_frame_t, uc); }
static inline size_t arch_sigframe_info_offset(void) { return __builtin_offsetof(arch_sig_rt_frame_t, info); }
static inline size_t arch_sigframe_tramp_offset(void) { return __builtin_offsetof(arch_sig_rt_frame_t, tramp); }
static inline uint64_t arch_sigframe_flag_get(const arch_sig_rt_frame_t *frame) { return frame->flag; }
static inline void arch_sigframe_flag_set(arch_sig_rt_frame_t *frame, uint64_t val) { frame->flag = val; }
static inline size_t arch_sigframe_size(void) { return sizeof(arch_sig_rt_frame_t); }
static inline arch_sigset_t *arch_ucontext_sigmask_ptr(arch_ucontext_t *uc) { return &uc->uc_sigmask; }
static inline const arch_sigset_t *arch_ucontext_sigmask_const_ptr(const arch_ucontext_t *uc) { return &uc->uc_sigmask; }
static inline void arch_ucontext_sigmask_set(arch_ucontext_t *uc, uint64_t mask) { uc->uc_sigmask.bits[0] = mask >> 1; }
static inline void arch_sigaction_set_handler(arch_user_sigaction_t *act, uintptr_t handler) { act->handler = handler; }
static inline void arch_sigaction_set_flags(arch_user_sigaction_t *act, uint64_t flags) { act->flags = flags; }
static inline void arch_sigaction_set_mask(arch_user_sigaction_t *act, uint64_t mask) { act->mask = mask >> 1; }
static inline uintptr_t arch_sigaction_get_handler(const arch_user_sigaction_t *act) { return act->handler; }
static inline uint64_t arch_sigaction_get_flags(const arch_user_sigaction_t *act) { return act->flags; }
static inline uint64_t arch_sigaction_get_mask(const arch_user_sigaction_t *act) { return act->mask << 1; }
static inline arch_sigset_t arch_user_sigset_from_kernel(uint64_t mask) { arch_sigset_t set = { .bits = { mask >> 1 } }; return set; }
static inline uint64_t arch_user_sigset_to_kernel(const arch_sigset_t *mask) { return mask->bits[0] << 1; }
static inline arch_siginfo_t *arch_sigframe_info_ptr(arch_sig_rt_frame_t *frame) { return &frame->info; }
static inline arch_ucontext_t *arch_sigframe_ucontext_ptr(arch_sig_rt_frame_t *frame) { return &frame->uc; }
static inline uint32_t *arch_sigframe_tramp_ptr(arch_sig_rt_frame_t *frame) { return frame->tramp; }
static inline uint64_t *arch_sigframe_extra_ptr(arch_sig_rt_frame_t *frame) { return &frame->arch_extra; }

#endif /* _ARCH_PPC64LE_SIGNAL_ABI_H */
