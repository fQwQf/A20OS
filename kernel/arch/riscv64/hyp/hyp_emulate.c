/* RISC-V guest instruction, SBI and fault decoding. */
#include "hyp/hyp_vcpu.h"
#include "hyp/hyp_sbi.h"
#include "mm/frame.h"
#include "core/cpu.h"
#include "core/klog.h"
#include "core/stdio.h"
#include "core/string.h"
/* Architectural ECALL causes: U=8, HS=9, VS=10. Only VS supervisor
 * calls are SBI; VU system calls belong to the guest kernel. Guest stage-1
 * faults are delegated, while G-stage and virtual-instruction traps are ours. */
#define HYP_SCAUSE_ECALL_U   8ULL
#define HYP_SCAUSE_ECALL_HS   9ULL
#define HYP_SCAUSE_ECALL_VS   10ULL
#define HYP_SCAUSE_FETCH_PF  12ULL
#define HYP_SCAUSE_LOAD_PF   13ULL
#define HYP_SCAUSE_STORE_PF  15ULL
#define HYP_SCAUSE_GPF_INST  20ULL
#define HYP_SCAUSE_GPF_LOAD  21ULL
#define HYP_SCAUSE_VIRT_INST 22ULL
#define HYP_SCAUSE_GPF_STORE 23ULL
#define HYP_SCAUSE_INTR_BIT   (1ULL << 63)

static void hyp_vcpu_record_fault(hyp_vcpu_t *vcpu, uint64_t scause,
                                  uint64_t stval, uint64_t htval)
{
    vcpu->exit_scause = scause;
    vcpu->exit_stval  = stval;
    vcpu->exit_htval  = htval;
    vcpu->exit        = HYP_EXIT_FAULT;
}

/* ---- second-stage faults: what a guest memory access that leaves stage-1
 * walk becomes ----
 *
 * The faulting instruction has not retired.  QEMU raises the second-stage
 * exception from the load/store slow path before the access (raise_mmu_exception,
 * target/riscv/cpu_helper.c:1773-1810) and leaves pc on the instruction, so
 * the guest's destination register and its store buffer still hold their old
 * contents when this C code runs.  That is what makes the two exits below
 * legal: after a successful mapping the instruction is simply retried, and
 * after an emulated MMIO access the run loop writes the result itself and
 * steps over the instruction.
 */

/* One 64-bit word of guest memory by GUEST PHYSICAL address.  Stage-2 is the
 * only map the host has for guest addresses; a GPA it does not translate is
 * simply not readable from here. */
static int hyp_guest_read_gpa(hyp_vcpu_t *vcpu, uint64_t gpa, uint64_t *out)
{
    paddr_t pa = hyp_s2_translate(vcpu->vm, gpa);
    if (!pa)
        return 0;
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return 0;
    *out = *(volatile uint64_t *)(pfn_to_virt(pfn) + (gpa & (PAGE_SIZE - 1)));
    return 1;
}

/* The guest's own stage-1, walked in software.  Once the guest kernel turns
 * paging on, its pc and its effective addresses are VS-virtual, and nothing in
 * the host can translate them: the host MMU walks the HOST's satp, and vsatp
 * roots a tree that lives in guest physical memory.  The run loop needs the
 * instruction word (to know an access's width, direction and operand) and the
 * hardware only hands it the guest PHYSICAL fault address, so the Sv39 walk
 * the guest's own MMU would do is done here, with every table fetch served by
 * stage-2.  Only the answer the decoder needs is computed: A/D/U are not
 * checked, and a walk that is wrong about permissions still yields the page the
 * hardware itself just used. */
static int hyp_guest_read_va(hyp_vcpu_t *vcpu, uint64_t va, uint64_t *out)
{
    uint64_t vsatp = hyp_arch_vsatp_get();

    uint64_t gva = va;
    uint64_t mode = (vsatp >> 60) & 0xf;
    if (mode != 0) {
        if (mode != 8)              /* Sv39 is the only guest shape this walks */
            return 0;
        uint64_t gpa = (vsatp & ((1ULL << 44) - 1)) << 12;   /* root PPN */
        /* level >= 0, not level > 0: the loop has to fetch the level-0 entry,
         * which IS the leaf.  With level > 0 it stopped after VPN2/VPN1, never
         * looked at a leaf, and `level == 0` below then refused every walk --
         * so once the guest turned paging on, nothing in it decoded and every
         * data access fell into hyp_guest_mem_fault's terminal exit.  This is
         * the shape the tree's own walks use (kernel/mm/mm.c:228,
         * kernel/mm/mm.c:554); `level > 0` is the ALLOCATION shape, where the
         * caller has already been handed the table to put a leaf in. */
        int level;
        int leaf = 0;
        unsigned leaf_level = 0;
        for (level = ARCH_PT_ROOT_LEVEL; level >= 0; level--) {
            uint64_t pte;
            if (!hyp_guest_read_gpa(vcpu, gpa +
                                    ((va >> (12 + 9 * level)) & 0x1ff) * 8, &pte))
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
        if (!leaf)                /* ran out of levels without finding one */
            return 0;
        /* arch_pte_addr() is the PPN shifted up by 12, which for a leaf found
         * at level L is only the first 4KB of a 1<<(12 + 9*L) region: the rest
         * of the guest's offset inside that region lives in va itself and is
         * NOT in gpa.  ORing just the page offset therefore resolves every
         * address to the START of the superpage -- for the guest this walk
         * exists to serve (A20OS maps its whole physical window with 1GB
         * megapages) that is a different 4KB page than the one the guest is
         * executing, so the instruction could not be read back and the faulting
         * access decoded as nothing at all.  The whole covered range is what
         * goes on: PPN << 12 | va & (range - 1). */
        unsigned bits = 12 + 9 * leaf_level;
        gva = gpa | (va & ((1ULL << bits) - 1));
    }
    return hyp_guest_read_gpa(vcpu, gva, out);
}

/* What the faulting instruction was asking for.  Only the shapes a device
 * model can serve are decoded: the LOAD and STORE encodings, 32-bit and RVC,
 * with the width and signedness straight out of the RISC-V load/store funct3
 * table (lb/lh/lw and c.lw sign-extend, lbu/lhu/lwu zero-extend, ld/c.ld are
 * 8 bytes).  An AMO or an SC is NOT decoded: answering one would also have to
 * write the old value back into rd, which no device in this slice asks for,
 * so it stays a recorded fault instead of a half-implemented access.
 *
 * ilen is the ENCODED length, not the access width: an access served here is
 * stepped over rather than retried (see hyp_guest_mem_fault), and a 16-bit
 * encoding advances sepc by two.  Getting that wrong does not fault -- it just
 * resumes the guest in the middle of some other instruction. */
struct hyp_guest_access {
    uint64_t va;      /* effective address the instruction computed */
    uint64_t value;   /* store data (already the guest register) */
    unsigned rd;      /* load destination register */
    int      len;     /* access width in bytes */
    int      store;
    int      sign;    /* sign-extend the loaded value (rv64 rule) */
    int      ilen;    /* 2 for RVC, 4 for the 32-bit encodings */
    int      fp;      /* FP transfer: address and width are real, but the
                       * register side is an FPR this model does not hold --
                       * fill-and-retry only, never emulated against MMIO */
};

static int hyp_guest_read_insn(hyp_vcpu_t *vcpu, uint32_t *insn)
{
    uint64_t word;
    if (!hyp_guest_read_va(vcpu, vcpu->pc, &word))
        return 0;
    /* The low halfword is enough for both encodings: a 32-bit instruction and
     * a 16-bit one are told apart by insn & 3, and every bit an RVC load/store
     * uses lives below 16.  Reading a full word at a pc whose page ends here is
     * fine too -- the G-stage fault is page granular and the tail byte is
     * never consulted. */
    *insn = (uint32_t)word;
    return 1;
}

/* Immediate field, sign-extended from its encoded width (both load and store
 * immediates are 12 bits). */
static uint64_t hyp_sext(uint64_t v, unsigned bits)
{
    return (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
}

/* The 3-bit compressed register fields name x8..x15 as 0..7. */
static inline unsigned hyp_rvc_reg3(unsigned f)
{
    return 8u + (f & 7u);
}

/*
 * One 16-bit (RVC) load or store, decoded into the same shape the 32-bit
 * encodings fill in.  RVC does not give these a funct3 table to index -- every
 * encoding here carries its own immediate layout -- so the immediates are
 * assembled bit by bit from the halfword, and the 32-bit I/S-type helper
 * (hyp_sext over insn >> 20) must NOT be used on them: those fields overlap
 * differently in every one of the eight encodings.
 *
 * WHICH OF THEM EXIST IN A GUEST IS A MEASURED QUESTION.  Objdumping the
 * A20OS guest image and counting (riscv64-unknown-elf-objdump -d -M no-aliases
 * kernel-nosyms.elf | grep -oE "\bc\.[a-z0-9._]+" | sort | uniq -c) shows the
 * memory forms in use: c.ldsp 74501, c.sdsp 62465, c.ld 5848, c.lw 3699,
 * c.sd 1361, c.swsp 1301, c.sw 1065, c.lwsp 1052 -- exactly the eight decoded
 * here.  Decoding only the one instruction that first wedged the guest (a
 * c.sw) just moves the same undecodable failure to the next c.sdsp, so all
 * eight land together.
 *
 * WHAT THE FP FORMS DO HERE (c.fldsp and c.fsdsp, 84 and 56 occurrences in
 * the guest image, plus c.fld/c.fsd and the 32-bit fld/fsd): address and width
 * only.  Guest entry programs sstatus with FS=Initial
 * (kernel/arch/riscv64/hyp/hyp_vcpu_asm.S), because mstatus.FS is a HOST-level
 * gate -- a guest raising FS from VS-mode only reaches vsstatus, so with
 * FS=Off at entry the guest's own trap handler kept faulting on the fsd that
 * saves its FP frame and recursed off the stack.  With FS live, an FP access
 * over a not-yet-filled page is a real second-stage access fault this decoder
 * must answer: it answers with va/len, hyp_ram_fill() populates the page, and
 * the GUEST RETRIES THE INSTRUCTION -- hardware does the FPR transfer itself,
 * because this model has no guest FPR file to put the data in or take it
 * from.  rd and value stay zero (an FPR index must never land in the GPR
 * file), and hyp_guest_mem_fault records a fault instead of emulating FP
 * against MMIO, where the value would have to come from or go to an FPR the
 * model cannot see.  RV64 has no flw/fsw (those are RV32-only), so nothing
 * memory-shaped is left unenumerated.
 */
static int hyp_decode_rvc_access(hyp_vcpu_t *vcpu, uint32_t insn,
                                 struct hyp_guest_access *a)
{
    uint16_t c = (uint16_t)insn;
    unsigned funct3 = (unsigned)((c >> 13) & 7);
    unsigned rdp    = hyp_rvc_reg3((unsigned)((c >> 2) & 7));  /* inst[4:2] */
    unsigned rs1p   = hyp_rvc_reg3((unsigned)((c >> 7) & 7));  /* inst[9:7] */

    a->ilen = 2;
    a->rd   = 0;
    a->sign = 0;
    a->value = 0;
    a->fp   = 0;

    switch (c & 3) {
    case 0:     /* quadrant 0: the *4SPN and * forms, x8..x15 only */
        switch (funct3) {
        case 0x0: return 0;    /* c.addi4spn: register-immediate, no access */
        case 0x2: {            /* c.lw  off[5:3]=inst[12:10] [2]=inst[6] [6]=inst[5] */
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 6) & 1) << 2) |
                           ((uint64_t)((c >> 5) & 1) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 4; a->store = 0; a->sign = 1; a->rd = rdp;
            return 1;
        }
        case 0x3: {            /* c.ld  off[5:3]=inst[12:10] [7:6]=inst[6:5] */
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 5) & 3) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 8; a->store = 0; a->sign = 0; a->rd = rdp;
            return 1;
        }
        /* The FP slots: funct3=0x1 is c.fld, 0x5 is c.fsd.  Both carry the
         * immediate layout of the integer twin sitting right beside them
         * (c.ld / c.sd above), which is the only part that matters here: the
         * destination or source register is an FPR, so rd and value stay at
         * their zero defaults and the fill-and-retry path never consults
         * either (see the block comment above this function). */
        case 0x1: {            /* c.fld  same offsets as c.ld */
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 5) & 3) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 8; a->store = 0; a->sign = 0; a->fp = 1;
            return 1;
        }
        case 0x5: {            /* c.fsd  same offsets as c.sd */
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 5) & 3) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 8; a->store = 1; a->fp = 1;
            return 1;
        }
        case 0x6: {            /* c.sw  same offsets as c.lw, rs2' = inst[4:2] */
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 6) & 1) << 2) |
                           ((uint64_t)((c >> 5) & 1) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 4; a->store = 1; a->value = vcpu->regs[rdp];
            return 1;
        }
        case 0x7: {            /* c.sd  same offsets as c.ld */
            uint64_t off = ((uint64_t)((c >> 10) & 7) << 3) |
                           ((uint64_t)((c >> 5) & 3) << 6);
            a->va = vcpu->regs[rs1p] + off;
            a->len = 8; a->store = 1; a->value = vcpu->regs[rdp];
            return 1;
        }
        default: return 0;
        }

    case 2:     /* quadrant 2: the *SP forms, full 5-bit registers off x2 */
        switch (funct3) {
        case 0x0: return 0;    /* c.slli */
        case 0x1: {            /* c.fldsp rd=inst[11:7], an FPR
                                *   off[5]=inst[12] [4:3]=inst[6:5] [8:6]=inst[4:2]
                                *   -- same immediate as the c.ldsp below */
            uint64_t off = ((uint64_t)((c >> 2) & 7) << 6) |
                           ((uint64_t)((c >> 12) & 1) << 5) |
                           ((uint64_t)((c >> 5) & 3) << 3);
            a->va = vcpu->regs[2] + off;
            a->len = 8; a->store = 0; a->sign = 0; a->fp = 1;
            a->rd = 0;   /* inst[11:7] is an FPR index, not an x register */
            return 1;
        }
        case 0x2: {            /* c.lwsp rd=inst[11:7]
                                *   off[5]=inst[12] [4:2]=inst[6:4] [7:6]=inst[3:2] */
            uint64_t off = ((uint64_t)((c >> 2) & 3) << 6) |
                           ((uint64_t)((c >> 12) & 1) << 5) |
                           ((uint64_t)((c >> 4) & 7) << 2);
            a->va = vcpu->regs[2] + off;
            a->len = 4; a->store = 0; a->sign = 1;
            a->rd = (unsigned)((c >> 7) & 0x1f);
            return 1;
        }
        case 0x3: {            /* c.ldsp rd=inst[11:7]
                                *   off[5]=inst[12] [4:3]=inst[6:5] [8:6]=inst[4:2] */
            uint64_t off = ((uint64_t)((c >> 2) & 7) << 6) |
                           ((uint64_t)((c >> 12) & 1) << 5) |
                           ((uint64_t)((c >> 5) & 3) << 3);
            a->va = vcpu->regs[2] + off;
            a->len = 8; a->store = 0; a->sign = 0;
            a->rd = (unsigned)((c >> 7) & 0x1f);
            return 1;
        }
        case 0x5: {            /* c.fsdsp rs2=inst[6:2], an FPR
                                *   off[5:3]=inst[12:10] [8:6]=inst[9:7]
                                *   -- same immediate as the c.sdsp below */
            uint64_t off = ((uint64_t)((c >> 7) & 7) << 6) |
                           ((uint64_t)((c >> 10) & 7) << 3);
            a->va = vcpu->regs[2] + off;
            a->len = 8; a->store = 1; a->fp = 1;
            a->value = 0;   /* the store data is an FPR, not vcpu->regs[] */
            return 1;
        }
        case 0x6: {            /* c.swsp rs2=inst[6:2]
                                *   off[5:2]=inst[12:9] [7:6]=inst[8:7] */
            uint64_t off = ((uint64_t)((c >> 7) & 3) << 6) |
                           ((uint64_t)((c >> 9) & 0xf) << 2);
            a->va = vcpu->regs[2] + off;
            a->len = 4; a->store = 1;
            a->value = vcpu->regs[(unsigned)((c >> 2) & 0x1f)];
            return 1;
        }
        case 0x7: {            /* c.sdsp rs2=inst[6:2]
                                *   off[5:3]=inst[12:10] [8:6]=inst[9:7] */
            uint64_t off = ((uint64_t)((c >> 7) & 7) << 6) |
                           ((uint64_t)((c >> 10) & 7) << 3);
            a->va = vcpu->regs[2] + off;
            a->len = 8; a->store = 1;
            a->value = vcpu->regs[(unsigned)((c >> 2) & 0x1f)];
            return 1;
        }
        default: return 0;
        }

    default:
        /* Quadrant 1 (c.addi, c.li, c.j, c.bnez, the MISC-ALU group, ...) has
         * no load or store in it at all, and c.addi4spn above is the only
         * quadrant-0 form that looks load-shaped and is not. */
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

    if (opcode == 0x03) {                       /* LOAD: imm is I-type, 12-bit */
        switch (funct3) {
        case 0: a->len = 1; a->sign = 1; break;  /* lb  */
        case 1: a->len = 2; a->sign = 1; break;  /* lh  */
        case 2: a->len = 4; a->sign = 1; break;  /* lw  */
        case 3: a->len = 8; a->sign = 0; break;  /* ld  */
        case 4: a->len = 1; a->sign = 0; break;  /* lbu */
        case 5: a->len = 2; a->sign = 0; break;  /* lhu */
        case 6: a->len = 4; a->sign = 0; break;  /* lwu */
        default: return 0;
        }
        a->va    = base + hyp_sext((uint64_t)(insn >> 20), 12);
        a->store = 0;
        a->rd    = rd;
        a->value = 0;
        return 1;
    }

    if (opcode == 0x23) {                       /* STORE: imm is S-type, 12-bit */
        switch (funct3) {
        case 0: a->len = 1; break;               /* sb */
        case 1: a->len = 2; break;               /* sh */
        case 2: a->len = 4; break;               /* sw */
        case 3: a->len = 8; break;               /* sd */
        default: return 0;
        }
        uint32_t imm = (uint32_t)(((insn >> 25) << 5) | ((insn >> 7) & 0x1f));
        a->va    = base + hyp_sext(imm, 12);
        a->store = 1;
        a->rd    = 0;
        a->sign  = 0;
        a->value = vcpu->regs[(insn >> 20) & 0x1f];   /* rs2 */
        return 1;
    }

    /* The FP memory ops of RV64: fld/fsd (opcode 0x07/0x27, funct3=3).  Same
     * I/S-type immediates as ld/sd; rd/rs2 are FPR indices, so the GPR-facing
     * rd and value stay zero and only the address and width are handed up for
     * fill-and-retry.  RV32's flw/fsw (funct3=2) are deliberately absent --
     * there is no RV32 guest on this path -- and fall through to the undecodable
     * answer below. */
    if (opcode == 0x07 && funct3 == 3) {           /* fld */
        a->va    = base + hyp_sext((uint64_t)(insn >> 20), 12);
        a->len   = 8; a->store = 0; a->sign = 0;
        a->rd    = 0; a->value = 0; a->fp = 1;
        return 1;
    }

    if (opcode == 0x27 && funct3 == 3) {           /* fsd */
        uint32_t imm = (uint32_t)(((insn >> 25) << 5) | ((insn >> 7) & 0x1f));
        a->va    = base + hyp_sext(imm, 12);
        a->len   = 8; a->store = 1; a->sign = 0;
        a->rd    = 0; a->value = 0; a->fp = 1;
        return 1;
    }

    return 0;
}

/* The rv64 rule for what a load leaves in rd: the low len bytes, sign-extended
 * for lb/lh/lw and zero-extended for lbu/lhu/lwu/ld. */
static uint64_t hyp_extend_loaded(uint64_t value, int len, int sign)
{
    uint64_t mask = (len >= 8) ? ~0ULL : ((1ULL << (len * 8)) - 1);
    value &= mask;
    if (sign && len < 8 && (value & (1ULL << (len * 8 - 1))))
        value |= ~mask;
    return value;
}

/* One guest memory access that could not be walked by stage-2.  Two exits,
 * in the order the contract fixes them:
 *
 *   gpa inside the RAM window -> hyp_ram_fill(), and the guest retries the
 *     instruction itself.  A fill that fails is NOT papered over: the window
 *     owns that address, so the answer is a recorded fault.
 *   otherwise -> hyp_dev_mmio().  The access is served HERE (the model returns
 *     the data, or takes the value), so the instruction is stepped over
 *     instead of retried -- retrying would fault again on the same unmapped
 *     page and loop forever.
 */
static int hyp_guest_mem_fault(hyp_vcpu_t *vcpu, uint64_t scause,
                               uint64_t htval)
{
    /* htval is the guest physical address of the faulting access, shifted
     * right by two.  QEMU composes it as (im_address | (address &
     * (TARGET_PAGE_SIZE - 1))) >> 2 (target/riscv/cpu_helper.c:1997, the
     * G-stage branch of riscv_cpu_tlb_fill), so htval << 2 carries the
     * INTRA-PAGE OFFSET as well as the page -- it is not a page base.  The
     * offset is masked off here because every consumer below wants a page:
     * hyp_ram_fill() rejects a non-page-aligned gpa outright (-EINVAL), and
     * the fetches of a page-granular table are keyed by page.
     *
     * The intra-page offset for the reported GPA comes from the instruction's
     * effective address instead, which is the same thing QEMU ORs in:
     * translation is page granular, so the offset the guest computed survives
     * into the GPA unchanged. */
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
        /* A fetch has neither width nor operand, so the device model is not
         * its place; a missing code page is the RAM window's to answer, and if
         * it cannot, executing a zero page is the guest's own illegal
         * instruction trap to report. */
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
        /* The raw word rides along because this line is the only thing between
         * "the guest stopped here" and knowing WHICH encoding it stopped on:
         * a 16-bit halfword disassembled by eye names the missing form in one
         * step, and the guess is not worth making -- an access this model
         * cannot decode is a recorded fault, never an improvised one. */
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
        return 1;                 /* RAM: the guest retries the instruction */
    if (rc != -EFAULT) {
        kerr("hyp: guest RAM fill gpa=%lx pc=%lx rc=%d\n",
             (unsigned long)gpage, (unsigned long)vcpu->pc, rc);
        hyp_vcpu_record_fault(vcpu, scause, gpa, htval);
        return 0;
    }

    /* An FP access against a device would have to move data between an FPR
     * and the device model, and this vcpu carries no FPR file: emulating it
     * would hand the device a GPR's value or hand the FPR a zero.  The RAM
     * path above answers FP faults the only honest way there is -- fill the
     * page and let the guest retry the instruction in hardware -- so reaching
     * this line with fp set means the access was never going to be emulated.
     * Record it as the fault it is rather than faking a transfer. */
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

    /* rd 0 is not a discard sink on this path, it is x0 -- hardwired zero in
     * hardware, so a load into it leaves no trace and the register file must
     * not be touched at all.  The guard is explicit rather than borrowed from
     * hyp_frame_to_vcpu() re-zeroing regs[0] on every guest trap: that is an
     * invariant in a different file, and a writeback that depended on it would
     * silently corrupt a banked x0 the day it stopped holding. */
    if (!a.store && a.rd)
        vcpu->regs[a.rd] = hyp_extend_loaded(value, a.len, a.sign);
    /* The access happened; do not redo it -- and step over the ENCODED length,
     * which is 2 for the RVC encodings.  A fixed +4 here resumes the guest
     * half-way into whatever follows, which shows up much later as a stray
     * illegal instruction or a walk into somebody else's function rather than
     * as anything to do with this line. */
    vcpu->pc += (uint64_t)a.ilen;
    return 1;
}

/* ---- scause 22: a VS-mode instruction trapped to HS-mode ----
 *
 * With hstatus.VTVM=1 the guest cannot touch its own memory-management CSRs,
 * so a real guest kernel raises this the first time it writes satp (QEMU
 * gates the satp CSR on VTVM, target/riscv/csr.c:626-634; SFENCE.VMA is gated
 * the same way).  The instruction arrives in the trap, not only in memory:
 * QEMU puts env->bins into stval for a virtual instruction fault
 * (cpu_helper.c:2405-2407), and htinst carries the same word when the guest
 * faults are reported with a transformed instruction.  Decoding stval first is
 * therefore both cheaper and more robust than a memory read -- it still works
 * when the guest's own page tables are what just broke -- and the memory read
 * stays as the fallback for a platform that leaves stval at zero.
 *
 * hstatus.SPV is 1 on this path, so this IS a guest trap: decoding and
 * re-executing one instruction on the guest's behalf is the same contract the
 * ecall path already runs under.
 */
#define HYP_CSR_SATP      0x180
#define HYP_CSR_VSTIMECMP 0x14d
#define HYP_OP_SYSTEM     0x73
/* SYSTEM funct3=0 carries the operation in imm[11:0], and the two that matter
 * here are distinct values: WFI is 0x105, SFENCE.VMA is funct7=0x09 with rs1
 * and rd riding in the operand fields (which is why the all-zero form is
 * 0x120 -- the boot code's "sfence.vma" at 0x8020011e encodes as 0x12000073,
 * the same bits a WFI-shaped imm would occupy but a different instruction).
 * ECALL (0x000) and EBREAK (0x001) are NOT ours to step over: the first
 * arrives as an ecall cause, the second is delegated. */
#define HYP_SYS_WFI        0x105u
#define HYP_SYS_FENCE_F7   0x09u

static void hyp_write_vsatp(uint64_t v)
{
    hyp_arch_vsatp_set(v);
    /* Changing the VS-stage root does NOT implicitly drop the entries cached
     * under the old one.  The guest's satp write arrives here rather than
     * reaching the CSR directly (hstatus.VTVM traps it), so this is the only
     * place the switch happens and the only place the matching fence can be
     * issued: without it this hart keeps resolving guest addresses through the
     * page tables the guest just abandoned.  The guest's own SFENCE.VMA (also
     * trapped, see hyp_emulate_virt_inst) covers the edits it makes after the
     * switch; this covers the switch itself. */
    hyp_arch_host_tlb_fence();
}

/* The trapped instruction, from the trap report when it carries one.
 * QEMU's stval for this cause is env->bins, so a word whose low two bits are
 * 11 and whose opcode is SYSTEM is the instruction itself; anything else (a
 * zero, or a transformed-instruction pseudo-op such as the 0x00003000 the
 * G-stage walk failures carry) is not decodable and the memory read decides. */
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
            /* Never park a managed VMM inside hardware WFI. Return an idle
             * slice so its host process can service input and cancellation. */
            vcpu->pc += 4;
            if (vcpu->vm->managed_console) vcpu->exit = HYP_EXIT_YIELD;
            return 0;
        }
        if (f7 == HYP_SYS_FENCE_F7) {
            /* SFENCE.VMA.  In HS-mode it is not gated, and it invalidates the
             * whole hart's translations, which is a superset of what the guest
             * asked for. */
            hyp_arch_host_tlb_fence();
            vcpu->pc += 4;
            return 0;
        }
        if (imm == 0 || imm == 1)
            return -1;       /* ECALL / EBREAK: not this path's to step over */
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
        next = (funct3 == 1) ? src
             : (funct3 == 2) ? (old | src)
             : (old & ~src);
        hyp_write_vsatp(next);
    } else if (csr == HYP_CSR_VSTIMECMP) {
        /* A set/clear form on a CSR that only ever takes a value is refused
         * rather than approximated.  Reading the old value back is only done
         * when the instruction asks for it in rd, and it is safe to ask: a
         * virtual instruction fault out of this CSR is only reachable when the
         * Sstc extension is implemented at all (the gate checks ext_sstc
         * before the henvcfg test, target/riscv/csr.c:572-614). */
        if (funct3 != 1 && funct3 != 5)
            return -1;
        if (rd) {
            old = hyp_arch_vstimecmp_get();
        } else {
            old = 0;
        }
        next = src;
        hyp_arch_vstimecmp_set(next);
    } else {
        /* Every other CSR is a recorded fault, never a silent pass-through:
         * the alternative is to let the guest believe a VS-mode CSR write
         * took effect when it did not, which is the failure mode this whole
         * emulation path exists to avoid.  The CSR number goes to stval --
         * that is the register the guest chose it with -- and the kerr names
         * it so the log says which one. */
        kerr("hyp: guest virtual instruction wrote CSR 0x%03x, which this "
             "slice does not emulate\n", csr);
        return -1;
    }

    if (rd)
        vcpu->regs[rd] = old;
    vcpu->pc += 4;
    return 0;
}

/* ---- bounded guest-trap trace ----
 *
 * A guest that stops talking looks identical from outside whether it is
 * wedged in one of its own loops or re-executing a trapped instruction the
 * host keeps re-trapping, and neither shape prints anything: the run loop
 * only speaks on a terminal exit, which neither of them reaches.  So the
 * dispatcher says where the guest was, twice over and both bounded:
 *
 *   - the first HYP_TRAP_TRACE_HEAD guest traps, in order, which bounds how
 *     far into the boot the guest actually got;
 *   - one line the first time any single (pc, scause) pair recurs
 *     HYP_TRAP_TRACE_REPEAT times, which names a trap loop outright.
 *
 * Both are counters on a slice that runs one guest on one CPU, which is the
 * same scope hyp_active_vcpu already claims for itself, so no locking is
 * needed and none is taken. */
#define HYP_TRAP_TRACE_HEAD   24
#define HYP_TRAP_TRACE_REPEAT 256


int hyp_vcpu_handle_trap(hyp_vcpu_t *vcpu, void *trap_frame,
                         uint64_t scause, uint64_t stval, uint64_t htval)
{
    (void)trap_frame;

    if (!vcpu || vcpu->magic != HYP_VCPU_MAGIC || !vcpu->running)
        return 0;

    /* Move host keystrokes into the guest's ingress ring, here, on every guest
     * trap -- before the cause is looked at at all.  Two things pin the spot,
     * and both are easy to get wrong:
     *
     *   BEFORE the host-IRQ early return below.  A host UART IRQ reaches HS
     *   with the interrupt bit set in scause, so that early return is exactly
     *   the path a new byte takes, and it is also the path the counters below
     *   never see (they start after it).  Pumping after it would mean a byte
     *   is invisible to the guest until some LATER trap came along, which is
     *   not a latency anyone can reason about.
     *
     *   AFTER the magic/running check above, because it dereferences
     *   vcpu->vm.  That is why this is not "pump once per run" somewhere
     *   convenient: by this point the VM is known good, and the call cannot
     *   fail.
     *
     * It is a drain of a non-blocking reader (hyp_dev.c spells out which one
     * and why), so an empty host ring costs one atomic load and a return. */
    hyp_dev_pump_rx(vcpu->vm);

    /* Host IRQ.  The host IRQ machinery has already run against this frame
     * by the time the dispatcher gets here; the guest just resumes. */
    if (scause & HYP_SCAUSE_INTR_BIT)
        return 1;

    /* A guest ecall is the guest TALKING, and the SBI dispatcher answers it
     * every time -- the vcpu test's guest spells one line this way.  Printing
     * a trace line between the character and the next one does not just add
     * noise, it splits the guest's own output across host lines: the line
     * smoke-hyp-vcpu anchors on (^HYP$) can then never appear, however
     * correct the SBI path underneath it is.  The trace exists to say where a
     * guest that STOPS got to, and an ecall is not that, so the handled
     * ecall causes stay out of both counters below.  Everything else -- page
     * faults, virtual instructions, unhandled causes -- is still traced. */
    uint64_t cause = scause & ~HYP_SCAUSE_INTR_BIT;
    int traced = (cause != HYP_SCAUSE_ECALL_U &&
                  cause != HYP_SCAUSE_ECALL_HS &&
                  cause != HYP_SCAUSE_ECALL_VS);

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
    case HYP_SCAUSE_ECALL_VS:
        return hyp_riscv_sbi_ecall(vcpu, scause);

    case HYP_SCAUSE_GPF_INST:
    case HYP_SCAUSE_GPF_LOAD:
    case HYP_SCAUSE_GPF_STORE:
        return hyp_guest_mem_fault(vcpu, scause, htval);

    case HYP_SCAUSE_VIRT_INST:
        if (hyp_emulate_virt_inst(vcpu, stval) == 0)
            return 1;
        /* Whatever else the guest did that traps here, this loop does not
         * know how to be the hypervisor for. */
        kerr("hyp: guest virtual instruction fault not emulated "
             "(pc=%lx stval=%lx)\n", (unsigned long)vcpu->pc,
             (unsigned long)stval);
        hyp_vcpu_record_fault(vcpu, scause, stval, htval);
        return 0;

    case HYP_SCAUSE_FETCH_PF:
    case HYP_SCAUSE_LOAD_PF:
    case HYP_SCAUSE_STORE_PF:
        /* The guest's own stage-1 fault.  v2 delegates these to VS-mode, so
         * reaching here means the delegation is not in force; the host has no
         * second page table to fill for the guest's user address space, so the
         * run exits with the fault recorded rather than pretending. */
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
