/* AMD SVM host backend. Intel VMX remains unsupported: reporting a usable
 * backend requires guest entry and second-stage translation, not CPUID alone.
 * Hardware definitions: AMD APM vol. 2 (24593), chapter 15 and appendix B.
 * Boot contract: flat 32-bit protected mode, paging off, CS=8, data=16;
 * regs[] follows x86 register numbering. The guest can enable its own paging.
 * VMMCALL EAX=1/EBX=byte writes console; EAX=8 shuts down. */
#include "hyp/hyp_vcpu.h"
#include "hyp/hyp_s2.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "cpu.h"
#include "hyp_svm.h"

/* Eight contiguous pages: guest VMCB, host VMLOAD bank, hardware HSAVE,
 * three IOPM pages and two MSRPM pages. Both permission maps intercept all.
 * The two FXSAVE images occupy reserved software space in the host bank. */
struct svm_state {
    pfn_t pages;
    unsigned initialized;
    uint64_t old_efer, old_hsave, old_cr4, old_dr6, old_dr7;
};
static uint64_t get64(void *p, unsigned off) { return *(uint64_t *)((uint8_t *)p + off); }
static void set64(void *p, unsigned off, uint64_t v) { *(uint64_t *)((uint8_t *)p + off) = v; }
static void set32(void *p, unsigned off, uint32_t v) { *(uint32_t *)((uint8_t *)p + off) = v; }
static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b,
                  uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}
int hyp_svm_supported(void)
{
    #if !defined(ARCH_HAS_PGTABLE_OPS) || defined(CONFIG_NOMMU)
    return 0;
    #endif
    uint32_t a, b, c, d;
    cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x8000000a) return 0;
    cpuid(0x80000001, 0, &a, &b, &c, &d);
    if (!(c & (1u << 2))) return 0;
    cpuid(0x8000000a, 0, &a, &b, &c, &d);
    /* NPT is mandatory; instruction decoding covers older SVM revisions. */
    if (b < 2 || !(d & 1)) return 0;
    cpuid(1, 0, &a, &b, &c, &d);
    if ((d & ((1u << 24) | (1u << 25))) != ((1u << 24) | (1u << 25))) return 0;
    return !(rdmsr(SVM_VM_CR) & (1ULL << 4)); /* firmware SVMDIS */
}

/* Every entry flushes all ASIDs. Per-vCPU VMCB/HSAVE ownership and the
 * common stop barrier permit independent vCPUs on different physical CPUs. */
static int svm_invalidate(hyp_vm_t *vm, uint64_t gpa)
{
    (void)vm; (void)gpa;
    return 0;
}
static uint64_t svm_leaf_flags(pte_t prot)
{
    if (!(prot & PTE_R)) return 0; /* NPT cannot encode execute-only pages. */
    return 5ULL | ((prot & PTE_W) ? 2ULL : 0) |
           ((prot & PTE_X) ? 0 : (1ULL << 63));
}
static const hyp_s2_format_t svm_npt = {
    .levels = 4, .address_mask = 0x000ffffffffff000ULL,
    .valid_mask = 1, .table_flags = 7,
    .leaf_check_mask = (1ULL << 63) | 7, .live_update = 1,
    .leaf_flags = svm_leaf_flags, .invalidate = svm_invalidate,
};
static int svm_vm_init(hyp_vm_t *vm) { return hyp_s2_tree_init(vm, &svm_npt); }
static void svm_segment(void *vmcb, unsigned off, uint16_t sel, uint16_t attr,
                        uint32_t limit, uint64_t base)
{
    uint8_t *p = (uint8_t *)vmcb + off;
    *(uint16_t *)(p + 0) = sel;
    *(uint16_t *)(p + 2) = attr;
    *(uint32_t *)(p + 4) = limit;
    *(uint64_t *)(p + 8) = base;
}
static void svm_init_state(struct svm_state *s)
{
    uint8_t *v = pfn_to_virt(s->pages);
    memset(v, 0, 8 * PAGE_SIZE);
    memset(v + 3 * PAGE_SIZE, 0xff, 5 * PAGE_SIZE);
    /* DR0..7 are not all automatically context switched by VMRUN. */
    set32(v, 0x004, 0xffffffffu);
    set32(v, 0x000, 1u << 20); /* CR4 writes: prohibit unbanked XSAVE state */
    set32(v, 0x008, 0); /* guest IDT handles its own synchronous exceptions */
    set32(v, 0x00c, (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) |
          (1u << 15) | (1u << 18) | (1u << 19) | (1u << 22) |
          (1u << 24) | (1u << 26) | (1u << 27) | (1u << 28) | (1u << 31));
    /* Intercept SVM, cache maintenance, MONITOR/MWAIT and XSETBV/RDPRU. */
    set32(v, 0x010, 0x7fff);
    set64(v, SVM_IOPM, pfn_to_phys(s->pages) + 3 * PAGE_SIZE);
    set64(v, SVM_MSRPM, pfn_to_phys(s->pages) + 6 * PAGE_SIZE);
    set32(v, SVM_ASID, 1);
    set32(v, SVM_INT_CTL, 1u << 24); /* physical interrupts use host IF */
    set64(v, SVM_NESTED_CTL, 1);
    svm_segment(v, SVM_CS, 8, 0xc9b, 0xffffffffu, 0);
    const unsigned segs[] = {SVM_ES, SVM_SS, SVM_DS, SVM_FS, SVM_GS};
    for (unsigned i = 0; i < sizeof(segs) / sizeof(segs[0]); i++)
        svm_segment(v, segs[i], 16, 0xc93, 0xffffffffu, 0);
    svm_segment(v, SVM_TR, 0, 0x8b, 0xffff, 0);
    svm_segment(v, SVM_LDTR, 0, 0x82, 0xffff, 0);
    set64(v, SVM_GUEST_EFER, SVM_SVME);
    set64(v, SVM_CR0, 0x31); /* PE | ET | NE */
    set64(v, SVM_CR4, (1u << 9) | (1u << 10)); /* OSFXSR | OSXMMEXCPT */
    set64(v, SVM_DR6, 0xffff0ff0ULL);
    set64(v, SVM_DR7, 0x400);
    set64(v, SVM_RFLAGS, 2);
    set64(v, SVM_PAT, 0x0007040600070406ULL);
    /* Reset x87 control word/MXCSR, all guest data and XMM registers zero. */
    *(uint16_t *)(v + PAGE_SIZE + 0xa00) = 0x37f;
    *(uint32_t *)(v + PAGE_SIZE + 0xa00 + 24) = 0x1f80;
    s->initialized = 1;
}
static void svm_vcpu_destroy(hyp_vcpu_t *vcpu)
{
    struct svm_state *s = vcpu->backend_data;
    if (s) { pfa_free(s->pages, 3); kfree(s); vcpu->backend_data = NULL; }
}
static void svm_reset(hyp_vcpu_t *vcpu, uint64_t entry, uint64_t hart, uint64_t opaque)
{
    struct svm_state *s = vcpu->backend_data;
    if (s) s->initialized = 0;
    memset(vcpu->regs, 0, sizeof(vcpu->regs));
    vcpu->pc = entry;
    vcpu->regs[6] = opaque; /* ESI boot-data GPA; EDI virtual CPU id */
    vcpu->regs[7] = hart;
}
static int svm_set_boot(hyp_vcpu_t *vcpu, uint64_t hart, uint64_t opaque)
{
    vcpu->regs[6] = opaque; vcpu->regs[7] = hart;
    return 0;
}
static int svm_setup(hyp_vcpu_t *vcpu)
{
    if (!hyp_svm_supported()) return -EOPNOTSUPP;
    if (!vcpu || !vcpu->vm || !vcpu->vm->s2_root) return -EINVAL;
    struct svm_state *s = vcpu->backend_data;
    if (!s) {
        s = kcalloc(1, sizeof(*s));
        if (!s) return -ENOMEM;
        s->pages = pfa_alloc(3);
        if (s->pages == PFN_NONE) { kfree(s); return -ENOMEM; }
        vcpu->backend_data = s;
    }
    if (!s->initialized && vcpu->pc > 0xffffffffULL) return -EINVAL;
    if (!s->initialized) svm_init_state(s);
    uint8_t *v = pfn_to_virt(s->pages);
    set64(v, SVM_NCR3, (uint64_t)(uintptr_t)vcpu->vm->s2_root - PAGE_OFFSET);
    s->old_efer = rdmsr(SVM_EFER);
    s->old_hsave = rdmsr(SVM_HSAVE_PA);
    __asm__ volatile("mov %%cr4, %0" : "=r"(s->old_cr4));
    __asm__ volatile("mov %%dr6, %0" : "=r"(s->old_dr6));
    __asm__ volatile("mov %%dr7, %0" : "=r"(s->old_dr7));
    /* FXRSTOR must include XMM even if the host itself uses no SSE. */
    __asm__ volatile("mov %0, %%cr4" :: "r"(s->old_cr4 | (3u << 9)) : "memory");
    wrmsr(SVM_EFER, s->old_efer | SVM_SVME);
    wrmsr(SVM_HSAVE_PA, pfn_to_phys(s->pages) + 2 * PAGE_SIZE);
    x86_64_timer_ensure_deadline(TICKS_PER_SEC / 100);
    return 0;
}
static void svm_put(hyp_vcpu_t *vcpu)
{
    struct svm_state *s = vcpu->backend_data;
    wrmsr(SVM_HSAVE_PA, s->old_hsave);
    wrmsr(SVM_EFER, s->old_efer);
    __asm__ volatile("mov %0, %%dr6" :: "r"(s->old_dr6));
    __asm__ volatile("mov %0, %%dr7" :: "r"(s->old_dr7));
    __asm__ volatile("mov %0, %%cr4" :: "r"(s->old_cr4) : "memory");
}
/* A deliberately small virtual CPUID model: no XSAVE/AVX, SVM, PMU, APIC,
 * MONITOR, FSGSBASE or host topology. Legacy/SSE state is explicitly banked. */
static void svm_cpuid(hyp_vcpu_t *vcpu)
{
    uint32_t leaf = vcpu->regs[0], a = 0, b = 0, c = 0, d = 0;
    if (leaf == 0) { a = 1; b = 0x68747541; d = 0x69746e65; c = 0x444d4163; }
    if (leaf == 1) {
        a = 0x00000663; b = 1u << 16; c = 1u << 31;
        d = (1u << 0) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 6) |
            (1u << 8) | (1u << 13) | (1u << 15) | (1u << 23) |
            (1u << 24) | (1u << 25) | (1u << 26);
    }
    if (leaf == 0x80000000) a = 0x80000008;
    if (leaf == 0x80000001) d = (1u << 11) | (1u << 20) | (1u << 29);
    if (leaf == 0x80000008) a = 48u | (48u << 8);
    vcpu->regs[0] = a; vcpu->regs[3] = b;
    vcpu->regs[1] = c; vcpu->regs[2] = d;
}
static int svm_msr(hyp_vcpu_t *vcpu, void *v)
{
    unsigned off;
    switch ((uint32_t)vcpu->regs[1]) {
    case SVM_EFER: off = SVM_GUEST_EFER; break;
    case 0xc0000081: off = SVM_STAR; break;
    case 0xc0000082: off = SVM_LSTAR; break;
    case 0xc0000083: off = SVM_CSTAR; break;
    case 0xc0000084: off = SVM_SFMASK; break;
    case 0xc0000100: off = SVM_FS + 8; break;
    case 0xc0000101: off = SVM_GS + 8; break;
    case 0xc0000102: off = SVM_KERNEL_GS; break;
    case 0x174: off = SVM_SYSENTER_CS; break;
    case 0x175: off = SVM_SYSENTER_ESP; break;
    case 0x176: off = SVM_SYSENTER_EIP; break;
    default: return 0;
    }
    if (get64(v, SVM_EXIT_INFO1) & 1) {
        uint64_t value = (uint32_t)vcpu->regs[0] | ((uint64_t)(uint32_t)vcpu->regs[2] << 32);
        if (off == SVM_GUEST_EFER) {
            if (value & ~0xd01ULL) return 0; /* SCE/LME/LMA/NXE only */
            value = (value & ~0x400ULL) | (get64(v, off) & 0x400ULL) | SVM_SVME;
        }
        /* VMLOAD checks canonical bases. Reject malformed MSRs before entry. */
        if (off == SVM_LSTAR || off == SVM_CSTAR || off == SVM_KERNEL_GS ||
            off == SVM_FS + 8 || off == SVM_GS + 8 ||
            off == SVM_SYSENTER_ESP || off == SVM_SYSENTER_EIP) {
            if ((int64_t)(value << 16) >> 16 != (int64_t)value) return 0;
        }
        set64(v, off, value);
    } else {
        uint64_t value = get64(v, off);
        if (off == SVM_GUEST_EFER) value &= ~SVM_SVME;
        vcpu->regs[0] = (uint32_t)value;
        vcpu->regs[2] = value >> 32;
    }
    return 1;
}
/* Resolve instruction bytes through the guest's own paging mode. The VMM's
 * pointers always address host RAM; no guest linear address is dereferenced.
 * This walker is only for intercept decode on CPUs lacking decode assists. */
static int svm_linear_gpa(hyp_vcpu_t *vcpu, void *v, uint64_t linear, uint64_t *gpa)
{
    uint64_t cr0 = get64(v, SVM_CR0), cr4 = get64(v, SVM_CR4);
    if (!(cr0 & (1ULL << 31))) { *gpa = linear; return 0; }
    uint64_t table = get64(v, SVM_CR3), entry;
    int long_mode = !!(get64(v, SVM_GUEST_EFER) & (1u << 10));
    if (!long_mode && !(cr4 & (1u << 5))) {
        uint32_t pte;
        uint64_t addr = (table & ~0xfffULL) + ((linear >> 22) & 1023) * 4;
        if (hyp_vm_copy_from_guest(vcpu->vm, &pte, addr, 4) || !(pte & 1)) return -EFAULT;
        if ((pte & 128) && (cr4 & 16)) {
            *gpa = (pte & 0xffc00000ULL) | (linear & 0x3fffff); return 0;
        }
        addr = (pte & ~0xfffULL) + ((linear >> 12) & 1023) * 4;
        if (hyp_vm_copy_from_guest(vcpu->vm, &pte, addr, 4) || !(pte & 1)) return -EFAULT;
        *gpa = (pte & ~0xfffULL) | (linear & 4095); return 0;
    }
    int top = long_mode ? 3 : 2;
    table &= long_mode ? 0x000ffffffffff000ULL : 0xffffffe0ULL;
    for (int level = top; level >= 0; level--) {
        unsigned bits = (!long_mode && level == 2) ? 3 : 511;
        uint64_t addr = table + ((linear >> (12 + 9 * level)) & bits) * 8;
        if (hyp_vm_copy_from_guest(vcpu->vm, &entry, addr, 8) || !(entry & 1)) return -EFAULT;
        if (level == 0 || ((entry & 128) && level <= 2)) {
            uint64_t mask = (1ULL << (12 + 9 * level)) - 1;
            *gpa = (entry & 0x000ffffffffff000ULL & ~mask) | (linear & mask); return 0;
        }
        table = entry & 0x000ffffffffff000ULL;
    }
    return -EFAULT;
}
/* Return encoded length, plus the register operand for MOV-to-CR4. */
static unsigned svm_decode(hyp_vcpu_t *vcpu, void *v, uint64_t code, unsigned *reg)
{
    uint8_t insn[15];
    unsigned have = 0;
    uint64_t linear = vcpu->pc + get64(v, SVM_CS + 8);
    for (; have < sizeof(insn); have++) {
        uint64_t gpa;
        if (svm_linear_gpa(vcpu, v, linear + have, &gpa) ||
            hyp_vm_copy_from_guest(vcpu->vm, &insn[have], gpa, 1)) break;
    }
    unsigned pos = 0, rex = 0;
    int cs64 = !!(*(uint16_t *)((uint8_t *)v + SVM_CS + 2) & (1u << 9));
    while (pos < have) {
        unsigned op = insn[pos];
        if (op == 0x66 || op == 0x67 || op == 0xf2 || op == 0xf3 ||
            op == 0x26 || op == 0x2e || op == 0x36 || op == 0x3e || op == 0x64 || op == 0x65) {
            rex = 0; pos++; continue;
        }
        if (cs64 && op >= 0x40 && op <= 0x4f) { rex = op; pos++; continue; }
        break;
    }
    if (code == 0x78) return pos < have && insn[pos] == 0xf4 ? pos + 1 : 0;
    if (pos + 1 >= have || insn[pos] != 0x0f) return 0;
    if ((code == 0x72 && insn[pos + 1] == 0xa2) ||
        (code == 0x7c && (insn[pos + 1] == 0x30 || insn[pos + 1] == 0x32))) return pos + 2;
    if (pos + 2 >= have) return 0;
    if (code == 0x81 && insn[pos + 1] == 1 && insn[pos + 2] == 0xd9) return pos + 3;
    if (code == 0x14 && insn[pos + 1] == 0x22 &&
        (insn[pos + 2] & 0xf8) == 0xe0 && !(rex & 4)) {
        *reg = (insn[pos + 2] & 7) | ((rex & 1) ? 8 : 0);
        return pos + 3;
    }
    return 0;
}
extern void hyp_svm_run(uint64_t guest, uint64_t host, uint64_t *regs,
                         void *host_fx, void *guest_fx);
static void svm_enter(hyp_vcpu_t *vcpu)
{
    struct svm_state *s = vcpu->backend_data;
    uint8_t *v = pfn_to_virt(s->pages);
    uint64_t pa = pfn_to_phys(s->pages);
    /* A bounded number of synchronous intercepts also yields to controllers. */
    for (unsigned budget = 0; budget < 256; budget++) {
        if (hyp_vcpu_control_exit(vcpu)) return;
        set64(v, SVM_RIP, vcpu->pc);
        set64(v, SVM_RAX, vcpu->regs[0]);
        set64(v, SVM_RSP, vcpu->regs[4]);
        v[SVM_TLB_CTL] = 1;
        hyp_svm_run(pa, pa + PAGE_SIZE, vcpu->regs,
                    v + PAGE_SIZE + 0x800, v + PAGE_SIZE + 0xa00);
        vcpu->pc = get64(v, SVM_RIP);
        vcpu->regs[0] = get64(v, SVM_RAX);
        vcpu->regs[4] = get64(v, SVM_RSP);
        uint64_t code = get64(v, SVM_EXIT_CODE);
        uint64_t info1 = get64(v, SVM_EXIT_INFO1), info2 = get64(v, SVM_EXIT_INFO2);
        vcpu->exit_scause = code;
        vcpu->exit_stval = info1;
        vcpu->exit_htval = info2;
        int advance = 0;
        unsigned reg = 0;
        unsigned length = svm_decode(vcpu, v, code, &reg);
        if (code >= 0x60 && code <= 0x64) { vcpu->exit = HYP_EXIT_YIELD; return; }
        if (code == 0x72) { svm_cpuid(vcpu); advance = 1; }
        else if (code == 0x7c) advance = svm_msr(vcpu, v);
        else if (code == 0x14 && (info1 & (1ULL << 63))) {
            uint64_t cr4 = vcpu->regs[info1 & 15];
            /* CR4.OSXSAVE would expose unbanked YMM/extended state. */
            if (!(cr4 & ~0x6ffULL)) { set64(v, SVM_CR4, cr4); advance = 1; }
        } else if (code == 0x81) {
            if (vcpu->regs[0] == 9) {
                vcpu->regs[0] = (uint64_t)(int64_t)hyp_vm_hart_start(vcpu->vm,
                    vcpu->regs[3], vcpu->regs[1], vcpu->regs[2]); advance = 1;
            } else if (vcpu->regs[0] == 10) {
                vcpu->exit = HYP_EXIT_HART_STOPPED; return;
            } else if (vcpu->regs[0] == 11) {
                vcpu->regs[0] = (uint64_t)(int64_t)hyp_vm_hart_status(vcpu->vm, vcpu->regs[3]); advance = 1;
            } else
            if (vcpu->regs[0] == 8) { vcpu->exit = HYP_EXIT_SHUTDOWN; return; }
            if (vcpu->regs[0] == 1) {
                if (hyp_console_putchar(vcpu->vm, (uint8_t)vcpu->regs[3])) {
                    vcpu->exit = HYP_EXIT_YIELD; return;
                }
                vcpu->regs[0] = 0; advance = 1;
            }
        } else if (code == 0x78) {
            if (!length) break;
            vcpu->pc += length; vcpu->exit = HYP_EXIT_YIELD; return;
        } else if (code == 0x400) {
            /* Present=0 faults can allocate RAM. Protection violations must
             * remain faults; otherwise a read-only page loops forever. */
            if (!(info1 & 1) && hyp_ram_fill(vcpu->vm, info2 & ~(uint64_t)(PAGE_SIZE - 1)) == 0) continue;
        }
        if (!advance) {
            vcpu->exit = (code == (uint64_t)-1) ? HYP_EXIT_ERROR : HYP_EXIT_FAULT;
            return;
        }
        if (!length) { vcpu->exit = HYP_EXIT_FAULT; return; }
        vcpu->pc += length;
        if (vcpu->vm->managed_console && hyp_console_pending(vcpu->vm)) {
            vcpu->exit = HYP_EXIT_YIELD; return;
        }
    }
    vcpu->exit = HYP_EXIT_YIELD;
}
static int svm_smp(hyp_vm_t *vm) { (void)vm; return 0; }
const hyp_backend_t *hyp_svm_backend(void)
{
    static const hyp_backend_t backend = {
        .name = "x86-svm", .gpa_limit = 1ULL << 48, .ram_base = 0x100000,
        .max_running_vcpus = CONFIG_NR_CPUS, .probe_smp = svm_smp, .reset = svm_reset,
        .vm_init = svm_vm_init, .vm_destroy = hyp_s2_tree_destroy,
        .map = hyp_s2_tree_map, .pin = hyp_s2_tree_pin,
        .unmap = hyp_s2_tree_unmap, .translate = hyp_s2_tree_translate,
        .audit = hyp_s2_tree_audit, .set_boot = svm_set_boot,
        .load = svm_setup, .enter = svm_enter,
        .put = svm_put, .vcpu_destroy = svm_vcpu_destroy,
    };
    return &backend;
}
