#ifdef CONFIG_CLUSTER_SSI
#include "cluster/arch_state.h"
#include "core/string.h"
struct ssi_context {
    uint64_t regs[18]; /* user registers through RFLAGS; never kernel fields */
    uint64_t fs, gs;
    uint8_t fpu[512] __attribute__((aligned(16)));
};
_Static_assert(sizeof(struct ssi_context) == CLUSTER_ARCH_STATE_SIZE,
               "SSI1 x86 wire ABI");
void arch_cluster_state_capture(cluster_arch_state_t *state,
                                trap_context_t *ctx) {
    struct ssi_context *w = (struct ssi_context *)state;
    memcpy(w->regs, ctx, sizeof(w->regs));
    w->fs = proc_current()->arch_tls_fs_base;
    w->gs = proc_current()->arch_tls_gs_base;
    __asm__ volatile("fxsave64 (%0)" : : "r"(w->fpu) : "memory");
}
int arch_cluster_state_valid(const cluster_arch_state_t *state) {
    const struct ssi_context *w = (const struct ssi_context *)state;
    uint32_t mxcsr;
    memcpy(&mxcsr, w->fpu + 24, 4);
    return w->regs[16] < USER_VA_LIMIT && w->regs[7] < USER_VA_LIMIT &&
           w->fs < USER_VA_LIMIT && w->gs < USER_VA_LIMIT &&
           !(mxcsr & ~0xffbfU);
}
void arch_cluster_state_restore(const cluster_arch_state_t *state, task_t *t,
                                trap_context_t *ctx, int live) {
    const struct ssi_context *w = (const struct ssi_context *)state;
    memcpy(ctx, w->regs, sizeof(w->regs));
    ctx->rflags = (ctx->rflags & 0xcd5U) | 0x202U;
    ctx->user_fs_base = t->arch_tls_fs_base = w->fs;
    ctx->user_gs_base = t->arch_tls_gs_base = w->gs;
    if (live) {
        arch_write_user_fs_base(w->fs);
        arch_write_user_gs_base(w->gs);
        __asm__ volatile("fxrstor64 (%0)" : : "r"(w->fpu) : "memory");
    } else {
        task_context_t *base = arch_task_context_base(t->kstack_base, 0, ctx);
        memcpy(base->fpu, w->fpu, 512);
    }
}
void arch_cluster_state_user_entry(const cluster_arch_state_t *state,
                                   uint64_t *ip, uint64_t *sp, uint64_t *tls) {
    const struct ssi_context *w = (const struct ssi_context *)state;
    *ip = w->regs[16];
    *sp = w->regs[7];
    *tls = w->fs;
}
#endif
