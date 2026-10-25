/* Experimental native LVZ backend. Availability requires both real LVZ
 * hardware and explicit opt-in. No LVZ CSR/instruction is probed on TCG. */
#include "hyp/hyp_vcpu.h"
#include "hyp/hyp_s2.h"
#include "core/bootargs.h"
#include "core/string.h"
#include "core/trap.h"
#include "core/timer.h"
#include "core/smp.h"
#include "mm/slab.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "hyp_lvz.h"

#define CSR_READ(csr) ({ uint64_t value; __asm__ volatile("csrrd %0, %1" : "=r"(value) : "i"(csr)); value; })
#define GCSR_READ(csr) ({ uint64_t value; __asm__ volatile("gcsrrd %0, %1" : "=r"(value) : "i"(csr) : "memory"); value; })
#define GCSR_WRITE(csr, val) do { uint64_t value = (val); __asm__ volatile("gcsrwr %0, %1" : "+r"(value) : "i"(csr) : "memory"); } while (0)
/* Hardware guest CSRs implemented by baseline LVZ. PMU, IOCSR and nested
 * virtualization stay trapped. TCFG is disabled before the bank is parked. */
#define LVZ_BANK(X) \
 X(0x0) X(0x1) X(0x2) X(0x3) X(0x4) X(0x5) X(0x6) X(0x7) X(0x8) X(0xc) \
 X(0x10) X(0x11) X(0x12) X(0x13) X(0x18) X(0x19) X(0x1a) X(0x1c) X(0x1d) X(0x1e) X(0x1f) \
 X(0x20) X(0x21) X(0x22) X(0x23) \
 X(0x30) X(0x31) X(0x32) X(0x33) X(0x34) X(0x35) X(0x36) X(0x37) \
 X(0x40) X(0x43) X(0x60) \
 X(0x88) X(0x89) X(0x8a) X(0x8b) X(0x8c) X(0x8d) X(0x8e) X(0x8f) \
 X(0x180) X(0x181) X(0x182) X(0x183)

static __attribute__((unused)) uint32_t cpucfg(unsigned index)
{
    uint64_t value;
    __asm__ volatile("cpucfg %0, %1" : "=r"(value) : "r"((uint64_t)index));
    return value;
}
static __attribute__((unused)) int opt_in(void)
{
    const char *args = bootargs_get();
    const char key[] = "a20.lvz_experimental=1";
    if (!args) return 0;
    while (*args) {
        while (*args == ' ' || *args == '\t') args++;
        const char *end = args;
        while (*end && *end != ' ' && *end != '\t') end++;
        if ((size_t)(end - args) == sizeof(key) - 1 && !memcmp(args, key, sizeof(key) - 1)) return 1;
        args = end;
    }
    return 0;
}
int hyp_supported(void)
{
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    /* CPUCFG itself is a baseline instruction. Read no LVZ CSR before this
     * check; QEMU currently reports CPUCFG2.LVZP=0. Need KS0..KS7. */
    uint32_t cfg = cpucfg(2);
    if (!(cfg & (1U << 10)) || ((cfg >> 11) & 7) != 1 || !opt_in()) return 0;
    /* Bank only the baseline's eight scratch CSRs; reject wider extensions
     * until their additional state has an explicit save/restore contract. */
    return (CSR_READ(0x21) & 15) == 8 && (CSR_READ(0x51) & (1U << 1));
#else
    return 0;
#endif
}
void hyp_arch_vmid_fenced(uint16_t vmid) { (void)vmid; }
void hyp_arch_s2_fence(uint16_t vmid, uint64_t gpa)
{
    (void)vmid; (void)gpa;
    if (hyp_supported()) __asm__ volatile("invtlb 0x12, $zero, $zero" ::: "memory");
}
uint64_t hyp_arch_vsatp_get(void) { return 0; }
void hyp_arch_vsatp_set(uint64_t value) { (void)value; }
uint64_t hyp_arch_vstimecmp_get(void) { return 0; }
void hyp_arch_vstimecmp_set(uint64_t value) { (void)value; }
void hyp_arch_host_tlb_fence(void) { hyp_arch_s2_fence(0, 0); }

#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
static uint64_t leaf_flags(pte_t prot)
{
    uint64_t value = LA_PTE_VALID | LA_PTE_MAT1;
    if (prot & PTE_W) value |= LA_PTE_DIRTY;
    if (!(prot & PTE_R)) value |= LA_PTE_NR;
    if (!(prot & PTE_X)) value |= LA_PTE_NX;
    return value;
}
static int invalidate(hyp_vm_t *vm, uint64_t gpa)
{
    (void)vm; hyp_arch_s2_fence(0, gpa); return 0;
}
static const hyp_s2_format_t format = {
    .levels = ARCH_PT_LEVELS, .address_mask = 0x0000fffffffff000ULL,
    .valid_mask = 1, .table_flags = 1, .leaf_flags = leaf_flags,
    .leaf_check_mask = LA_PTE_VALID | LA_PTE_DIRTY | LA_PTE_MAT1 | LA_PTE_NR | LA_PTE_NX,
    .invalidate = invalidate, .live_update = 1,
};
static int vm_init(hyp_vm_t *vm) { return hyp_s2_tree_init(vm, &format); }
static hyp_lvz_state_t *state_get(hyp_vcpu_t *vcpu)
{
    hyp_lvz_state_t *s = vcpu->backend_data;
    if (!s) {
        s = kcalloc(1, sizeof(*s));
        if (!s) return NULL;
        s->gcsr[0] = 1U << 3; /* PLV0, DA=1, guest physical reset entry. */
        s->gcsr[0x20] = s->gcsr[0x40] = vcpu->id;
        s->gcsr[0x1c] = LA64_PWCL_VALUE;
        s->gcsr[0x1d] = LA64_PWCH_VALUE;
        s->gcsr[0x1e] = 12;
        s->gcsr[0x21] = (CSR_READ(0x21) & ~15ULL) | 8;
        s->gcsr[0x22] = 1U << 12;
        s->gcsr[0x23] = CSR_READ(0x23);
        vcpu->backend_data = s;
    }
    return s;
}
static void reset(hyp_vcpu_t *vcpu, uint64_t entry, uint64_t hart, uint64_t opaque)
{
    hyp_lvz_state_t *s = state_get(vcpu);
    if (s) {
        memset(s, 0, sizeof(*s));
        s->gcsr[0] = 1U << 3;
        s->gcsr[0x20] = s->gcsr[0x40] = hart;
        s->gcsr[0x1c] = LA64_PWCL_VALUE;
        s->gcsr[0x1d] = LA64_PWCH_VALUE;
        s->gcsr[0x1e] = 12;
        s->gcsr[0x21] = (CSR_READ(0x21) & ~15ULL) | 8;
        s->gcsr[0x22] = 1U << 12;
        s->gcsr[0x23] = CSR_READ(0x23);
    }
    memset(vcpu->regs, 0, sizeof(vcpu->regs));
    vcpu->regs[4] = hart; vcpu->regs[5] = opaque; vcpu->pc = entry;
    vcpu->exit = HYP_EXIT_NONE;
}
static int set_boot(hyp_vcpu_t *vcpu, uint64_t hart, uint64_t opaque)
{
    vcpu->regs[4] = hart; vcpu->regs[5] = opaque; return 0;
}
int hyp_arch_vcpu_setup(hyp_vcpu_t *vcpu)
{
    if (!hyp_supported()) return -EOPNOTSUPP;
    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC || !vcpu->vm || vcpu->vm->magic != HYP_VM_MAGIC)
        return -EINVAL;
    hyp_lvz_state_t *s = state_get(vcpu);
    if (!s) return -ENOMEM;
    s->s2_root = va_to_pa(vcpu->vm->s2_root);
    uint64_t caps = CSR_READ(0x51);
    /* MATP_ROOT is the capability, MATC_ROOT is the selected policy. */
    if (!(caps & (1U << 1))) return -EOPNOTSUPP;
    s->gcfg = (1U << 4) | ((caps & (1U << 16)) ? 2U << 20 : 0);
#define RESTORE(csr) GCSR_WRITE(csr, s->gcsr[csr]);
    LVZ_BANK(RESTORE)
#undef RESTORE
    GCSR_WRITE(0x41, 0); /* Guest timers are unsupported in this experiment. */
    GCSR_WRITE(0x44, 1);
    GCSR_WRITE(0x60, s->gcsr[0x60] | 2); /* Never inherit an LL reservation. */
    timer_set_interval(ARCH_TIMER_FREQ / 100);
    return 0;
}
static void sync_irqs(hyp_vcpu_t *vcpu, unsigned set, unsigned clear)
{
    hyp_lvz_state_t *s = vcpu->backend_data;
    if (!s) return;
    if (set & 2) { vcpu->exit = HYP_EXIT_FAULT; vcpu->exit_stval = 0x41; }
    uint64_t estat = GCSR_READ(0x5);
    if (set & 1) estat |= 1;
    if (clear & 1) estat &= ~1ULL;
    GCSR_WRITE(0x5, estat);
    if (set & 4) s->gintc |= 1;
    if (clear & 4) s->gintc &= ~1ULL;
}
static void put(hyp_vcpu_t *vcpu)
{
    hyp_lvz_state_t *s = vcpu->backend_data;
    if (!s) return;
#define SAVE(csr) s->gcsr[csr] = GCSR_READ(csr);
    LVZ_BANK(SAVE)
#undef SAVE
    uint64_t timer = GCSR_READ(0x41);
    GCSR_WRITE(0x41, 0);
    GCSR_WRITE(0x44, 1);
    if (timer & 1) { /* Refuse a timer-using guest rather than lose its phase. */
        vcpu->exit = HYP_EXIT_FAULT; vcpu->exit_stval = 0x41;
    }
}
static void destroy(hyp_vcpu_t *vcpu) { kfree(vcpu->backend_data); vcpu->backend_data = NULL; }
static int probe_smp(hyp_vm_t *vm) { (void)vm; return hyp_supported() ? 0 : -EOPNOTSUPP; }
static void fault(hyp_vcpu_t *vcpu, hyp_lvz_state_t *s, uint64_t code)
{
    vcpu->exit = HYP_EXIT_FAULT; vcpu->exit_scause = code;
    vcpu->exit_stval = s->badv; vcpu->exit_htval = s->badi;
}
static int hypercall(hyp_vcpu_t *vcpu, hyp_lvz_state_t *s)
{
    /* A20 experimental HVCL 0: a7=1 putchar, 8 VM off, 9 hart off. */
    if (s->badi != 0x002b8000) return 0;
    if (vcpu->regs[11] == 1) {
        if (hyp_console_putchar(vcpu->vm, vcpu->regs[4]) == -EAGAIN) {
            vcpu->exit = HYP_EXIT_YIELD; return 1;
        }
        vcpu->regs[4] = 0; vcpu->pc += 4; return 1;
    }
    if (vcpu->regs[11] == 8) { hyp_vm_shutdown(vcpu->vm); vcpu->exit = HYP_EXIT_SHUTDOWN; return 1; }
    if (vcpu->regs[11] == 9) { vcpu->exit = HYP_EXIT_HART_STOPPED; return 1; }
    return 0;
}
/* Return -1 for a hole, 0 for denied, 1 for allowed. A remote fault may
 * already have filled the page; distinguish that race from an NX/RO fault. */
static int access_allowed(hyp_vm_t *vm, uint64_t gpa, unsigned code)
{
    if (gpa >= vm->backend->gpa_limit) return 0;
    uint64_t flags = spin_lock_irqsave(&vm->s2_lock);
    uint64_t *table = (uint64_t *)vm->s2_root;
    int result = -1;
    for (int level = ARCH_PT_LEVELS - 1; level >= 0; level--) {
        uint64_t entry = table[(gpa >> (12 + 9 * level)) & 511];
        if (!(entry & 1)) break;
        if (level) {
            pfn_t pfn = phys_to_pfn(entry & format.address_mask);
            if (!pfn_valid(pfn) || !(pfa.meta[pfn].flags & FRAME_F_PT)) { result = 0; break; }
            table = pfn_to_virt(pfn);
        }
        else result = code == 1 ? !(entry & LA_PTE_NR) :
                      code == 2 ? !!(entry & LA_PTE_DIRTY) : !(entry & LA_PTE_NX);
    }
    spin_unlock_irqrestore(&vm->s2_lock, flags);
    return result;
}
void hyp_arch_vcpu_enter(hyp_vcpu_t *vcpu)
{
    if (!hyp_supported() || !vcpu || !vcpu->backend_data || vcpu->exit != HYP_EXIT_NONE) return;
    arch_local_irq_disable();
    hyp_lvz_state_t *s = vcpu->backend_data;
    for (;;) {
        memcpy(s->gpr, vcpu->regs, sizeof(s->gpr)); s->gpr[0] = 0; s->pc = vcpu->pc;
        hyp_lvz_switch(s);
        memcpy(vcpu->regs, s->gpr, sizeof(s->gpr)); vcpu->regs[0] = 0; vcpu->pc = s->pc;
        unsigned code = (s->estat >> 16) & 63;
        if (!code) {
            uint64_t pending = s->estat & CSR_READ(4) & 0x1fff;
            for (unsigned i = 0; i < 13; i++) if (pending & (1U << i)) arch_handle_irq(i, 0);
            vcpu->exit = HYP_EXIT_YIELD;
        } else if (code >= 1 && code <= 3) {
            /* Separate permission denial from a concurrent successful fill. */
            int allowed = access_allowed(vcpu->vm, s->badv, code);
            if (!allowed || (allowed < 0 && hyp_ram_fill(vcpu->vm, s->badv & ~(uint64_t)(PAGE_SIZE - 1))))
                fault(vcpu, s, code);
        } else if (code == 23 && hypercall(vcpu, s)) {
        } else if (code == 22 && (s->badi & 0xfffffc00U) == 0x00006c00U) {
            unsigned rd = s->badi & 31, rj = (s->badi >> 5) & 31;
            unsigned index = vcpu->regs[rj];
            uint64_t value = index < 21 ? cpucfg(index) : 0;
            if (index == 2) value &= ~((0x3ffU << 0) | (15U << 10) | (7U << 18) | (1U << 25));
            if (rd) vcpu->regs[rd] = value;
            vcpu->pc += 4;
        } else {
            /* Includes FP/LSX/LASX/LBT-disabled traps, unsupported CSRs,
             * IOCSR, cache maintenance and devices. Host state stays private. */
            fault(vcpu, s, code);
        }
        if (vcpu->exit != HYP_EXIT_NONE || hyp_vcpu_control_exit(vcpu)) return;
        hyp_vcpu_sync_irqs(vcpu);
    }
}
const hyp_backend_t *hyp_arch_backend(void)
{
    static const hyp_backend_t backend = {
        .name = "loongarch-lvz-experimental", .max_running_vcpus = CONFIG_NR_CPUS,
        .gpa_limit = 1ULL << (12 + 9 * ARCH_PT_LEVELS), .ram_base = 0x80000000,
        .vm_init = vm_init, .vm_destroy = hyp_s2_tree_destroy,
        .map = hyp_s2_tree_map, .pin = hyp_s2_tree_pin, .unmap = hyp_s2_tree_unmap,
        .translate = hyp_s2_tree_translate, .audit = hyp_s2_tree_audit,
        .probe_smp = probe_smp, .reset = reset, .set_boot = set_boot, .sync_irqs = sync_irqs,
        .load = hyp_arch_vcpu_setup, .enter = hyp_arch_vcpu_enter, .put = put, .vcpu_destroy = destroy,
    };
    return hyp_supported() ? &backend : NULL;
}
#else
int hyp_arch_vcpu_setup(hyp_vcpu_t *vcpu) { (void)vcpu; return -EOPNOTSUPP; }
void hyp_arch_vcpu_enter(hyp_vcpu_t *vcpu) { (void)vcpu; }
const hyp_backend_t *hyp_arch_backend(void) { return NULL; }
#endif
