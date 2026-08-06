/*
 * A20OS kernel extension programs (KEP) — verifier, interpreter, registry.
 *
 * See kernel/include/ext/kep.h for the design contract.  This file owns:
 *  - the 32-bit instruction encoding;
 *  - the linear-scan verifier (forward jumps only, context-bound memory,
 *    in-range register and offset fields, program must end with EXIT);
 *  - the interpreter (fixed instruction budget, no loops by construction);
 *  - the extension-point registry and attach/detach lifetime rules.
 *
 * Program lifetime: kep_prog_load() returns a process-local fd; the fd
 * holds one reference.  kep_prog_attach() takes an additional reference
 * held by each extension point that runs the program.  The kernel drops
 * its references when the owning process dies (fd table cleanup) or on
 * detach, whichever comes last.
 */

#include "ext/kep.h"

#include "core/klog.h"
#include "core/string.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

/* ---- instruction encoding (32-bit) ----
 * bits 31-28 opcode | 27-24 rd | 23-20 rs | 19-16 aux | 15-0 imm/off
 */
#define KEP_OP_MASK   0xF0000000U
#define KEP_OP_SHIFT  28
#define KEP_RD_MASK   0x0F000000U
#define KEP_RD_SHIFT  24
#define KEP_RS_MASK   0x00F00000U
#define KEP_RS_SHIFT  20
#define KEP_AUX_MASK  0x000F0000U
#define KEP_AUX_SHIFT 16
#define KEP_IMM_MASK  0x0000FFFFU

enum {
    KEP_OP_MOVI = 0, KEP_OP_MOV, KEP_OP_LDC, KEP_OP_STC,
    KEP_OP_ALU,  KEP_OP_ADDI, KEP_OP_ANDI, KEP_OP_ORI,
    KEP_OP_XORI, KEP_OP_SHLI, KEP_OP_SHRI, KEP_OP_JMP,
    KEP_OP_JCC,  KEP_OP_EXIT,
};

enum {
    KEP_ALU_ADD = 0, KEP_ALU_SUB, KEP_ALU_AND, KEP_ALU_OR,
    KEP_ALU_XOR, KEP_ALU_SHL, KEP_ALU_SHR, KEP_ALU_NEG,
};

enum { KEP_CC_EQ = 0, KEP_CC_NE, KEP_CC_LT, KEP_CC_LE, KEP_CC_GT, KEP_CC_GE };
#define KEP_CC_SIGNED 8

#define KEP_MAX_PROGS 64
#define KEP_MAX_POINTS 16

typedef struct kep_prog {
    int used;
    int id;               /* stable kernel-owned id (1-based) */
    int owner_pid;
    uint32_t insns[KEP_MAX_PROG_INSNS];
    uint32_t ninsns;
    uint32_t nwords;      /* required context size (max LDC/STC offset) */
    int refs;             /* owner reference + one per attachment */
} kep_prog_t;

static kep_prog_t kep_progs[KEP_MAX_PROGS];
static kep_point_t *kep_points[KEP_MAX_POINTS];
static int kep_next_id = 1;
static spinlock_t kep_registry_lock = SPINLOCK_INIT;

/* ---- verifier ---- */

static int kep_insn_field(uint32_t insn, uint32_t mask, unsigned shift,
                          unsigned width)
{
    unsigned v = (insn & mask) >> shift;
    return v < (1U << width);
}

static int kep_verify(const uint32_t *insns, uint32_t ninsns,
                      uint32_t *nwords_out)
{
    if (!insns || ninsns == 0 || ninsns > KEP_MAX_PROG_INSNS)
        return -EINVAL;

    uint32_t max_off = 0;

    for (uint32_t pc = 0; pc < ninsns; pc++) {
        uint32_t insn = insns[pc];
        unsigned op = (insn & KEP_OP_MASK) >> KEP_OP_SHIFT;
        unsigned rd = (insn & KEP_RD_MASK) >> KEP_RD_SHIFT;
        unsigned rs = (insn & KEP_RS_MASK) >> KEP_RS_SHIFT;
        unsigned aux = (insn & KEP_AUX_MASK) >> KEP_AUX_SHIFT;
        uint32_t imm = insn & KEP_IMM_MASK;

        switch (op) {
        case KEP_OP_MOVI:
        case KEP_OP_ADDI:
        case KEP_OP_ANDI:
        case KEP_OP_ORI:
        case KEP_OP_XORI:
        case KEP_OP_SHLI:
        case KEP_OP_SHRI:
            if (rd >= KEP_REGS) return -EINVAL;
            break;
        case KEP_OP_MOV:
        case KEP_OP_ALU:
            if (rd >= KEP_REGS || rs >= KEP_REGS) return -EINVAL;
            if (op == KEP_OP_ALU && aux > KEP_ALU_NEG) return -EINVAL;
            break;
        case KEP_OP_LDC:
            /* imm = context word offset; reads 8 bytes at words[imm]. */
            if (rd >= KEP_REGS) return -EINVAL;
            if (imm > KEP_MAX_CONTEXT_WORDS - 1) return -EINVAL;
            if (imm + 1 > max_off) max_off = imm + 1;
            break;
        case KEP_OP_STC:
            if (rs >= KEP_REGS) return -EINVAL;
            if (imm > KEP_MAX_CONTEXT_WORDS - 1) return -EINVAL;
            if (imm + 1 > max_off) max_off = imm + 1;
            break;
        case KEP_OP_JMP:
            /* Forward only: target = pc + 1 + imm; imm is unsigned, so a
             * target of pc+1 (fall-through) is the minimum. */
            if (pc + 1 + imm >= ninsns) return -EINVAL;
            break;
        case KEP_OP_JCC:
            if (rd >= KEP_REGS || rs >= KEP_REGS) return -EINVAL;
            if ((aux & ~KEP_CC_SIGNED) > KEP_CC_GE) return -EINVAL;
            if (pc + 1 + imm >= ninsns) return -EINVAL;
            break;
        case KEP_OP_EXIT:
            /* EXIT terminates; it may appear at any position, but the
             * program must end with one so execution can never fall off
             * the end. */
            break;
        default:
            return -EINVAL;
        }
    }

    if ((insns[ninsns - 1] & KEP_OP_MASK) != (KEP_OP_EXIT << KEP_OP_SHIFT))
        return -EINVAL;
    if (max_off == 0)
        return -EINVAL; /* useless program: never touches the context */
    if (nwords_out)
        *nwords_out = max_off;
    return 0;
}

/* ---- interpreter ---- */

static uint32_t kep_exec(const kep_prog_t *p, kep_ctx_t *ctx)
{
    uint64_t regs[KEP_REGS] = {0};
    uint32_t pc = 0;
    uint32_t steps = 0;

    while (steps++ < KEP_MAX_PROG_INSNS * 4) {
        uint32_t insn = p->insns[pc];
        unsigned op = (insn & KEP_OP_MASK) >> KEP_OP_SHIFT;
        unsigned rd = (insn & KEP_RD_MASK) >> KEP_RD_SHIFT;
        unsigned rs = (insn & KEP_RS_MASK) >> KEP_RS_SHIFT;
        unsigned aux = (insn & KEP_AUX_MASK) >> KEP_AUX_SHIFT;
        uint32_t imm = insn & KEP_IMM_MASK;
        int64_t simm = (int64_t)(int32_t)(imm << 16) >> 16;

        switch (op) {
        case KEP_OP_MOVI: regs[rd] = (uint64_t)simm; pc++; break;
        case KEP_OP_MOV:  regs[rd] = regs[rs]; pc++; break;
        case KEP_OP_LDC:
            regs[rd] = (imm < ctx->nwords) ? ctx->words[imm] : 0;
            pc++;
            break;
        case KEP_OP_STC:
            if (imm < ctx->nwords)
                ctx->words[imm] = regs[rs];
            pc++;
            break;
        case KEP_OP_ALU:
            switch (aux) {
            case KEP_ALU_ADD: regs[rd] += regs[rs]; break;
            case KEP_ALU_SUB: regs[rd] -= regs[rs]; break;
            case KEP_ALU_AND: regs[rd] &= regs[rs]; break;
            case KEP_ALU_OR:  regs[rd] |= regs[rs]; break;
            case KEP_ALU_XOR: regs[rd] ^= regs[rs]; break;
            case KEP_ALU_SHL: regs[rd] <<= regs[rs] & 63; break;
            case KEP_ALU_SHR: regs[rd] >>= regs[rs] & 63; break;
            case KEP_ALU_NEG: regs[rd] = ~regs[rd] + 1; break;
            }
            pc++;
            break;
        case KEP_OP_ADDI: regs[rd] += (uint64_t)simm; pc++; break;
        case KEP_OP_ANDI: regs[rd] &= imm; pc++; break;
        case KEP_OP_ORI:  regs[rd] |= imm; pc++; break;
        case KEP_OP_XORI: regs[rd] ^= imm; pc++; break;
        case KEP_OP_SHLI: regs[rd] <<= imm & 63; pc++; break;
        case KEP_OP_SHRI: regs[rd] >>= imm & 63; pc++; break;
        case KEP_OP_JMP:  pc += 1 + imm; break;
        case KEP_OP_JCC: {
            uint64_t a = regs[rd], b = regs[rs];
            int taken = 0;
            switch (aux & ~KEP_CC_SIGNED) {
            case KEP_CC_EQ: taken = a == b; break;
            case KEP_CC_NE: taken = a != b; break;
            case KEP_CC_LT:
                taken = (aux & KEP_CC_SIGNED) ?
                        (int64_t)a < (int64_t)b : a < b;
                break;
            case KEP_CC_LE:
                taken = (aux & KEP_CC_SIGNED) ?
                        (int64_t)a <= (int64_t)b : a <= b;
                break;
            case KEP_CC_GT:
                taken = (aux & KEP_CC_SIGNED) ?
                        (int64_t)a > (int64_t)b : a > b;
                break;
            case KEP_CC_GE:
                taken = (aux & KEP_CC_SIGNED) ?
                        (int64_t)a >= (int64_t)b : a >= b;
                break;
            }
            pc += taken ? 1 + imm : 1;
            break;
        }
        case KEP_OP_EXIT:
        default:
            return (uint32_t)regs[0];
        }
    }
    return 0; /* budget exhausted: treat as allow, verifier excludes loops */
}

/* ---- registry ---- */

int kep_register_point(kep_point_t *pt)
{
    if (!pt || pt->nwords == 0 || pt->nwords > KEP_MAX_CONTEXT_WORDS)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&kep_registry_lock);
    for (int i = 0; i < KEP_MAX_POINTS; i++) {
        if (kep_points[i] && kep_points[i]->id == pt->id) {
            spin_unlock_irqrestore(&kep_registry_lock, flags);
            return -EEXIST;
        }
    }
    for (int i = 0; i < KEP_MAX_POINTS; i++) {
        if (!kep_points[i]) {
            spin_init(&pt->lock);
            kep_points[i] = pt;
            spin_unlock_irqrestore(&kep_registry_lock, flags);
            kdebug("[KEP] point %u '%s' registered (%u words)\n",
                   pt->id, pt->name, pt->nwords);
            return 0;
        }
    }
    spin_unlock_irqrestore(&kep_registry_lock, flags);
    return -ENOSPC;
}

static kep_point_t *kep_point_find(uint32_t id)
{
    for (int i = 0; i < KEP_MAX_POINTS; i++) {
        if (kep_points[i] && kep_points[i]->id == id)
            return kep_points[i];
    }
    return NULL;
}

int kep_point_query(uint32_t point_id, char *name, size_t name_len,
                    uint32_t *nwords_out)
{
    uint64_t flags = spin_lock_irqsave(&kep_registry_lock);
    kep_point_t *pt = kep_point_find(point_id);
    if (!pt) {
        spin_unlock_irqrestore(&kep_registry_lock, flags);
        return -ENOENT;
    }
    if (name && name_len && pt->name) {
        strncpy(name, pt->name, name_len - 1);
        name[name_len - 1] = '\0';
    }
    if (nwords_out)
        *nwords_out = pt->nwords;
    spin_unlock_irqrestore(&kep_registry_lock, flags);
    return 0;
}

/* ---- program management ---- */

int kep_prog_load(const uint32_t *instr, uint32_t len)
{
    task_t *cur = proc_current();
    if (!cur || !instr)
        return -EINVAL;

    uint32_t kbuf[KEP_MAX_PROG_INSNS];
    if (len > KEP_MAX_PROG_INSNS)
        return -E2BIG;
    if (copy_from_user(kbuf, instr, len * sizeof(uint32_t)) < 0)
        return -EFAULT;

    uint32_t nwords = 0;
    int vr = kep_verify(kbuf, len, &nwords);
    if (vr < 0)
        return vr;

    int slot = -1;
    uint64_t flags = spin_lock_irqsave(&kep_registry_lock);
    for (int i = 0; i < KEP_MAX_PROGS; i++) {
        if (!kep_progs[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        spin_unlock_irqrestore(&kep_registry_lock, flags);
        return -ENOSPC;
    }
    kep_prog_t *p = &kep_progs[slot];
    memset(p, 0, sizeof(*p));
    memcpy(p->insns, kbuf, len * sizeof(uint32_t));
    p->ninsns = len;
    p->nwords = nwords;
    p->used = 1;
    p->id = kep_next_id++;
    if (kep_next_id <= 0)
        kep_next_id = 1;
    p->owner_pid = cur->pid;
    p->refs = 1;
    spin_unlock_irqrestore(&kep_registry_lock, flags);

    kdebug("[KEP] pid %d loaded prog %d (%u insns, %u ctx words)\n",
           cur->pid, p->id, len, nwords);
    return p->id;
}

static kep_prog_t *kep_prog_by_id(int prog_id)
{
    if (prog_id <= 0)
        return NULL;
    for (int i = 0; i < KEP_MAX_PROGS; i++) {
        if (kep_progs[i].used && kep_progs[i].id == prog_id)
            return &kep_progs[i];
    }
    return NULL;
}

/* Resolve a program id with an ownership check against the caller. */
static kep_prog_t *kep_prog_owned(int prog_id)
{
    task_t *cur = proc_current();
    if (!cur)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&kep_registry_lock);
    kep_prog_t *p = kep_prog_by_id(prog_id);
    if (p && p->owner_pid == cur->pid) {
        spin_unlock_irqrestore(&kep_registry_lock, flags);
        return p;
    }
    spin_unlock_irqrestore(&kep_registry_lock, flags);
    return NULL;
}

int kep_prog_attach(int prog_id, uint32_t point_id)
{
    kep_prog_t *p = kep_prog_owned(prog_id);
    if (!p)
        return -ENOENT;

    uint64_t flags = spin_lock_irqsave(&kep_registry_lock);
    kep_point_t *pt = kep_point_find(point_id);
    if (!pt || pt->nwords < p->nwords) {
        spin_unlock_irqrestore(&kep_registry_lock, flags);
        return -EINVAL;
    }
    kep_attached_t *a = kmalloc(sizeof(*a));
    if (!a) {
        spin_unlock_irqrestore(&kep_registry_lock, flags);
        return -ENOMEM;
    }
    a->prog = p;
    a->next = pt->attached;
    pt->attached = a;
    p->refs++;
    spin_unlock_irqrestore(&kep_registry_lock, flags);
    return 0;
}

int kep_prog_detach(int prog_id, uint32_t point_id)
{
    kep_prog_t *p = kep_prog_owned(prog_id);
    if (!p)
        return -ENOENT;

    uint64_t flags = spin_lock_irqsave(&kep_registry_lock);
    for (int i = 0; i < KEP_MAX_POINTS; i++) {
        kep_point_t *pt = kep_points[i];
        if (!pt || (point_id != 0 && pt->id != point_id))
            continue;
        kep_attached_t **pp = &pt->attached;
        while (*pp) {
            kep_attached_t *a = *pp;
            if (a->prog == p) {
                *pp = a->next;
                kfree(a);
                p->refs--;
            } else {
                pp = &a->next;
            }
        }
    }
    spin_unlock_irqrestore(&kep_registry_lock, flags);
    return 0;
}

int kep_prog_release(int prog_id)
{
    uint64_t flags = spin_lock_irqsave(&kep_registry_lock);
    kep_prog_t *p = kep_prog_by_id(prog_id);
    if (!p) {
        spin_unlock_irqrestore(&kep_registry_lock, flags);
        return -ENOENT;
    }
    if (p->owner_pid != (proc_current() ? proc_current()->pid : -1)) {
        spin_unlock_irqrestore(&kep_registry_lock, flags);
        return -EPERM;
    }
    if (p->refs > 0)
        p->refs--;
    if (p->refs == 0)
        p->used = 0;
    spin_unlock_irqrestore(&kep_registry_lock, flags);
    return 0;
}

void kep_release_process(int pid)
{
    if (pid <= 0)
        return;
    uint64_t flags = spin_lock_irqsave(&kep_registry_lock);
    for (int i = 0; i < KEP_MAX_PROGS; i++) {
        kep_prog_t *p = &kep_progs[i];
        if (!p->used || p->owner_pid != pid)
            continue;
        /* Unlink from every extension point, then drop the owner ref. */
        for (int j = 0; j < KEP_MAX_POINTS; j++) {
            kep_point_t *pt = kep_points[j];
            if (!pt)
                continue;
            kep_attached_t **pp = &pt->attached;
            while (*pp) {
                kep_attached_t *a = *pp;
                if (a->prog == p) {
                    *pp = a->next;
                    kfree(a);
                    p->refs--;
                } else {
                    pp = &a->next;
                }
            }
        }
        if (p->refs <= 0)
            p->used = 0;
    }
    spin_unlock_irqrestore(&kep_registry_lock, flags);
}

uint32_t kep_point_run(kep_point_t *pt, kep_ctx_t *ctx)
{
    if (!pt || !ctx)
        return 0;
    uint32_t verdict = 0;
    uint64_t flags = spin_lock_irqsave(&pt->lock);
    for (kep_attached_t *a = pt->attached; a; a = a->next) {
        verdict = kep_exec(a->prog, ctx);
        if (verdict != 0)
            break; /* first non-allow verdict wins, attach order */
    }
    spin_unlock_irqrestore(&pt->lock, flags);
    return verdict;
}
