/*
 * A20OS kernel extension programs (KEP) — eBPF verifier, interpreter,
 * registry.
 *
 * See kernel/include/ext/kep.h for the design contract.  This file owns:
 *  - the verifier for the standard Linux eBPF instruction encoding
 *    (bpf_insn_t), using the KEP safety model: strictly forward jumps
 *    (structural termination), memory access limited to the extension
 *    point context window through R1 with constant offsets, bounded
 *    instruction count, program must end with EXIT;
 *  - the interpreter (fixed instruction budget, no loops by
 *    construction);
 *  - the extension-point registry and attach/detach lifetime rules.
 *
 * Program lifetime: kep_prog_load() returns a kernel-owned id; the owner
 * holds one reference.  kep_prog_attach() takes an additional reference
 * held by each extension point that runs the program.  The kernel drops
 * its references when the owning process dies or on detach, whichever
 * comes last.
 */

#include "ext/kep.h"

#include "core/klog.h"
#include "core/string.h"
#include "mm/slab.h"
#include "fs/fdtable.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

#define KEP_MAX_PROGS 64
#define KEP_MAX_POINTS 16

typedef struct kep_prog {
    int used;
    int id;               /* stable kernel-owned id (1-based) */
    int owner_pid;
    int owner_fd;         /* Linux ABI fd alias, -1 when unused */
    bpf_insn_t insns[KEP_MAX_PROG_INSNS];
    uint32_t ninsns;
    uint32_t max_ctx_off; /* highest context byte offset touched + 8 */
    int refs;             /* owner reference + one per attachment */
} kep_prog_t;

static kep_prog_t kep_progs[KEP_MAX_PROGS];
static kep_point_t *kep_points[KEP_MAX_POINTS];
static int kep_next_id = 1;
static spinlock_t kep_registry_lock = SPINLOCK_INIT;

/* ---- verifier ---- */

static int kep_verify(const bpf_insn_t *insns, uint32_t ninsns,
                      uint32_t *max_off_out)
{
    if (!insns || ninsns == 0 || ninsns > KEP_MAX_PROG_INSNS)
        return -EINVAL;

    uint32_t max_off = 0;

    for (uint32_t pc = 0; pc < ninsns; pc++) {
        const bpf_insn_t *insn = &insns[pc];
        uint8_t code = insn->code;
        unsigned cls = BPF_CLASS(code);
        unsigned dst = insn->dst_reg;
        unsigned src = insn->src_reg;
        int16_t off = insn->off;
        int32_t imm = insn->imm;

        switch (cls) {
        case BPF_LD:
            /* LD_IMM64 (double instruction): BPF_LD|BPF_DW|BPF_IMM. */
            if (BPF_SIZE(code) != BPF_DW || BPF_MODE(code) != BPF_IMM ||
                dst > 10) {
                /* absolute/indirect packet loads are not supported */
                if (BPF_MODE(code) == BPF_ABS || BPF_MODE(code) == BPF_IND)
                    return -EOPNOTSUPP;
                return -EINVAL;
            }
            if (pc + 1 >= ninsns)
                return -EINVAL; /* imm64 needs the second half */
            pc++;
            break;
        case BPF_LDX:
            /* Memory read: only R1 (ctx) with DW size and constant
             * non-negative offset within the context window. */
            if (src != BPF_REG_CTX || dst > 10 || BPF_SIZE(code) != BPF_DW ||
                BPF_MODE(code) != BPF_MEM || off < 0 ||
                (uint32_t)off + 8 > KEP_MAX_CONTEXT_BYTES)
                return -EINVAL;
            if ((uint32_t)off + 8 > max_off)
                max_off = (uint32_t)off + 8;
            break;
        case BPF_ST:
        case BPF_STX:
            /* Memory write: same context-window discipline. */
            if (BPF_SIZE(code) != BPF_DW || BPF_MODE(code) != BPF_MEM ||
                off < 0 || (uint32_t)off + 8 > KEP_MAX_CONTEXT_BYTES)
                return -EINVAL;
            if (cls == BPF_STX && dst != BPF_REG_CTX)
                return -EINVAL; /* only ctx-target stores */
            if (cls == BPF_ST && src != BPF_K)
                return -EINVAL;
            if ((uint32_t)off + 8 > max_off)
                max_off = (uint32_t)off + 8;
            break;
        case BPF_ALU:
        case BPF_ALU64: {
            unsigned op = BPF_OP(code);
            if (op > BPF_ARSH || (op == BPF_NEG && BPF_SRC(code) != BPF_K) ||
                dst > 10 || (BPF_SRC(code) == BPF_X && src > 10))
                return -EINVAL;
            break;
        }
        case BPF_JMP:
        case BPF_JMP32: {
            unsigned op = BPF_OP(code);
            if (op == BPF_CALL)
                return -EOPNOTSUPP; /* no helper calls yet */
            if (op == BPF_EXIT)
                break; /* EXIT may appear mid-program (branch target) */
            if (op > BPF_JSLE || dst > 10 ||
                (BPF_SRC(code) == BPF_X && src > 10))
                return -EINVAL;
            /* Strictly forward jumps: off must be positive. */
            if (off <= 0)
                return -EINVAL;
            if (pc + 1 + (uint32_t)off >= ninsns)
                return -EINVAL;
            break;
        }
        default:
            return -EINVAL;
        }
    }

    /* The program must end with EXIT so execution can never fall off the
     * end; unreachable code after a mid-program EXIT is allowed (it may be
     * a forward jump target). */
    if (BPF_CLASS(insns[ninsns - 1].code) != BPF_JMP ||
        BPF_OP(insns[ninsns - 1].code) != BPF_EXIT)
        return -EINVAL;
    if (max_off == 0)
        return -EINVAL; /* useless program: never touches the context */
    if (max_off_out)
        *max_off_out = max_off;
    return 0;
}

/* ---- interpreter ---- */

static uint64_t bpf_reg_read(const uint64_t *regs, unsigned reg)
{
    return regs[reg & 0xf];
}

static void bpf_reg_write(uint64_t *regs, unsigned reg, uint64_t val)
{
    regs[reg & 0xf] = val;
}

static uint32_t kep_exec(const kep_prog_t *p, kep_ctx_t *ctx)
{
    uint64_t regs[KEP_REGS] = {0};
    regs[BPF_REG_CTX] = (uint64_t)(uintptr_t)ctx->words;
    uint32_t pc = 0;
    uint32_t steps = 0;

    while (steps++ < KEP_MAX_PROG_INSNS * 8) {
        const bpf_insn_t *insn = &p->insns[pc];
        uint8_t code = insn->code;
        unsigned cls = BPF_CLASS(code);
        unsigned dst = insn->dst_reg & 0xf;
        unsigned src = insn->src_reg & 0xf;
        int16_t off = insn->off;
        int32_t imm = insn->imm;

        switch (cls) {
        case BPF_LD: {
            uint64_t v = (uint64_t)(uint32_t)imm;
            if (BPF_MODE(code) == BPF_IMM && BPF_SIZE(code) == BPF_DW) {
                uint64_t hi = (uint64_t)(uint32_t)p->insns[pc + 1].imm;
                v = (uint32_t)v | (hi << 32);
                pc += 2;
            } else {
                pc++;
            }
            bpf_reg_write(regs, dst, v);
            break;
        }
        case BPF_LDX: {
            uint64_t v = 0;
            if (off >= 0 && (uint32_t)off + 8 <= ctx->nwords * 8)
                v = *(uint64_t *)((char *)ctx->words + off);
            bpf_reg_write(regs, dst, v);
            pc++;
            break;
        }
        case BPF_ST: {
            uint64_t v = (uint64_t)(uint32_t)imm;
            if (BPF_SRC(code) == BPF_X)
                v = bpf_reg_read(regs, src);
            if (off >= 0 && (uint32_t)off + 8 <= ctx->nwords * 8)
                *(uint64_t *)((char *)ctx->words + off) = v;
            pc++;
            break;
        }
        case BPF_STX:
            if (dst == BPF_REG_CTX && off >= 0 &&
                (uint32_t)off + 8 <= ctx->nwords * 8)
                *(uint64_t *)((char *)ctx->words + off) =
                    bpf_reg_read(regs, src);
            pc++;
            break;
        case BPF_ALU:
        case BPF_ALU64: {
            uint64_t a = bpf_reg_read(regs, dst);
            uint64_t b = BPF_SRC(code) == BPF_X ?
                         bpf_reg_read(regs, src) :
                         (uint64_t)(uint32_t)imm;
            uint64_t r = a;
            switch (BPF_OP(code)) {
            case BPF_ADD: r = a + b; break;
            case BPF_SUB: r = a - b; break;
            case BPF_MUL: r = a * b; break;
            case BPF_DIV: r = b ? a / b : 0; break;
            case BPF_OR:  r = a | b; break;
            case BPF_AND: r = a & b; break;
            case BPF_LSH: r = a << (b & 63); break;
            case BPF_RSH: r = a >> (b & 63); break;
            case BPF_NEG: r = ~a + 1; break;
            case BPF_MOD: r = b ? a % b : 0; break;
            case BPF_XOR: r = a ^ b; break;
            case BPF_MOV: r = b; break;
            case BPF_ARSH: r = (int64_t)a >> (b & 63); break;
            }
            if (cls == BPF_ALU)
                r = (uint32_t)r; /* 32-bit ops zero-extend */
            bpf_reg_write(regs, dst, r);
            pc++;
            break;
        }
        case BPF_JMP:
        case BPF_JMP32: {
            unsigned op = BPF_OP(code);
            if (op == BPF_EXIT)
                return (uint32_t)bpf_reg_read(regs, 0);
            uint64_t a = bpf_reg_read(regs, dst);
            uint64_t b = BPF_SRC(code) == BPF_X ?
                         bpf_reg_read(regs, src) :
                         (uint64_t)(uint32_t)imm;
            if (cls == BPF_JMP32) {
                a = (uint32_t)a;
                b = (uint32_t)b;
            }
            int taken = 0;
            switch (op) {
            case BPF_JA:   taken = 1; break;
            case BPF_JEQ:  taken = a == b; break;
            case BPF_JGT:  taken = a > b; break;
            case BPF_JGE:  taken = a >= b; break;
            case BPF_JSET: taken = (a & b) != 0; break;
            case BPF_JNE:  taken = a != b; break;
            case BPF_JSGT: taken = (int64_t)a > (int64_t)b; break;
            case BPF_JSGE: taken = (int64_t)a >= (int64_t)b; break;
            case BPF_JLT:  taken = a < b; break;
            case BPF_JLE:  taken = a <= b; break;
            case BPF_JSLT: taken = (int64_t)a < (int64_t)b; break;
            case BPF_JSLE: taken = (int64_t)a <= (int64_t)b; break;
            }
            pc += taken ? 1 + (uint32_t)off : 1;
            break;
        }
        default:
            return 0; /* unreachable by construction */
        }
    }
    return 0; /* budget exhausted: verifier excludes loops */
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

int kep_prog_load(const bpf_insn_t *insns, uint32_t len)
{
    task_t *cur = proc_current();
    if (!cur || !insns)
        return -EINVAL;

    bpf_insn_t kbuf[KEP_MAX_PROG_INSNS];
    if (len > KEP_MAX_PROG_INSNS)
        return -E2BIG;
    if (copy_from_user(kbuf, insns, len * sizeof(bpf_insn_t)) < 0)
        return -EFAULT;

    uint32_t max_off = 0;
    int vr = kep_verify(kbuf, len, &max_off);
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
    memcpy(p->insns, kbuf, len * sizeof(bpf_insn_t));
    p->ninsns = len;
    p->max_ctx_off = max_off;
    p->used = 1;
    p->id = kep_next_id++;
    if (kep_next_id <= 0)
        kep_next_id = 1;
    p->owner_pid = cur->pid;
    p->owner_fd = -1;
    p->refs = 1;
    spin_unlock_irqrestore(&kep_registry_lock, flags);

    kdebug("[KEP] pid %d loaded prog %d (%u insns, ctx %u bytes)\n",
           cur->pid, p->id, len, max_off);
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
    if (!pt || pt->nwords * 8 < p->max_ctx_off) {
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

int kep_prog_set_fd(int prog_id, int fd)
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
    p->owner_fd = fd;
    spin_unlock_irqrestore(&kep_registry_lock, flags);
    return 0;
}

int kep_prog_find_by_fd(int fd)
{
    task_t *cur = proc_current();
    if (!cur || fd < 0)
        return -ENOENT;
    uint64_t flags = spin_lock_irqsave(&kep_registry_lock);
    for (int i = 0; i < KEP_MAX_PROGS; i++) {
        kep_prog_t *p = &kep_progs[i];
        if (p->used && p->owner_pid == cur->pid && p->owner_fd == fd) {
            spin_unlock_irqrestore(&kep_registry_lock, flags);
            return p->id;
        }
    }
    spin_unlock_irqrestore(&kep_registry_lock, flags);
    return -ENOENT;
}

/* Drop programs whose fd alias was closed by the owner (Linux ABI). */
void kep_sweep_fds(void)
{
    task_t *cur = proc_current();
    if (!cur)
        return;
    uint64_t flags = spin_lock_irqsave(&kep_registry_lock);
    for (int i = 0; i < KEP_MAX_PROGS; i++) {
        kep_prog_t *p = &kep_progs[i];
        if (!p->used || p->owner_pid != cur->pid || p->owner_fd < 0)
            continue;
        if (!fdtable_get_current(p->owner_fd)) {
            /* The fd is gone; detach everywhere and drop the owner ref. */
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
            p->owner_fd = -1;
            if (p->refs <= 0)
                p->used = 0;
        }
    }
    spin_unlock_irqrestore(&kep_registry_lock, flags);
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
