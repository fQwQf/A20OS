/* ARMv7 virtualization extensions: an MMU-off HYP monitor separates two
 * independent PL1 contexts. The host remains in SVC with short descriptors;
 * stage-2 always uses the independent 64-bit LPAE descriptor tree. */
#include "hyp/hyp_arch.h"
#include "hyp/hyp_vcpu.h"
#include "hyp/hyp_s2.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "core/cpu.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/timer.h"
#include "hyp_state.h"
extern uint32_t arm32_hyp_boot_cpus[];

static uint32_t monitor_call(uint32_t command, uint32_t argument)
{
    register uint32_t r0 __asm__("r0") = command;
    register uint32_t r1 __asm__("r1") = argument;
    __asm__ volatile(".arch_extension virt\nhvc #0xa20" : "+r"(r0), "+r"(r1) :: "memory", "cc");
    return r0;
}
int hyp_supported(void)
{
#ifdef CONFIG_NOMMU
    return 0;
#else
    unsigned cpu = cpu_current_id();
    /* The symbol resides in the identity-linked physical entry section. */
    uint32_t *flags = (void *)((uintptr_t)arm32_hyp_boot_cpus + PAGE_OFFSET);
    return cpu < CONFIG_NR_CPUS && flags[cpu] == 1;
#endif
}
void hyp_arch_vmid_fenced(uint16_t vmid) { (void)vmid; }
void hyp_arch_s2_fence(uint16_t vmid, uint64_t gpa)
{
    (void)vmid; (void)gpa;
    if (hyp_supported()) monitor_call(2, 0);
}
void hyp_arch_host_tlb_fence(void) { hyp_arch_s2_fence(0, 0); }
uint64_t hyp_arch_vsatp_get(void) { return 0; }
void hyp_arch_vsatp_set(uint64_t value) { (void)value; }
uint64_t hyp_arch_vstimecmp_get(void) { return 0; }
void hyp_arch_vstimecmp_set(uint64_t value) { (void)value; }

#if defined(ARCH_HAS_HYP_S2) && !defined(CONFIG_NOMMU)
static uint64_t stage2_leaf(pte_t prot)
{
    uint64_t leaf = 3 | (15ULL << 2) | (3ULL << 8) | (1ULL << 10);
    if (prot & PTE_R) leaf |= 1ULL << 6;
    if (prot & PTE_W) leaf |= 1ULL << 7;
    if (!(prot & HYP_PROT_EXEC)) leaf |= 1ULL << 54;
    return leaf;
}
static int stage2_invalidate(hyp_vm_t *vm, uint64_t gpa)
{
    (void)vm; (void)gpa;
    if (hyp_supported()) monitor_call(2, 0);
    return 0;
}
static const hyp_s2_format_t stage2_format = {
    .levels = 3, .address_mask = 0xfffff000ULL,
    .valid_mask = 1, .table_flags = 3,
    .leaf_check_mask = 3 | (15ULL << 2) | (3ULL << 6) |
                       (3ULL << 8) | (1ULL << 54),
    .leaf_flags = stage2_leaf, .invalidate = stage2_invalidate,
    .live_update = 1,
};
static int arm32_vm_init(hyp_vm_t *vm) { return hyp_s2_tree_init(vm, &stage2_format); }
static int arm32_smp(hyp_vm_t *vm) { (void)vm; return 0; }

static void init_bank(struct arm32_hyp_state *s)
{
    memset(s, 0, PAGE_SIZE);
    s->guest.cpsr = 0x1d3; /* ARM SVC, interrupts masked at reset. */
    s->guest.sys[0] = 0x00c50078; /* ARMv7 SCTLR RES1, MMU/cache off. */
    s->guest.sys[2] = 0x00f00000; /* CPACR: FP/NEON enabled. */
    s->guest.fpexc = 1u << 30;
}
static void arm32_reset(hyp_vcpu_t *vcpu, uint64_t entry, uint64_t hart, uint64_t opaque)
{
    (void)hart;
    if (vcpu->backend_data) init_bank(vcpu->backend_data);
    memset(vcpu->regs, 0, sizeof(vcpu->regs));
    vcpu->pc = entry;
    vcpu->regs[0] = (uint32_t)opaque;
}
static int arm32_set_boot(hyp_vcpu_t *vcpu, uint64_t hart, uint64_t dtb)
{
    (void)hart;
    if (dtb >> 32) return -EINVAL;
    /* ARM Linux boot: r0=0, r1=~0, r2=DTB. PSCI CPU_ON uses r0=context. */
    vcpu->regs[0] = 0; vcpu->regs[1] = ~0U; vcpu->regs[2] = dtb;
    return 0;
}
static void arm32_destroy_vcpu(hyp_vcpu_t *vcpu)
{
    if (vcpu->backend_data) {
        frame_put(virt_to_pfn(vcpu->backend_data));
        vcpu->backend_data = NULL;
    }
}
int hyp_arch_vcpu_setup(hyp_vcpu_t *vcpu)
{
    if (!hyp_supported()) return -EOPNOTSUPP;
    if (!vcpu || !vcpu->vm || vcpu->magic != HYP_VCPU_MAGIC ||
        (vcpu->pc & 3) || vcpu->pc >> 32) return -EINVAL;
    if (!vcpu->backend_data) {
        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) return -ENOMEM;
        vcpu->backend_data = pfn_to_virt(pfn);
        init_bank(vcpu->backend_data);
    }
    struct arm32_hyp_state *s = vcpu->backend_data;
    s->vttbr = va_to_pa(vcpu->vm->s2_root);
    /* 32-bit IPA, L1 start, WBWA inner-shareable table walks, RES1 bit31. */
    s->vtcr = (1u << 31) | (1u << 6) | (1u << 8) | (1u << 10) | (3u << 12);
    s->hcr = (1u << 21) | (1u << 20) | (1u << 19) | (1u << 14) |
             (1u << 13) | (1u << 5) | (1u << 4) | (1u << 3) | (1u << 1) | 1;
    s->mpidr = 0x80000000u | vcpu->id;
    timer_set_interval(ARCH_TIMER_FREQ / 100);
    return 0;
}
static int guest_call(hyp_vcpu_t *vcpu)
{
    uint32_t fn = vcpu->regs[0];
    int32_t result = -1;
    switch (fn) {
    case 1:
        if (hyp_console_putchar(vcpu->vm, vcpu->regs[1]) == -EAGAIN) {
            vcpu->pc -= 4; vcpu->exit = HYP_EXIT_YIELD; return 0;
        }
        result = 0; break;
    case 8: case 0x84000008: case 0x84000009:
        vcpu->exit = HYP_EXIT_SHUTDOWN; return 0;
    case 0x84000000: case 0x80000000: result = 0x10001; break;
    case 0x84000002: vcpu->exit = HYP_EXIT_HART_STOPPED; return 0;
    case 0x84000003: /* PSCI CPU_ON */
        if (vcpu->regs[2] & 3) { result = -2; break; }
        result = hyp_vm_hart_start(vcpu->vm, (uint32_t)vcpu->regs[1],
                                  (uint32_t)vcpu->regs[2], (uint32_t)vcpu->regs[3]);
        if (result == -EALREADY || result == -EBUSY) result = -4;
        else if (result == -ESHUTDOWN) result = -3;
        else if (result) result = -2;
        break;
    case 0x84000004: { /* AFFINITY_INFO, lowest_affinity_level=0 only. */
        int status = hyp_vm_hart_status(vcpu->vm, (uint32_t)vcpu->regs[1]);
        result = (uint32_t)vcpu->regs[2] || status < 0 ? -2 :
                 status == HYP_HART_STOPPED ? 1 : status == HYP_HART_START_PENDING ? 2 : 0;
        break;
    }
    case 0x8400000a:
        switch ((uint32_t)vcpu->regs[1]) {
        case 0x84000000: case 0x84000002: case 0x84000003: case 0x84000004:
        case 0x84000008: case 0x84000009: case 0x8400000a: result = 0; break;
        }
        break;
    }
    vcpu->regs[0] = (uint32_t)result;
    return 1;
}
static int guest_mmio(hyp_vcpu_t *vcpu, struct arm32_hyp_state *s, uint32_t ipa)
{
    uint32_t iss = s->hsr;
    if (!(iss & (1u << 24)) || (iss & ((1u << 7) | (1u << 10)))) return 0;
    unsigned reg = (iss >> 16) & 15, size = 1u << ((iss >> 22) & 3);
    if (reg == 15 || size > 4 || ipa < 0x09000000 || ipa >= 0x09001000) return 0;
    int write = (iss >> 6) & 1;
    uint32_t value = vcpu->regs[reg], off = ipa & 0xfff;
    if (write && off == 0) {
        if (hyp_console_putchar(vcpu->vm, value) == -EAGAIN) {
            vcpu->exit = HYP_EXIT_YIELD; return 2;
        }
    } else if (!write) {
        value = off == 0x18 ? 0x90 : 0; /* polling PL011, empty RX */
        if (iss & (1u << 21)) {
            if (size == 1) value = (int8_t)value;
            if (size == 2) value = (int16_t)value;
        }
        vcpu->regs[reg] = value;
    }
    vcpu->pc += (s->hsr & (1u << 25)) ? 4 : 2;
    return 1;
}
void hyp_arch_vcpu_enter(hyp_vcpu_t *vcpu)
{
    struct arm32_hyp_state *s = vcpu->backend_data;
    for (;;) {
        if (hyp_vcpu_control_exit(vcpu)) return;
        if (__atomic_load_n(&vcpu->vm->broken, __ATOMIC_ACQUIRE)) {
            vcpu->exit = HYP_EXIT_ERROR; return;
        }
        for (unsigned i = 0; i < 13; i++) s->guest.r[i] = vcpu->regs[i];
        s->guest.banked[2] = vcpu->regs[13]; /* SP_svc */
        s->guest.banked[3] = vcpu->regs[14];
        s->guest.pc = vcpu->pc;
        arch_dma_sync_for_device(s, PAGE_SIZE);
        uint32_t rc = monitor_call(1, va_to_pa(s));
        arch_dma_sync_for_cpu(s, PAGE_SIZE);
        if (rc) { vcpu->exit = HYP_EXIT_ERROR; return; }
        for (unsigned i = 0; i < 13; i++) vcpu->regs[i] = s->guest.r[i];
        vcpu->regs[13] = s->guest.banked[2];
        vcpu->regs[14] = s->guest.banked[3];
        vcpu->pc = s->guest.pc;
        vcpu->exit_scause = s->hsr; vcpu->exit_stval = s->hdfar; vcpu->exit_htval = s->hpfar;
        if (s->kind == 1) { vcpu->exit = HYP_EXIT_YIELD; return; }
        if (s->kind) { vcpu->exit = HYP_EXIT_FAULT; return; }
        unsigned ec = s->hsr >> 26;
        if (ec == 0x12 || ec == 0x13) {
            if (ec == 0x13) vcpu->pc += 4;
            if (!guest_call(vcpu)) return;
        } else if (ec == 1) {
            vcpu->pc += (s->hsr & (1u << 25)) ? 4 : 2;
            vcpu->exit = HYP_EXIT_YIELD; return;
        } else if (ec == 0x20 || ec == 0x24) {
            unsigned fsc = s->hsr & 63;
            if (fsc >= 4 && fsc <= 7) {
                uint32_t far = ec == 0x20 ? s->hifar : s->hdfar;
                uint32_t ipa = ((s->hpfar & 0xfffffff0U) << 8) | (far & 0xfff);
                if (hyp_vm_ram_contains(vcpu->vm, ipa)) {
                    if (!hyp_ram_fill(vcpu->vm, ipa & ~(uint32_t)(PAGE_SIZE - 1))) continue;
                } else if (ec == 0x24) {
                    int mmio = guest_mmio(vcpu, s, ipa);
                    if (mmio == 2) return;
                    if (mmio) continue;
                }
            }
            vcpu->exit = HYP_EXIT_FAULT; return;
        } else { vcpu->exit = HYP_EXIT_FAULT; return; }
        if (vcpu->vm->managed_console && hyp_console_pending(vcpu->vm)) {
            vcpu->exit = HYP_EXIT_YIELD; return;
        }
    }
}
static void arm32_put(hyp_vcpu_t *vcpu) { (void)vcpu; }
const hyp_backend_t *hyp_arch_backend(void)
{
    static const hyp_backend_t backend = {
        .name = "arm32-hyp", .s2_root_order = 0, .max_running_vcpus = CONFIG_NR_CPUS,
        .gpa_limit = 1ULL << 32, .ram_base = 0x40000000,
        .probe_smp = arm32_smp, .reset = arm32_reset,
        .vm_init = arm32_vm_init, .vm_destroy = hyp_s2_tree_destroy,
        .map = hyp_s2_tree_map, .pin = hyp_s2_tree_pin,
        .unmap = hyp_s2_tree_unmap, .translate = hyp_s2_tree_translate,
        .audit = hyp_s2_tree_audit, .set_boot = arm32_set_boot,
        .load = hyp_arch_vcpu_setup, .enter = hyp_arch_vcpu_enter,
        .put = arm32_put, .vcpu_destroy = arm32_destroy_vcpu,
    };
    return &backend;
}
#else
int hyp_arch_vcpu_setup(hyp_vcpu_t *vcpu) { (void)vcpu; return -EOPNOTSUPP; }
void hyp_arch_vcpu_enter(hyp_vcpu_t *vcpu) { (void)vcpu; }
const hyp_backend_t *hyp_arch_backend(void) { return NULL; }
#endif
