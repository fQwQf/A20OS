/* AArch64 non-VHE backend. EL1-only firmware is deliberately unsupported:
 * owning EL2, not merely observing ID_AA64PFR0, is required for isolation. */
#include "hyp/hyp_arch.h"
#include "hyp/hyp_vcpu.h"
#include "hyp/hyp_s2.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "core/cpu.h"
#include "core/smp.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/timer.h"
#include "hyp_state.h"

extern uint64_t aarch64_hyp_boot_cpus[CONFIG_NR_CPUS];

static uint64_t monitor_call(uint64_t command, uint64_t argument)
{
    register uint64_t x0 __asm__("x0") = command;
    register uint64_t x1 __asm__("x1") = argument;
    __asm__ volatile("hvc #0xa20" : "+r"(x0), "+r"(x1) :: "memory", "cc");
    return x0;
}

int hyp_supported(void)
{
#ifdef CONFIG_NOMMU
    return 0;
#else
    unsigned cpu = cpu_current_id();
    return cpu < CONFIG_NR_CPUS && aarch64_hyp_boot_cpus[cpu] == 1;
#endif
}

void hyp_arch_vmid_fenced(uint16_t vmid) { (void)vmid; }
void hyp_arch_s2_fence(uint16_t vmid, uint64_t gpa)
{
    (void)vmid; (void)gpa;
    if (hyp_supported()) monitor_call(2, 0);
}
void hyp_arch_host_tlb_fence(void) { hyp_arch_s2_fence(0, 0); }
/* Compatibility hooks for the RISC-V instruction emulator. AArch64 exits
 * are decoded here and never call those CSR-based paths. */
uint64_t hyp_arch_vsatp_get(void) { return 0; }
void hyp_arch_vsatp_set(uint64_t value) { (void)value; }
uint64_t hyp_arch_vstimecmp_get(void) { return 0; }
void hyp_arch_vstimecmp_set(uint64_t value) { (void)value; }

#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
static uint64_t stage2_leaf(pte_t prot)
{
    /* Stage 2 page: normal WB cacheable, inner shareable, AF=1.
     * S2AP[7:6] is independently readable/writable; XN[54] denies fetch. */
    uint64_t leaf = 3 | (15ULL << 2) | (3ULL << 8) | (1ULL << 10);
    if (prot & PTE_R) leaf |= 1ULL << 6;
    if (prot & PTE_W) leaf |= 1ULL << 7;
    if (!(prot & PTE_X)) leaf |= 1ULL << 54;
    return leaf;
}
static int stage2_invalidate(hyp_vm_t *vm, uint64_t gpa)
{
    (void)vm; (void)gpa;
    /* The generic tree rejects remote active CPUs. A local guest has already
     * returned to EL1 before any update, and entry always invalidates VMID 0. */
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
static int aarch64_vm_init(hyp_vm_t *vm)
{
    return hyp_s2_tree_init(vm, &stage2_format);
}
static int aarch64_smp(hyp_vm_t *vm)
{
    (void)vm;
    return smp_online_cpu_count() > 1 ? 0 : -EOPNOTSUPP;
}

/* EL2 accesses state with its MMU off. Clean to PoC before the call and
 * invalidate after it; a dedicated aligned frame avoids unrelated data in
 * the same cache line. CTR_EL0.DminLine specifies the actual line size. */
static void state_cache(struct aarch64_hyp_state *state, int invalidate)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t stride = 4ULL << ((ctr >> 16) & 15);
    for (uintptr_t p = (uintptr_t)state; p < (uintptr_t)state + PAGE_SIZE; p += stride) {
        if (invalidate) __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
        else __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
static void aarch64_destroy_vcpu(hyp_vcpu_t *vcpu)
{
    if (vcpu->backend_data) {
        frame_put(virt_to_pfn(vcpu->backend_data));
        vcpu->backend_data = NULL;
    }
}

static void aarch64_reset(hyp_vcpu_t *vcpu, uint64_t entry,
                          uint64_t hart, uint64_t opaque)
{
    if (vcpu->backend_data) {
        struct aarch64_hyp_state *s = vcpu->backend_data;
        memset(s, 0, PAGE_SIZE);
        s->guest.pstate = 0x3c5;
        s->guest.sys[0] = 0x30d00800;
        s->guest.sys[2] = 3ULL << 20;
        s->mpidr = 0x80000000ULL | hart;
    }
    memset(vcpu->regs, 0, sizeof(vcpu->regs));
    vcpu->pc = entry;
    vcpu->regs[0] = opaque;
}
static int aarch64_set_boot(hyp_vcpu_t *vcpu, uint64_t hart, uint64_t dtb)
{
    if (hart != vcpu->id) return -EINVAL;
    /* AArch64 Linux/Image boot protocol: x0=DTB, x1..x3=0, EL1h. */
    vcpu->regs[0] = dtb;
    vcpu->regs[1] = vcpu->regs[2] = vcpu->regs[3] = 0;
    return 0;
}

int hyp_arch_vcpu_setup(hyp_vcpu_t *vcpu)
{
    if (!hyp_supported()) return -EOPNOTSUPP;
    if (!vcpu || !vcpu->vm || vcpu->magic != HYP_VCPU_MAGIC ||
        (vcpu->pc & 3)) return -EINVAL;
    if (!vcpu->backend_data) {
        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) return -ENOMEM;
        struct aarch64_hyp_state *s = pfn_to_virt(pfn);
        memset(s, 0, PAGE_SIZE);
        s->guest.pstate = 0x3c5; /* EL1h, DAIF masked at reset. */
        s->guest.sys[0] = 0x30d00800; /* SCTLR_EL1 RES1, MMU/cache off. */
        s->guest.sys[2] = 3ULL << 20; /* FP/SIMD enabled; no SVE. */
        vcpu->backend_data = s;
    }
    struct aarch64_hyp_state *s = vcpu->backend_data;
    s->vttbr = va_to_pa(vcpu->vm->s2_root); /* VMID zero, full flush per entry. */
    /* 39-bit IPA, L1 start, 4K granule, inner shareable WBWA walks;
     * PS=32-bit physical addresses, supported by every Armv8-A EL2 CPU.
     * The currently supported virt machine RAM lies below 4 GiB. */
    if (s->vttbr >> 32) return -EOPNOTSUPP;
    s->vtcr = (1ULL << 31) | 25 | (1ULL << 6) |
              (1ULL << 8) | (1ULL << 10) | (3ULL << 12);
    /* HCR: RW, trap ACTLR/implementation registers/SMC/WFE/WFI,
     * route physical SError/IRQ/FIQ to EL2, force set/way invalidate, VM. */
    s->hcr = (1ULL << 31) | (1ULL << 21) | (1ULL << 20) |
             (1ULL << 19) | (1ULL << 14) | (1ULL << 13) |
             (1ULL << 5) | (1ULL << 4) | (1ULL << 3) | (1ULL << 1) | 1;
    s->mpidr = 0x80000000ULL | vcpu->id;
    uint64_t now, deadline, ctl, freq;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    __asm__ volatile("mrs %0, cntp_cval_el0" : "=r"(deadline));
    __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(ctl));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    uint64_t slice = freq / 100;
    if (!(ctl & 1) || (ctl & 2) || !deadline ||
        (deadline > now && deadline - now > slice)) timer_set_interval(slice);
    return 0;
}

static int guest_mmio(hyp_vcpu_t *vcpu, struct aarch64_hyp_state *s, uint64_t ipa)
{
    uint64_t iss = s->esr;
    if (!(iss & (1ULL << 24)) || (iss & ((1ULL << 7) | (1ULL << 10)))) return 0;
    unsigned reg = (iss >> 16) & 31, size = 1u << ((iss >> 22) & 3);
    int write = (iss >> 6) & 1;
    uint64_t value = reg == 31 ? 0 : vcpu->regs[reg];
    if (ipa >= 0x09000000 && ipa < 0x09001000) {
        /* QEMU virt PL011, polling only. No virtual GIC is advertised. */
        unsigned offset = ipa & 0xfff;
        if (size != 4 && size != 1) return 0;
        if (offset == 0 && write) {
            if (hyp_console_putchar(vcpu->vm, value) == -EAGAIN) {
                vcpu->exit = HYP_EXIT_YIELD;
                return 2;
            }
        }
        else if (offset == 0 && !write) {
            uint64_t flags = spin_lock_irqsave(&vcpu->vm->console_lock);
            unsigned tail = vcpu->vm->rx_tail;
            value = 0;
            if (tail != __atomic_load_n(&vcpu->vm->rx_head, __ATOMIC_ACQUIRE)) {
                value = vcpu->vm->rx_buf[tail];
                __atomic_store_n(&vcpu->vm->rx_tail,
                                (tail + 1) % HYP_VM_RX_RING, __ATOMIC_RELEASE);
            }
            spin_unlock_irqrestore(&vcpu->vm->console_lock, flags);
        } else if (offset == 0x18 && !write) {
            value = 0x90; /* TXFE, RXFE; console ingress handled below. */
            if (vcpu->vm->rx_head != vcpu->vm->rx_tail) value &= ~0x10ULL;
        } else if (!write) value = 0;
    } else return 0;
    if (!write && reg != 31) {
        if (size < 8) value &= (1ULL << (size * 8)) - 1;
        if ((iss & (1ULL << 21)) && size < 8) {
            uint64_t sign = 1ULL << (size * 8 - 1);
            value = (value ^ sign) - sign;
        }
        if (!(iss & (1ULL << 15))) value = (uint32_t)value;
        vcpu->regs[reg] = value;
    }
    vcpu->pc += 4;
    return 1;
}

static int guest_call(hyp_vcpu_t *vcpu)
{
    uint64_t fn = vcpu->regs[0];
    int64_t result = -1; /* PSCI_NOT_SUPPORTED */
    switch (fn) {
    case 1: /* A20 bare-metal smoke console hypercall: x1 is one byte. */
        if (hyp_console_putchar(vcpu->vm, vcpu->regs[1]) == -EAGAIN) {
            vcpu->pc -= 4; /* Retry after the controller drains output. */
            vcpu->exit = HYP_EXIT_YIELD;
            return 0;
        }
        result = 0;
        break;
    case 8:
        vcpu->exit = HYP_EXIT_SHUTDOWN;
        return 0;
    case 0x84000000: result = 0x10001; break; /* PSCI 1.1 */
    case 0x80000000: result = 0x10001; break; /* SMCCC 1.1 */
    case 0x84000008: case 0x84000009:
        vcpu->exit = HYP_EXIT_SHUTDOWN;
        return 0;
    case 0x84000003: case 0xc4000003: { /* CPU_ON: affinity maps to vCPU id. */
        uint64_t hart = vcpu->regs[1];
        int rc = hyp_vm_hart_start(vcpu->vm, hart, vcpu->regs[2], vcpu->regs[3]);
        result = !rc ? 0 : rc == -EALREADY ? -4 : -2;
        break;
    }
    case 0x84000004: case 0xc4000004: { /* AFFINITY_INFO */
        int st = hyp_vm_hart_status(vcpu->vm, vcpu->regs[1]);
        result = st < 0 ? -2 : st == HYP_HART_STOPPED ? 1 :
                 st == HYP_HART_START_PENDING ? 2 : 0;
        break;
    }
    case 0x84000002: /* CPU_OFF */
        vcpu->exit = HYP_EXIT_HART_STOPPED;
        return 0;
    case 0x8400000a: /* PSCI_FEATURES */
        switch ((uint32_t)vcpu->regs[1]) {
        case 0x84000000: case 0x84000002: case 0x84000003:
        case 0xc4000003: case 0x84000004: case 0xc4000004: case 0x84000008:
        case 0x84000009: case 0x8400000a: result = 0; break;
        }
        break;
    }
    vcpu->regs[0] = result;
    /* HVC records the following PC; trapped SMC records the SMC itself. */
    return 1;
}

void hyp_arch_vcpu_enter(hyp_vcpu_t *vcpu)
{
    struct aarch64_hyp_state *s = vcpu->backend_data;
    for (;;) {
        if (hyp_vcpu_control_exit(vcpu)) return;
        if (__atomic_load_n(&vcpu->vm->broken, __ATOMIC_ACQUIRE)) {
            vcpu->exit = HYP_EXIT_ERROR;
            return;
        }
        memcpy(s->guest.x, vcpu->regs, sizeof(s->guest.x));
        s->guest.sp1 = vcpu->regs[31];
        s->guest.pc = vcpu->pc;
        state_cache(s, 0);
        uint64_t rc = monitor_call(1, va_to_pa(s));
        state_cache(s, 1);
        if (rc) { vcpu->exit = HYP_EXIT_ERROR; return; }
        memcpy(vcpu->regs, s->guest.x, sizeof(s->guest.x));
        vcpu->regs[31] = s->guest.sp1;
        vcpu->pc = s->guest.pc;
        vcpu->exit_scause = s->esr;
        vcpu->exit_stval = s->far;
        vcpu->exit_htval = s->hpfar;
        if (s->kind == 1) { vcpu->exit = HYP_EXIT_YIELD; return; }
        if (s->kind) { vcpu->exit = HYP_EXIT_FAULT; return; }
        if (!vcpu->vm->managed_console) hyp_dev_pump_rx(vcpu->vm);
        unsigned ec = s->esr >> 26;
        if (ec == 0x16 || ec == 0x17) { /* HVC64 / SMC64 */
            if (ec == 0x17) vcpu->pc += 4;
            if (!guest_call(vcpu)) return;
        } else if (ec == 1) { /* trapped WFI/WFE */
            vcpu->pc += 4;
            vcpu->exit = HYP_EXIT_YIELD;
            return;
        } else if (ec == 0x20 || ec == 0x24) { /* lower EL instruction/data abort */
            unsigned fsc = s->esr & 63;
            /* HPFAR is valid on translation faults. Permission faults can
             * leave it UNKNOWN, so never demand-map from stale HPFAR. */
            if (fsc >= 4 && fsc <= 7) {
                uint64_t ipa = ((s->hpfar & 0xfffffffff0ULL) << 8) | (s->far & 0xfff);
                if (hyp_vm_ram_contains(vcpu->vm, ipa)) {
                    if (!hyp_ram_fill(vcpu->vm, ipa & ~(uint64_t)(PAGE_SIZE - 1))) continue;
                } else if (ec == 0x24) {
                    int mmio = guest_mmio(vcpu, s, ipa);
                    if (mmio == 2) return;
                    if (mmio) {
                        if (vcpu->vm->managed_console && hyp_console_pending(vcpu->vm)) {
                            vcpu->exit = HYP_EXIT_YIELD;
                            return;
                        }
                        continue;
                    }
                }
            }
            vcpu->exit = HYP_EXIT_FAULT;
            return;
        } else { vcpu->exit = HYP_EXIT_FAULT; return; }
        if (vcpu->vm->managed_console && hyp_console_pending(vcpu->vm)) {
            vcpu->exit = HYP_EXIT_YIELD;
            return;
        }
    }
}
static void aarch64_put(hyp_vcpu_t *vcpu)
{
    (void)vcpu; /* Every monitor return already restores the entire host bank. */
}
const hyp_backend_t *hyp_arch_backend(void)
{
    static const hyp_backend_t backend = {
        .name = "aarch64-el2", .s2_root_order = 0, .max_running_vcpus = CONFIG_NR_CPUS,
        .gpa_limit = 1ULL << 39, .ram_base = 0x40000000,
        .probe_smp = aarch64_smp, .reset = aarch64_reset,
        .vm_init = aarch64_vm_init, .vm_destroy = hyp_s2_tree_destroy,
        .map = hyp_s2_tree_map, .pin = hyp_s2_tree_pin,
        .unmap = hyp_s2_tree_unmap, .translate = hyp_s2_tree_translate,
        .audit = hyp_s2_tree_audit, .set_boot = aarch64_set_boot,
        .load = hyp_arch_vcpu_setup, .enter = hyp_arch_vcpu_enter,
        .put = aarch64_put, .vcpu_destroy = aarch64_destroy_vcpu,
    };
    return &backend;
}
#else
int hyp_arch_vcpu_setup(hyp_vcpu_t *vcpu) { (void)vcpu; return -EOPNOTSUPP; }
void hyp_arch_vcpu_enter(hyp_vcpu_t *vcpu) { (void)vcpu; }
const hyp_backend_t *hyp_arch_backend(void) { return NULL; }
#endif
