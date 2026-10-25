/* RV32 H backend: Sv32x4 G-stage, banked VS state and RV32 Sstc pairs.
 * Public vCPU GPR/arch/tramp fields remain uint64_t; asm stores zero-extended
 * XLEN values. A dedicated trap frame preserves guest FP and guest sp/tp. */
#include "hyp/hyp_arch.h"
#include "hyp/hyp_vcpu.h"
#include "arch/riscv32/include/platform.h"
#include "arch/riscv32/include/page_table.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "core/trap.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "core/cpu.h"
#include "proc/proc.h"
#include "hyp_asm_offsets.h"
_Static_assert(__builtin_offsetof(hyp_vcpu_t, fpu) == HYP_ASM_VCPU_FPU_OFF, "FPU offset");
_Static_assert(__builtin_offsetof(hyp_vcpu_t, fcsr) == HYP_ASM_VCPU_FCSR_OFF, "FCSR offset");

extern int riscv32_fdt_has_isa_extension(const char *);
static int hyp_probe(void)
{
    static int probed = -1;
    int value = __atomic_load_n(&probed, __ATOMIC_ACQUIRE);
    if (value >= 0) return value;
    value = riscv32_fdt_has_isa_extension("h") &&
            riscv32_fdt_has_isa_extension("sstc");
    if (value) {
        uint32_t hstatus;
        __asm__ volatile("csrr %0, hstatus" : "=r"(hstatus));
        (void)hstatus;
    }
    __atomic_store_n(&probed, value, __ATOMIC_RELEASE);
    return value;
}

static uint64_t hyp_run_start[CONFIG_NR_CPUS];

int hyp_supported(void)
{
    return hyp_probe();
}

static uint32_t hyp_make_hgatp(pte_t *root, uint16_t vmid)
{

    uint32_t ppn = va_to_pa(root) >> PAGE_SIZE_BITS;
    return (1UL << 31) | ((uint32_t)vmid << 22) | (uint32_t)ppn;
}

void hyp_arch_vmid_fenced(uint16_t vmid)
{

    (void)vmid;
}

void hyp_arch_s2_fence(uint16_t vmid, uint64_t gpa)
{

    uint64_t page = gpa >> 2;
    __asm__ volatile(".insn r 0x73, 0, 0x31, x0, %0, %1"
                     :: "r"((uint32_t)page), "r"((uint32_t)vmid) : "memory");
}

uint64_t hyp_arch_vsatp_get(void)
{
    uint32_t v;
    __asm__ volatile("csrr %0, 0x280" : "=r"(v));
    return v;
}

void hyp_arch_vsatp_set(uint64_t satp)
{
    __asm__ volatile("csrw 0x280, %0" :: "r"((uint32_t)satp) : "memory");
}

uint64_t hyp_arch_vstimecmp_get(void)
{
    uint32_t lo, hi;
    __asm__ volatile("csrr %0, 0x24d" : "=r"(lo));
    __asm__ volatile("csrr %0, 0x25d" : "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}
void hyp_arch_vstimecmp_set(uint64_t deadline)
{

    __asm__ volatile("csrw 0x24d, %0" :: "r"((uint32_t)-1) : "memory");
    __asm__ volatile("csrw 0x25d, %0" :: "r"((uint32_t)(deadline >> 32)) : "memory");
    __asm__ volatile("csrw 0x24d, %0" :: "r"((uint32_t)deadline) : "memory");
}

void hyp_arch_host_tlb_fence(void)
{

    __asm__ volatile(".insn r 0x73, 0, 0x11, x0, x0, x0" ::: "memory");
}

#define HYP_HSTATUS_SPV  (1ULL << 7)
#define HYP_HSTATUS_SPVP (1ULL << 8)
#define HYP_HSTATUS_HU   (1ULL << 9)
#define HYP_HSTATUS_VTW  (1ULL << 21)
#define HYP_HSTATUS_VTVM (1ULL << 20)

#define HYP_HSTATUS_GUEST_ON  (HYP_HSTATUS_SPV | HYP_HSTATUS_SPVP | HYP_HSTATUS_VTVM | HYP_HSTATUS_VTW)
#define HYP_HSTATUS_GUEST_OFF (HYP_HSTATUS_GUEST_ON | HYP_HSTATUS_HU)

#define HYP_HEDELEG_DEFAULT ((1ULL << 0)  | (1UL << 1)  | (1ULL << 2)  | \
                             (1ULL << 3)  | (1ULL << 4)  | (1ULL << 5)  | \
                             (1ULL << 6)  | (1ULL << 7)  | (1ULL << 8)  | \
                             (1ULL << 12) | (1ULL << 13) | (1ULL << 15) | \
                             (1ULL << 18))

void __attribute__((noreturn)) hyp_arch_vcpu_exit(hyp_vcpu_t *vcpu);

extern void hyp_guest_trap_entry(void);

_Static_assert(__builtin_offsetof(hyp_vcpu_t, regs) == HYP_ASM_VCPU_REGS_OFF,
               "hyp_vcpu_asm.S indexes regs[] at HYP_ASM_VCPU_REGS_OFF");
_Static_assert(__builtin_offsetof(hyp_vcpu_t, pc) == HYP_ASM_VCPU_PC_OFF,
               "hyp_vcpu_asm.S indexes pc at HYP_ASM_VCPU_PC_OFF");
_Static_assert(__builtin_offsetof(hyp_vcpu_t, arch) == HYP_ASM_VCPU_ARCH_OFF,
               "hyp_vcpu_asm.S indexes arch[] at HYP_ASM_VCPU_ARCH_OFF");
_Static_assert(__builtin_offsetof(hyp_vcpu_t, tramp) == HYP_ASM_VCPU_TRAMP_OFF,
               "hyp_vcpu_asm.S indexes tramp[] at HYP_ASM_VCPU_TRAMP_OFF");

_Static_assert(sizeof(((hyp_vcpu_t *)0)->tramp) / sizeof(uint64_t) ==
                   HYP_ASM_TRAMP_SLOTS,
               "HYP_VCPU_TRAMP_U64 and HYP_ASM_TRAMP_SLOTS disagree");
_Static_assert(HYP_ASM_TRAMP_X0 + 31 < HYP_ASM_TRAMP_SLOTS,
               "tramp[] must hold guest x[31]");

static uint32_t hyp_read_hstatus(void)
{
    uint32_t v;
    __asm__ volatile("csrr %0, hstatus" : "=r"(v));
    return v;
}

static void hyp_write_hstatus(uint32_t v)
{
    __asm__ volatile("csrw hstatus, %0" :: "r"((uint32_t)v) : "memory");
}

static uint32_t hyp_read_htval(void)
{
    uint32_t v;
    __asm__ volatile("csrr %0, htval" : "=r"(v));
    return v;
}

int hyp_arch_vcpu_setup(hyp_vcpu_t *vcpu)
{
    if (!hyp_supported())
        return -EOPNOTSUPP;
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC ||
        !vcpu->vm || vcpu->vm->magic != HYP_VM_MAGIC)
        return -EINVAL;

    __asm__ volatile("csrw hedeleg, %0" :: "r"((uint32_t)HYP_HEDELEG_DEFAULT) : "memory");
    __asm__ volatile("csrw hideleg, %0" :: "r"((uint32_t)HYP_HIDELEG_DEFAULT) : "memory");

    uint32_t hcounteren;
    __asm__ volatile("csrr %0, 0x606" : "=r"(hcounteren));
    __asm__ volatile("csrw 0x606, %0" :: "r"(hcounteren | (1UL << 1)) : "memory");
    uint32_t henvcfg;
    /* STCE is bit 31 of henvcfgh on RV32, gated by menvcfgh.STCE. */
    __asm__ volatile("csrr %0, 0x61a" : "=r"(henvcfg));
    __asm__ volatile("csrw 0x61a, %0" :: "r"(henvcfg | (1UL << 31)) : "memory");
    __asm__ volatile("csrr %0, 0x61a" : "=r"(henvcfg));
    if (!(henvcfg & (1UL << 31)))
        return -EOPNOTSUPP;

    uint64_t tick_slice = ARCH_TIMER_FREQ / 100;
    uint64_t tick_now = timer_get_ticks();
    hyp_run_start[cpu_current_id()] = tick_now;

    /* SBI arms the host timer separately from the guest comparator. */
    timer_set_interval(tick_slice);
    __asm__ volatile("csrs sie, %0" :: "r"(1UL << 5) : "memory");
    uint32_t hgatp = hyp_make_hgatp(vcpu->vm->s2_root, vcpu->vm->vmid);
    __asm__ volatile("csrw hgatp, %0" :: "r"(hgatp) : "memory");

    __asm__ volatile(".word 0x0000100f" ::: "memory");

    uint32_t hs = hyp_read_hstatus();
    hs &= ~(HYP_HSTATUS_HU | (0x3fUL << 12));
    hyp_write_hstatus(hs | HYP_HSTATUS_GUEST_ON);

    hyp_arch_vstimecmp_set(vcpu->timer_deadline);

    __asm__ volatile("csrw 0x200, %0" :: "r"((uint32_t)vcpu->vsstatus) : "memory");
    __asm__ volatile("csrw 0x204, %0" :: "r"((uint32_t)vcpu->vsie) : "memory");
    __asm__ volatile("csrw 0x205, %0" :: "r"((uint32_t)vcpu->vstvec) : "memory");
    __asm__ volatile("csrw 0x240, %0" :: "r"((uint32_t)vcpu->vsscratch) : "memory");
    __asm__ volatile("csrw 0x241, %0" :: "r"((uint32_t)vcpu->vsepc) : "memory");
    __asm__ volatile("csrw 0x242, %0" :: "r"((uint32_t)vcpu->vscause) : "memory");
    __asm__ volatile("csrw 0x243, %0" :: "r"((uint32_t)vcpu->vstval) : "memory");
    __asm__ volatile("csrw 0x280, %0" :: "r"((uint32_t)vcpu->vsatp) : "memory");
    __asm__ volatile("csrw 0x645, %0" :: "r"((uint32_t)(vcpu->hvip & HYP_HIDELEG_DEFAULT)) : "memory");

    __asm__ volatile(".insn r 0x73, 0, 0x31, x0, x0, x0" ::: "memory");
    __asm__ volatile(".insn r 0x73, 0, 0x11, x0, x0, x0" ::: "memory");
    return 0;
}

static void hyp_frame_to_vcpu(hyp_vcpu_t *vcpu, trap_context_t *ctx)
{
    for (int i = 1; i < 32; i++)
        vcpu->regs[i] = vcpu->tramp[HYP_ASM_TRAMP_X0 + i];
    vcpu->regs[0] = 0;
    vcpu->regs[2] = vcpu->tramp[HYP_ASM_TRAMP_GUEST_SP];
    vcpu->pc = TRAP_CTX_EPC(ctx);

    vcpu->arch[23] = !(ctx->sstatus & (1ULL << 8));
    for (int i = 0; i < 32; ++i)
        vcpu->fpu[i] = ctx->f[i];
    vcpu->fcsr = ctx->fcsr;
}

static void hyp_arm_guest_trap_channel(hyp_vcpu_t *vcpu)
{
    vcpu->tramp[HYP_ASM_TRAMP_HOST_SP] = vcpu->arch[1];
    vcpu->tramp[HYP_ASM_TRAMP_HOST_TP] = vcpu->arch[3];
    __asm__ volatile("csrw sscratch, %0"
                     :: "r"((uintptr_t)&vcpu->tramp[0]) : "memory");
}

static void hyp_vcpu_to_frame(hyp_vcpu_t *vcpu, trap_context_t *ctx)
{
    for (int i = 1; i < 32; i++)
        ctx->x[i] = vcpu->regs[i];
    TRAP_CTX_EPC(ctx) = vcpu->pc;
}

int hyp_arch_guest_trap(void *trap_frame)
{
    hyp_vcpu_t *vcpu = hyp_vcpu_active();

    if (!vcpu || !vcpu->running)
        return 0;

    if (!(hyp_read_hstatus() & HYP_HSTATUS_SPV))
        return 0;

    trap_context_t *ctx = trap_frame;
    uint64_t scause = arch_read_cause();

    __asm__ volatile("csrw stvec, %0" :: "r"((uintptr_t)__trap_from_kernel) : "memory");

    /* The trap prelude banked GPRs before switching host sp and tp. */
    hyp_frame_to_vcpu(vcpu, ctx);

    if (scause & CAUSE_INTR_MASK)
        arch_handle_irq(scause & CAUSE_CODE_MASK, 0);

    int resume = hyp_vcpu_handle_trap(vcpu, ctx, scause, arch_read_tval(),
                                      hyp_read_htval());
    if (__atomic_load_n(&vcpu->vm->broken, __ATOMIC_ACQUIRE)) {
        vcpu->exit = HYP_EXIT_ERROR; resume = 0;
    }
    if (scause & CAUSE_INTR_MASK) { vcpu->exit = HYP_EXIT_YIELD; resume = 0; }
    if (resume && vcpu->exit == HYP_EXIT_YIELD) resume = 0;
    if (resume) hyp_vcpu_sync_irqs(vcpu);
    if (resume && hyp_vcpu_control_exit(vcpu)) resume = 0;

    if (resume && vcpu->vm->managed_console) {
        unsigned run_cpu = cpu_current_id();
        if (timer_get_ticks() - hyp_run_start[run_cpu] >=
            ARCH_TIMER_FREQ / 100) {
            vcpu->exit = HYP_EXIT_YIELD;
            resume = 0;
        }
    }
    if (resume && vcpu->vm->managed_console &&
        ((scause & CAUSE_INTR_MASK) || hyp_console_pending(vcpu->vm))) {
        vcpu->exit = HYP_EXIT_YIELD;
        resume = 0;
    }
    if (resume) {
        hyp_vcpu_to_frame(vcpu, ctx);

        __asm__ volatile("csrw stvec, %0" :: "r"((uintptr_t)hyp_guest_trap_entry)
                         : "memory");
        hyp_arm_guest_trap_channel(vcpu);
        return 1;
    }

    vcpu->running = 0;
    hyp_arch_vcpu_exit(vcpu);
}

int arch_hyp_guest_trap(trap_context_t *ctx)
{
    return hyp_arch_guest_trap(ctx);
}

static __attribute__((unused)) void hyp_riscv_put(hyp_vcpu_t *vcpu)
{
    uint32_t live;
    __asm__ volatile("csrr %0, 0x645" : "=r"(live));
    vcpu->hvip = live;
    vcpu->timer_deadline = hyp_arch_vstimecmp_get();
    hyp_arch_vstimecmp_set((uint64_t)-1);
    __asm__ volatile("csrr %0, 0x200" : "=r"(live)); vcpu->vsstatus = live;
    __asm__ volatile("csrr %0, 0x204" : "=r"(live)); vcpu->vsie = live;
    __asm__ volatile("csrr %0, 0x205" : "=r"(live)); vcpu->vstvec = live;
    __asm__ volatile("csrr %0, 0x240" : "=r"(live)); vcpu->vsscratch = live;
    __asm__ volatile("csrr %0, 0x241" : "=r"(live)); vcpu->vsepc = live;
    __asm__ volatile("csrr %0, 0x242" : "=r"(live)); vcpu->vscause = live;
    __asm__ volatile("csrr %0, 0x243" : "=r"(live)); vcpu->vstval = live;
    __asm__ volatile("csrr %0, 0x280" : "=r"(live)); vcpu->vsatp = live;
    __asm__ volatile("csrw 0x280, zero" ::: "memory");
    __asm__ volatile("csrw hgatp, zero" ::: "memory");
    __asm__ volatile("csrw 0x645, zero" ::: "memory");
    __asm__ volatile(".insn r 0x73, 0, 0x31, x0, x0, x0" ::: "memory");
}

static __attribute__((unused)) void hyp_riscv_sync_irqs(hyp_vcpu_t *vcpu,
                                                       unsigned set, unsigned clear)
{
    (void)vcpu;
    uint32_t on = 0, off = 0;
    const unsigned bit[] = {2, 6, 10};
    for (unsigned i = 0; i < 3; i++) {
        if (set & (1u << i)) on |= 1ULL << bit[i];
        if (clear & (1u << i)) off |= 1ULL << bit[i];
    }
    uint32_t live;
    __asm__ volatile("csrr %0, 0x645" : "=r"(live));
    live = (live & ~off) | on;
    __asm__ volatile("csrw 0x645, %0" :: "r"(live) : "memory");
}

#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
extern int hyp_riscv_probe_smp(hyp_vm_t *);
extern int hyp_riscv_remote_fence(hyp_vm_t *, uint32_t, unsigned);
extern void hyp_riscv_reset(hyp_vcpu_t *, uint64_t, uint64_t, uint64_t);
extern int hyp_riscv_vm_init(hyp_vm_t *vm);
extern void hyp_riscv_vm_destroy(hyp_vm_t *vm);
extern int hyp_riscv_s2_map(hyp_vm_t *, uint64_t, pfn_t, pte_t);
extern int hyp_riscv_s2_pin(hyp_vm_t *, uint64_t, pfn_t *);
extern int hyp_riscv_s2_unmap(hyp_vm_t *, uint64_t);
extern paddr_t hyp_riscv_s2_translate(hyp_vm_t *, uint64_t);
extern int hyp_riscv_s2_audit(hyp_vm_t *, mm_pt_audit_report_t *);
static int hyp_riscv_set_boot(hyp_vcpu_t *vcpu, uint64_t hartid, uint64_t dtb)
{
    vcpu->regs[10] = hartid;
    vcpu->regs[11] = dtb;
    return 0;
}

const hyp_backend_t *hyp_arch_backend(void)
{
    static const hyp_backend_t backend = {
        .name = "riscv32-h", .s2_root_order = 2, .max_running_vcpus = CONFIG_NR_CPUS,
        .probe_smp = hyp_riscv_probe_smp, .remote_fence = hyp_riscv_remote_fence,
        .reset = hyp_riscv_reset,
        .gpa_limit = 1ULL << 34, .ram_base = 0x80000000ULL,
        .vm_init = hyp_riscv_vm_init, .vm_destroy = hyp_riscv_vm_destroy,
        .pin = hyp_riscv_s2_pin, .sync_irqs = hyp_riscv_sync_irqs,
        .map = hyp_riscv_s2_map, .unmap = hyp_riscv_s2_unmap,
        .translate = hyp_riscv_s2_translate, .audit = hyp_riscv_s2_audit,
        .set_boot = hyp_riscv_set_boot,
        .load = hyp_arch_vcpu_setup, .enter = hyp_arch_vcpu_enter,
        .put = hyp_riscv_put,
    };
    return &backend;
}

#endif
