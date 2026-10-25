/* RV32 VS instruction/SBI emulation. SBI TIME uses a0:a1, VS page walks
 * use Sv32 uint32 descriptors, interrupt scause bit is XLEN-1. Reads use
 * four-byte chunks to avoid crossing a page while decoding the last PTE. */
#include "hyp/hyp_vcpu.h"
#include "mm/frame.h"
#include "core/cpu.h"
#include "core/klog.h"
#include "core/stdio.h"
#include "core/string.h"

#define HYP_SCAUSE_ECALL_VU   8ULL
#define HYP_SCAUSE_ECALL_VS   9ULL
#define HYP_SCAUSE_ECALL_S   10ULL
#define HYP_SCAUSE_FETCH_PF  12ULL
#define HYP_SCAUSE_LOAD_PF   13ULL
#define HYP_SCAUSE_STORE_PF  15ULL
#define HYP_SCAUSE_GPF_INST  20ULL
#define HYP_SCAUSE_GPF_LOAD  21ULL
#define HYP_SCAUSE_VIRT_INST 22ULL
#define HYP_SCAUSE_GPF_STORE 23ULL
#define HYP_SCAUSE_INTR_BIT   (1ULL << 31)

static void hyp_vcpu_record_fault(hyp_vcpu_t *vcpu, uint64_t scause,
                                  uint64_t stval, uint64_t htval)
{
    vcpu->exit_scause = scause;
    vcpu->exit_stval  = stval;
    vcpu->exit_htval  = htval;
    vcpu->exit        = HYP_EXIT_FAULT;
}

#define HYP_SBI_EID_SRST     0x53525354ULL
#define HYP_SBI_EID_HSM      0x48534dULL
#define HYP_SBI_EID_IPI      0x735049ULL
#define HYP_SBI_FID_SRST_RESET 1ULL

#define HYP_SBI_ERR_NOT_SUPPORTED (-2)

#define HYP_SBI_EID_V02_BASE 0x10ULL
#define HYP_SBI_LEGACY_SET_TIMER 0x0ULL
#define HYP_SBI_CONSOLE_GETCHAR  0x02ULL

static void hyp_write_vstimecmp(uint64_t deadline)
{
    hyp_arch_vstimecmp_set(deadline);
}

static int hyp_guest_ecall(hyp_vcpu_t *vcpu, uint64_t scause)
{
    uint64_t eid = vcpu->regs[17];
    uint64_t fid = vcpu->regs[16];

    if (eid < HYP_SBI_EID_V02_BASE) {
        switch (eid) {
        case HYP_SBI_LEGACY_SET_TIMER:
            hyp_write_vstimecmp((uint32_t)vcpu->regs[10] | ((uint64_t)(uint32_t)vcpu->regs[11] << 32));
            vcpu->regs[10] = 0;
            break;
        case HYP_SBI_CONSOLE_PUTCHAR:
            if (hyp_console_putchar(vcpu->vm, (uint8_t)vcpu->regs[10]) == -EAGAIN) {
                vcpu->exit = HYP_EXIT_YIELD;
                return 0;
            }
            vcpu->regs[10] = 0;
            break;
        case HYP_SBI_CONSOLE_GETCHAR:
            vcpu->regs[10] = (uint64_t)HYP_SBI_ERR_NOT_SUPPORTED;
            break;
        case HYP_SBI_SHUTDOWN:
            vcpu->exit_scause = scause;
            hyp_vm_shutdown(vcpu->vm);
            vcpu->exit = HYP_EXIT_SHUTDOWN;
            return 0;
        default:

            kerr("hyp: legacy SBI call %lu is not implemented\n",
                 (unsigned long)eid);
            hyp_vcpu_record_fault(vcpu, scause, eid, 0);
            return 0;
        }
        vcpu->pc += 4;
        return 1;
    }

    if (eid == HYP_SBI_EID_SRST) {
        if (fid == 0 && vcpu->regs[10] == 0) {
            vcpu->exit_scause = scause;
            hyp_vm_shutdown(vcpu->vm);
            vcpu->exit = HYP_EXIT_SHUTDOWN;
            return 0;
        }
    }
    int rc = -EOPNOTSUPP;
    uint64_t result = 0;
    if (eid == 0x10) {
        rc = 0;
        switch (fid) {
        case 0: result = 2; break;
        case 1: result = 0xa20; break;
        case 2: result = 1; break;
        case 3: {
            uint64_t ext = vcpu->regs[10];
            result = ext == 0x10 || ext == 0x54494d45 || ext == HYP_SBI_EID_IPI ||
                     ext == HYP_SBI_EID_HSM || ext == 0x52464e43 || ext == HYP_SBI_EID_SRST;
            break;
        }
        case 4: case 5: case 6: result = 0; break;
        default: rc = -EOPNOTSUPP;
        }
    } else if (eid == 0x54494d45 && fid == 0) {
        hyp_write_vstimecmp((uint32_t)vcpu->regs[10] | ((uint64_t)(uint32_t)vcpu->regs[11] << 32)); rc = 0;
    } else if (eid == HYP_SBI_EID_HSM) {
        if (fid == 0)
            rc = hyp_vm_hart_start(vcpu->vm, vcpu->regs[10], vcpu->regs[11], vcpu->regs[12]);
        else if (fid == 1) {
            __atomic_store_n(&vcpu->hart_state, HYP_HART_STOP_PENDING, __ATOMIC_RELEASE);
            vcpu->exit = HYP_EXIT_HART_STOPPED;
            return 0;
        } else if (fid == 2) {
            rc = hyp_vm_hart_status(vcpu->vm, vcpu->regs[10]);
            if (rc >= 0) { result = (uint64_t)rc; rc = 0; }
        }
    } else if (eid == HYP_SBI_EID_IPI && fid == 0) {
        rc = hyp_vm_ipi(vcpu->vm, vcpu->regs[10], (uint32_t)vcpu->regs[11] == 0xffffffffU ? (uint64_t)-1 : vcpu->regs[11]);
    } else if (eid == 0x52464e43 && fid <= 2) {
        if (fid == 0) __asm__ volatile(".word 0x0000100f" ::: "memory");
        else hyp_arch_host_tlb_fence();
        rc = hyp_vm_fence(vcpu->vm, vcpu->regs[10], (uint32_t)vcpu->regs[11] == 0xffffffffU ? (uint64_t)-1 : vcpu->regs[11], fid == 0 ? 0 : 1);
    }
    int64_t error = !rc ? 0 : rc == -EOPNOTSUPP ? -2 : rc == -EINVAL ? -3 :
                    rc == -EALREADY ? -6 : -1;
    vcpu->regs[10] = (uint64_t)error;
    vcpu->regs[11] = result;
    vcpu->pc += 4;
    return 1;
}

static int hyp_guest_read_gpa(hyp_vcpu_t *vcpu, uint64_t gpa, uint64_t *out, unsigned count)
{
    if ((gpa & (PAGE_SIZE - 1)) + count > PAGE_SIZE) return 0;
    pfn_t pfn;
    if (!vcpu->vm->backend->pin || vcpu->vm->backend->pin(vcpu->vm, gpa, &pfn))
        return 0;
    const volatile uint8_t *p = pfn_to_virt(pfn) + (gpa & (PAGE_SIZE - 1));
    *out = 0;
    for (unsigned i = 0; i < count; i++) *out |= (uint64_t)p[i] << (i * 8);
    frame_put(pfn);
    return 1;
}

static int hyp_guest_read_va(hyp_vcpu_t *vcpu, uint64_t va, uint64_t *out, unsigned count)
{
    uint64_t vsatp = hyp_arch_vsatp_get();

    uint64_t gva = va;
    uint64_t mode = vsatp >> 31;
    if (mode != 0) {
        if (mode != 1)
            return 0;
        uint64_t gpa = (vsatp & ((1ULL << 22) - 1)) << 12;

        int level;
        int leaf = 0;
        unsigned leaf_level = 0;
        for (level = ARCH_PT_ROOT_LEVEL; level >= 0; level--) {
            uint64_t pte;
            if (!hyp_guest_read_gpa(vcpu, gpa +
                                    ((va >> (12 + 10 * level)) & 0x3ff) * 4, &pte, 4))
                return 0;
            if (!(pte & PTE_V))
                return 0;
            gpa = arch_pte_addr(pte);
            if (arch_pte_is_leaf(pte)) {
                leaf = 1;
                leaf_level = (unsigned)level;
                break;
            }
        }
        if (!leaf)
            return 0;

        unsigned bits = 12 + 10 * leaf_level;
        gva = gpa | (va & ((1ULL << bits) - 1));
    }
    return hyp_guest_read_gpa(vcpu, gva, out, count);
}

struct hyp_guest_access {
    uint64_t va;
    uint64_t value;
    unsigned rd;
    int      len;
    int      store;
    int      sign;
    int      ilen;
    int      fp;
};

static int hyp_guest_read_insn(hyp_vcpu_t *vcpu, uint32_t *insn)
{
    uint64_t lo, hi;
    if (!hyp_guest_read_va(vcpu, (uint32_t)vcpu->pc, &lo, 2)) return 0;
    *insn = (uint32_t)lo;
    if ((lo & 3) == 3) {
        if (!hyp_guest_read_va(vcpu, (uint32_t)(vcpu->pc + 2), &hi, 2)) return 0;
        *insn |= (uint32_t)hi << 16;
    }
    return 1;
}

static uint64_t hyp_sext(uint64_t v, unsigned bits)
{
    return (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
}

static inline unsigned hyp_rvc_reg3(unsigned f)
{
    return 8u + (f & 7u);
}

static int hyp_decode_rvc_access(hyp_vcpu_t *vcpu, uint32_t insn,
                                 struct hyp_guest_access *a)
{
    uint16_t c = (uint16_t)insn;
    unsigned funct3 = (unsigned)((c >> 13) & 7);
    unsigned rdp    = hyp_rvc_reg3((unsigned)((c >> 2) & 7));
    unsigned rs1p   = hyp_rvc_reg3((unsigned)((c >> 7) & 7));

    a->ilen = 2;
    a->rd   = 0;
    a->sign = 0;
    a->value = 0;
    a->fp   = 0;

    switch (c & 3) {
    case 0:
        switch (funct3) {
        case 0x0: return 0;
        case 0x2: {
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 6) & 1) << 2) |
                           ((uint64_t)((c >> 5) & 1) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 4; a->store = 0; a->sign = 1; a->rd = rdp;
            return 1;
        }
        case 0x3: {
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 6) & 1) << 2) |
                           ((uint64_t)((c >> 5) & 1) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 4; a->store = 0; a->sign = 0; a->rd = rdp; a->fp = 1;
            return 1;
        }

        case 0x1: {
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 5) & 3) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 8; a->store = 0; a->sign = 0; a->fp = 1;
            return 1;
        }
        case 0x5: {
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 5) & 3) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 8; a->store = 1; a->fp = 1;
            return 1;
        }
        case 0x6: {
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 6) & 1) << 2) |
                           ((uint64_t)((c >> 5) & 1) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 4; a->store = 1; a->value = vcpu->regs[rdp];
            return 1;
        }
        case 0x7: {
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 6) & 1) << 2) |
                           ((uint64_t)((c >> 5) & 1) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 4; a->store = 1; a->fp = 1;
            return 1;
        }
        default: return 0;
        }

    case 2:
        switch (funct3) {
        case 0x0: return 0;
        case 0x1: {
            uint64_t off = ((uint64_t)((c >> 2) & 7) << 6) |
                           ((uint64_t)((c >> 12) & 1) << 5) |
                           ((uint64_t)((c >> 5) & 3) << 3);
            a->va = vcpu->regs[2] + off;
            a->len = 8; a->store = 0; a->sign = 0; a->fp = 1;
            a->rd = 0;
            return 1;
        }
        case 0x2: {
            uint64_t off = ((uint64_t)((c >> 2) & 3) << 6) |
                           ((uint64_t)((c >> 12) & 1) << 5) |
                           ((uint64_t)((c >> 4) & 7) << 2);
            a->va = vcpu->regs[2] + off;
            a->len = 4; a->store = 0; a->sign = 1;
            a->rd = (unsigned)((c >> 7) & 0x1f);
            return 1;
        }
        case 0x3: {
            uint64_t off = ((uint64_t)((c >> 2) & 3) << 6) |
                           ((uint64_t)((c >> 12) & 1) << 5) |
                           ((uint64_t)((c >> 4) & 7) << 2);
            a->va = vcpu->regs[2] + off;
            a->len = 4; a->store = 0; a->sign = 0; a->fp = 1;
            a->rd = (unsigned)((c >> 7) & 0x1f);
            return 1;
        }
        case 0x5: {
            uint64_t off = ((uint64_t)((c >> 7) & 7) << 6) |
                           ((uint64_t)((c >> 10) & 7) << 3);
            a->va = vcpu->regs[2] + off;
            a->len = 8; a->store = 1; a->fp = 1;
            a->value = 0;
            return 1;
        }
        case 0x6: {
            uint64_t off = ((uint64_t)((c >> 7) & 3) << 6) |
                           ((uint64_t)((c >> 9) & 0xf) << 2);
            a->va = vcpu->regs[2] + off;
            a->len = 4; a->store = 1;
            a->value = vcpu->regs[(unsigned)((c >> 2) & 0x1f)];
            return 1;
        }
        case 0x7: {
            uint64_t off = ((uint64_t)((c >> 7) & 3) << 6) |
                           ((uint64_t)((c >> 9) & 0xf) << 2);
            a->va = vcpu->regs[2] + off;
            a->len = 4; a->store = 1; a->fp = 1;
            a->value = vcpu->regs[(unsigned)((c >> 2) & 0x1f)];
            return 1;
        }
        default: return 0;
        }

    default:

        return 0;
    }
}

static int hyp_decode_guest_access(hyp_vcpu_t *vcpu, struct hyp_guest_access *a)
{
    uint32_t insn;
    if (!hyp_guest_read_insn(vcpu, &insn))
        return 0;
    if ((insn & 3) != 3)
        return hyp_decode_rvc_access(vcpu, insn, a);

    a->ilen = 4;
    a->fp   = 0;

    unsigned opcode = insn & 0x7f;
    unsigned funct3 = (insn >> 12) & 7;
    unsigned rs1    = (insn >> 15) & 0x1f;
    unsigned rd     = (insn >> 7) & 0x1f;
    uint64_t base   = vcpu->regs[rs1];

    if (opcode == 0x03) {
        switch (funct3) {
        case 0: a->len = 1; a->sign = 1; break;
        case 1: a->len = 2; a->sign = 1; break;
        case 2: a->len = 4; a->sign = 1; break;
        case 3: return 0;
        case 4: a->len = 1; a->sign = 0; break;
        case 5: a->len = 2; a->sign = 0; break;
        case 6: return 0;
        default: return 0;
        }
        a->va    = base + hyp_sext((uint64_t)(insn >> 20), 12);
        a->store = 0;
        a->rd    = rd;
        a->value = 0;
        return 1;
    }

    if (opcode == 0x23) {
        switch (funct3) {
        case 0: a->len = 1; break;
        case 1: a->len = 2; break;
        case 2: a->len = 4; break;
        case 3: return 0;
        default: return 0;
        }
        uint32_t imm = (uint32_t)(((insn >> 25) << 5) | ((insn >> 7) & 0x1f));
        a->va    = base + hyp_sext(imm, 12);
        a->store = 1;
        a->rd    = 0;
        a->sign  = 0;
        a->value = vcpu->regs[(insn >> 20) & 0x1f];
        return 1;
    }

    if (opcode == 0x07 && (funct3 == 2 || funct3 == 3)) {
        a->va    = base + hyp_sext((uint64_t)(insn >> 20), 12);
        a->len   = funct3 == 2 ? 4 : 8; a->store = 0; a->sign = 0;
        a->rd    = 0; a->value = 0; a->fp = 1;
        return 1;
    }

    if (opcode == 0x27 && (funct3 == 2 || funct3 == 3)) {
        uint32_t imm = (uint32_t)(((insn >> 25) << 5) | ((insn >> 7) & 0x1f));
        a->va    = base + hyp_sext(imm, 12);
        a->len   = funct3 == 2 ? 4 : 8; a->store = 1; a->sign = 0;
        a->rd    = 0; a->value = 0; a->fp = 1;
        return 1;
    }

    return 0;
}

static uint64_t hyp_extend_loaded(uint64_t value, int len, int sign)
{
    uint64_t mask = (len >= 8) ? ~0ULL : ((1ULL << (len * 8)) - 1);
    value &= mask;
    if (sign && len < 8 && (value & (1ULL << (len * 8 - 1))))
        value |= ~mask;
    return value;
}

static int hyp_guest_mem_fault(hyp_vcpu_t *vcpu, uint64_t scause,
                               uint64_t htval)
{

    uint64_t gpage = (htval << 2) & ~(uint64_t)(PAGE_SIZE - 1);
    extern pte_t hyp_riscv_s2_prot(hyp_vm_t *, uint64_t);
    pte_t access = scause == HYP_SCAUSE_GPF_INST ? PTE_X :
                   scause == HYP_SCAUSE_GPF_STORE ? PTE_W : PTE_R;
    pte_t present = hyp_riscv_s2_prot(vcpu->vm, gpage);
    if (present && !(present & access)) {
        hyp_vcpu_record_fault(vcpu, scause, gpage, htval);
        return 0;
    }


    if (scause == HYP_SCAUSE_GPF_INST) {

        int rc = hyp_ram_fill(vcpu->vm, gpage);
        if (rc == 0)
            return 1;
        kerr("hyp: guest instruction fetch fault gpa=%lx pc=%lx rc=%d\n",
             (unsigned long)gpage, (unsigned long)vcpu->pc, rc);
        hyp_vcpu_record_fault(vcpu, scause, gpage, htval);
        return 0;
    }

    struct hyp_guest_access a;
    if (!hyp_decode_guest_access(vcpu, &a)) {

        uint32_t raw = 0;
        hyp_guest_read_insn(vcpu, &raw);
        kerr("hyp: undecodable guest access at pc=%lx (insn=%x%s)\n",
             (unsigned long)vcpu->pc, raw,
             (raw & 3) == 3 ? "" : " rvc");
        hyp_vcpu_record_fault(vcpu, scause, gpage, htval);
        return 0;
    }
    uint64_t gpa = gpage | (a.va & (PAGE_SIZE - 1));

    int rc = hyp_ram_fill(vcpu->vm, gpage);
    if (rc == 0)
        return 1;
    if (rc != -EFAULT) {
        kerr("hyp: guest RAM fill gpa=%lx pc=%lx rc=%d\n",
             (unsigned long)gpage, (unsigned long)vcpu->pc, rc);
        hyp_vcpu_record_fault(vcpu, scause, gpa, htval);
        return 0;
    }

    if (a.fp) {
        kerr("hyp: guest FP %s to MMIO gpa=%lx pc=%lx\n",
             a.store ? "store" : "load", (unsigned long)gpa,
             (unsigned long)vcpu->pc);
        hyp_vcpu_record_fault(vcpu, scause, gpa, htval);
        return 0;
    }

    uint64_t value = a.value;
    int io = hyp_dev_mmio(vcpu->vm, gpa, a.store, &value, a.len);
    if (io == -EAGAIN) { vcpu->exit = HYP_EXIT_YIELD; return 0; }
    if (io <= 0) {
        kerr("hyp: no device for guest %s of %d byte(s) at gpa=%lx\n",
             a.store ? "store" : "load", a.len, (unsigned long)gpa);
        hyp_vcpu_record_fault(vcpu, scause, gpa, htval);
        return 0;
    }

    if (!a.store && a.rd)
        vcpu->regs[a.rd] = hyp_extend_loaded(value, a.len, a.sign);

    vcpu->pc += (uint64_t)a.ilen;
    return 1;
}

#define HYP_CSR_SATP      0x180
#define HYP_CSR_VSTIMECMP 0x14d
#define HYP_OP_SYSTEM     0x73

#define HYP_SYS_WFI        0x105u
#define HYP_SYS_FENCE_F7   0x09u

static void hyp_write_vsatp(uint64_t v)
{
    hyp_arch_vsatp_set(v);

    hyp_arch_host_tlb_fence();
}

static int hyp_virt_inst_word(hyp_vcpu_t *vcpu, uint64_t stval,
                              uint32_t *insn)
{
    if (stval && (stval & 3) == 3 &&
        (stval & 0x7f) == HYP_OP_SYSTEM) {
        *insn = (uint32_t)stval;
        return 1;
    }
    return hyp_guest_read_insn(vcpu, insn);
}

static int hyp_emulate_virt_inst(hyp_vcpu_t *vcpu, uint64_t stval)
{
    uint32_t insn;
    if (!hyp_virt_inst_word(vcpu, stval, &insn))
        return -1;

    unsigned opcode = insn & 0x7f;
    unsigned funct3 = (insn >> 12) & 7;
    if (opcode != HYP_OP_SYSTEM)
        return -1;

    if (funct3 == 0) {
        unsigned imm   = (insn >> 20) & 0xfff;
        unsigned f7    = (insn >> 25) & 0x7f;

        if (imm == HYP_SYS_WFI) {

            vcpu->pc += 4;
            if (vcpu->vm->managed_console) vcpu->exit = HYP_EXIT_YIELD;
            return 0;
        }
        if (f7 == HYP_SYS_FENCE_F7) {

            hyp_arch_host_tlb_fence();
            vcpu->pc += 4;
            return 0;
        }
        if (imm == 0 || imm == 1)
            return -1;
        kerr("hyp: guest virtual instruction %08lx is not a SYSTEM CSR op "
             "this slice emulates\n", (unsigned long)insn);
        return -1;
    }

    if (funct3 != 1 && funct3 != 2 && funct3 != 3 &&
        funct3 != 5 && funct3 != 6 && funct3 != 7)
        return -1;

    unsigned rs1 = (insn >> 15) & 0x1f;
    unsigned rd  = (insn >> 7) & 0x1f;
    unsigned csr = insn >> 20;
    uint64_t src = (funct3 >= 5) ? (uint64_t)rs1 : vcpu->regs[rs1];
    uint64_t old, next;

    if (csr == HYP_CSR_SATP) {
        uint64_t v = hyp_arch_vsatp_get();
        old = v;
        next = ((funct3 & 3) == 1) ? src
             : ((funct3 & 3) == 2) ? (old | src)
             : (old & ~src);
        hyp_write_vsatp(next);
    } else if (csr == HYP_CSR_VSTIMECMP || csr == 0x15d) {

        if (funct3 != 1 && funct3 != 5)
            return -1;
        if (rd) {
            old = (uint32_t)(hyp_arch_vstimecmp_get() >> (csr == 0x15d ? 32 : 0));
        } else {
            old = 0;
        }
        uint64_t full = hyp_arch_vstimecmp_get();
        next = csr == 0x15d ? ((uint64_t)(uint32_t)src << 32) | (uint32_t)full
                           : (full & 0xffffffff00000000ULL) | (uint32_t)src;
        hyp_write_vstimecmp(next);
    } else {

        kerr("hyp: guest virtual instruction wrote CSR 0x%03x, which this "
             "slice does not emulate\n", csr);
        return -1;
    }

    if (rd)
        vcpu->regs[rd] = old;
    vcpu->pc += 4;
    return 0;
}

#define HYP_TRAP_TRACE_HEAD   24
#define HYP_TRAP_TRACE_REPEAT 256

int hyp_vcpu_handle_trap(hyp_vcpu_t *vcpu, void *trap_frame,
                         uint64_t scause, uint64_t stval, uint64_t htval)
{
    (void)trap_frame;

    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC || !vcpu->running)
        return 0;

    hyp_dev_pump_rx(vcpu->vm);

    if (scause & HYP_SCAUSE_INTR_BIT)
        return 1;

    uint64_t cause = scause & ~HYP_SCAUSE_INTR_BIT;
    int traced = (cause != HYP_SCAUSE_ECALL_VU &&
                  cause != HYP_SCAUSE_ECALL_VS &&
                  cause != HYP_SCAUSE_ECALL_S);

    if (traced && vcpu->trap_seen < HYP_TRAP_TRACE_HEAD) {
        kerr("hyp: trap #%lu scause=%lx pc=%lx stval=%lx htval=%lx\n",
             (unsigned long)vcpu->trap_seen, (unsigned long)scause,
             (unsigned long)vcpu->pc, (unsigned long)stval,
             (unsigned long)htval);
    }
    if (traced)
        vcpu->trap_seen++;

    if (traced && vcpu->pc == vcpu->trap_loop_pc &&
        scause == vcpu->trap_loop_scause) {
        if (++vcpu->trap_loop_run == HYP_TRAP_TRACE_REPEAT) {
            kerr("hyp: trap loop at pc=%lx scause=%lx (%lu traps so far)\n",
                 (unsigned long)vcpu->pc, (unsigned long)scause,
                 (unsigned long)vcpu->trap_seen);
        }
    } else {
        vcpu->trap_loop_pc    = vcpu->pc;
        vcpu->trap_loop_scause = scause;
        vcpu->trap_loop_run   = 1;
    }

    switch (scause & ~HYP_SCAUSE_INTR_BIT) {
    case HYP_SCAUSE_ECALL_VU:
    case HYP_SCAUSE_ECALL_VS:
    case HYP_SCAUSE_ECALL_S:
        return hyp_guest_ecall(vcpu, scause);

    case HYP_SCAUSE_GPF_INST:
    case HYP_SCAUSE_GPF_LOAD:
    case HYP_SCAUSE_GPF_STORE:
        return hyp_guest_mem_fault(vcpu, scause, htval);

    case HYP_SCAUSE_VIRT_INST:
        if (hyp_emulate_virt_inst(vcpu, stval) == 0)
            return 1;

        kerr("hyp: guest virtual instruction fault not emulated "
             "(pc=%lx stval=%lx)\n", (unsigned long)vcpu->pc,
             (unsigned long)stval);
        hyp_vcpu_record_fault(vcpu, scause, stval, htval);
        return 0;

    case HYP_SCAUSE_FETCH_PF:
    case HYP_SCAUSE_LOAD_PF:
    case HYP_SCAUSE_STORE_PF:

        kerr("hyp: undelegated guest page fault scause=%lu stval=%lx\n",
             (unsigned long)scause, (unsigned long)stval);
        hyp_vcpu_record_fault(vcpu, scause, stval, htval);
        return 0;

    default:
        kerr("hyp: unhandled guest trap scause=%lu stval=%lx\n",
             (unsigned long)scause, (unsigned long)stval);
        hyp_vcpu_record_fault(vcpu, scause, stval, htval);
        return 0;
    }
}
