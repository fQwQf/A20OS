#ifndef _ARCH_ARMV7M_SIGNAL_ABI_H
#define _ARCH_ARMV7M_SIGNAL_ABI_H

#include "core/types.h"
#include "trap_frame.h"

#define ARCH_SIGSET_SIZE 8

typedef struct {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    union {
        int32_t _sifields[29];
        struct {
            int32_t si_pid;
            int32_t si_uid;
            int32_t si_status;
            int32_t _pad[26];
        } _kill;
    };
} arch_siginfo_t;

typedef struct arch_sigaltstack {
    uintptr_t ss_sp;
    int32_t ss_flags;
    uint32_t ss_size;
} arch_sigaltstack_t;

typedef arch_sigaltstack_t arch_stack_t;

typedef struct {
    uint32_t bits[2];
} arch_sigset_t;

typedef struct {
    uintptr_t handler;
    uint32_t flags;
    uint32_t restorer;
    uint32_t mask[2];
} arch_user_sigaction_t;

typedef struct arch_ucontext {
    uint32_t          uc_flags;
    uintptr_t         uc_link;
    arch_stack_t      uc_stack;
    arch_sigset_t     uc_sigmask;
    ARCH_UCONTEXT_PAD_FIELDS
    arch_sigcontext_t uc_mcontext;
} arch_ucontext_t;

typedef struct {
    uint32_t        flag;
    arch_ucontext_t uc;
    arch_siginfo_t  info;
    uint32_t        arch_extra;
    uint32_t        tramp[2];
} arch_sig_rt_frame_t;

static inline size_t arch_sigframe_flag_offset(void) { return __builtin_offsetof(arch_sig_rt_frame_t, flag); }
static inline size_t arch_sigframe_uc_offset(void) { return __builtin_offsetof(arch_sig_rt_frame_t, uc); }
static inline size_t arch_sigframe_info_offset(void) { return __builtin_offsetof(arch_sig_rt_frame_t, info); }
static inline size_t arch_sigframe_tramp_offset(void) { return __builtin_offsetof(arch_sig_rt_frame_t, tramp); }
static inline uint64_t arch_sigframe_flag_get(const arch_sig_rt_frame_t *frame) { return frame->flag; }
static inline void arch_sigframe_flag_set(arch_sig_rt_frame_t *frame, uint64_t val) { frame->flag = (uint32_t)val; }
static inline size_t arch_sigframe_size(void) { return sizeof(arch_sig_rt_frame_t); }
static inline arch_sigset_t *arch_ucontext_sigmask_ptr(arch_ucontext_t *uc) { return &uc->uc_sigmask; }
static inline const arch_sigset_t *arch_ucontext_sigmask_const_ptr(const arch_ucontext_t *uc) { return &uc->uc_sigmask; }
static inline void arch_ucontext_sigmask_set(arch_ucontext_t *uc, uint64_t mask) {
    uc->uc_sigmask.bits[0] = (uint32_t)((mask >> 1) & 0xffffffffU);
    uc->uc_sigmask.bits[1] = (uint32_t)((mask >> 33) & 0xffffffffU);
}
static inline void arch_sigaction_set_handler(arch_user_sigaction_t *act, uintptr_t handler) { act->handler = (uint32_t)handler; }
static inline void arch_sigaction_set_flags(arch_user_sigaction_t *act, uint64_t flags) { act->flags = (uint32_t)flags; }
static inline void arch_sigaction_set_mask(arch_user_sigaction_t *act, uint64_t mask) {
    uint64_t user_mask = mask >> 1;
    act->mask[0] = (uint32_t)user_mask;
    act->mask[1] = (uint32_t)(user_mask >> 32);
}
static inline uintptr_t arch_sigaction_get_handler(const arch_user_sigaction_t *act) { return act->handler; }
static inline uint64_t arch_sigaction_get_flags(const arch_user_sigaction_t *act) { return act->flags; }
static inline uint64_t arch_sigaction_get_mask(const arch_user_sigaction_t *act) {
    uint64_t user_mask = ((uint64_t)act->mask[1] << 32) | act->mask[0];
    return user_mask << 1;
}
static inline arch_sigset_t arch_user_sigset_from_kernel(uint64_t mask) {
    arch_sigset_t set = { .bits = { (uint32_t)((mask >> 1) & 0xffffffffU), (uint32_t)((mask >> 33) & 0xffffffffU) } };
    return set;
}
static inline uint64_t arch_user_sigset_to_kernel(const arch_sigset_t *mask) {
    return ((uint64_t)mask->bits[0] << 1) | ((uint64_t)mask->bits[1] << 33);
}
static inline arch_siginfo_t *arch_sigframe_info_ptr(arch_sig_rt_frame_t *frame) { return &frame->info; }
static inline arch_ucontext_t *arch_sigframe_ucontext_ptr(arch_sig_rt_frame_t *frame) { return &frame->uc; }
static inline uint32_t *arch_sigframe_tramp_ptr(arch_sig_rt_frame_t *frame) { return frame->tramp; }
static inline uint32_t *arch_sigframe_extra_ptr(arch_sig_rt_frame_t *frame) { return &frame->arch_extra; }

#endif
