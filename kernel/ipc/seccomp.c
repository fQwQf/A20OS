#include "ipc/seccomp.h"

#include "core/klog.h"
#include "core/lock.h"
#include "core/signal_defs.h"
#include "core/string.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "abi/linux/syscall_nr.h"
#include "sys/usercopy.h"

/* linux/bpf_common.h opcode scaffolding */
#define BPF_CLASS(code) ((code) & 0x07)
#define BPF_LD    0x00
#define BPF_LDX   0x01
#define BPF_ST    0x02
#define BPF_STX   0x03
#define BPF_ALU   0x04
#define BPF_JMP   0x05
#define BPF_RET   0x06
#define BPF_MISC  0x07

#define BPF_SIZE(code) ((code) & 0x18)
#define BPF_W     0x00
#define BPF_H     0x08
#define BPF_B     0x10

#define BPF_MODE(code) ((code) & 0xe0)
#define BPF_IMM   0x00
#define BPF_ABS   0x20
#define BPF_IND   0x40
#define BPF_MEM   0x60
#define BPF_LEN   0x80
#define BPF_MSH   0xa0

#define BPF_OP(code) ((code) & 0xf0)
#define BPF_ADD   0x00
#define BPF_SUB   0x10
#define BPF_MUL   0x20
#define BPF_DIV   0x30
#define BPF_OR    0x40
#define BPF_AND   0x50
#define BPF_LSH   0x60
#define BPF_RSH   0x70
#define BPF_NEG   0x80
#define BPF_MOD   0x90
#define BPF_XOR   0xa0

#define BPF_JA    0x00
#define BPF_JEQ   0x10
#define BPF_JGT   0x20
#define BPF_JGE   0x30
#define BPF_JSET  0x40

#define BPF_SRC(code) ((code) & 0x08)
#define BPF_K     0x00
#define BPF_X     0x08

#define BPF_RVAL(code) ((code) & 0x18)
#define BPF_RVAL_A 0x10
#define BPF_RVAL_X 0x18

#define BPF_MISCOP(code) ((code) & 0xf0)
#define BPF_TAX   0x00
#define BPF_TXA   0x80

#define BPF_MAXINSNS 4096
#define BPF_MEMWORDS 16

typedef struct {
    uint16_t code;
    uint8_t jt;
    uint8_t jf;
    uint32_t k;
} sock_filter_t;

typedef struct seccomp_filter {
    int refs;
    struct seccomp_filter *prev;
    uint16_t len;
    sock_filter_t *insns;
} seccomp_filter_t;

typedef struct {
    uint32_t nr;
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t args[6];
} seccomp_data_wire_t;

static const uint32_t SECCOMP_ARCH_TOKEN =
    0xc0000000U | (uint32_t)ARCH_ELF_MACHINE; /* AUDIT_ARCH_<ISA> */

/* seccomp_data field offsets for ABS/IND loads */
#define SD_NR        0
#define SD_ARCH      4
#define SD_IP_LO     8
#define SD_IP_HI     12
#define SD_ARGS(i)   (16 + (i) * 8)
#define SD_SIZE      sizeof(seccomp_data_wire_t)

static int seccomp_check_load_off(uint16_t mode, uint32_t k, uint32_t size)
{
    if (mode == BPF_ABS)
        return (k > SD_SIZE || SD_SIZE - k < size) ? -1 : 0;
    /* BPF_IND: offset is X-relative, bounds checked at runtime. */
    return 0;
}

static int bpf_verify(const sock_filter_t *fp, int len)
{
    if (len < 1 || len > BPF_MAXINSNS)
        return -EINVAL;
    if (BPF_CLASS(fp[len - 1].code) != BPF_RET)
        return -EINVAL;

    for (int i = 0; i < len; i++) {
        const sock_filter_t *f = &fp[i];
        uint16_t code = f->code;

        switch (BPF_CLASS(code)) {
        case BPF_LD:
        case BPF_LDX:
            switch (BPF_MODE(code)) {
            case BPF_IMM:
                break;
            case BPF_ABS:
            case BPF_IND:
                if (BPF_SIZE(code) != BPF_W)
                    return -EINVAL;
                if (BPF_MODE(code) == BPF_ABS &&
                    seccomp_check_load_off(BPF_ABS, f->k, 4) < 0)
                    return -EINVAL;
                break;
            case BPF_MEM:
                if (BPF_SIZE(code) != BPF_W)
                    return -EINVAL;
                if (f->k >= BPF_MEMWORDS)
                    return -EINVAL;
                break;
            case BPF_LEN:
                if (BPF_SIZE(code) != BPF_W)
                    return -EINVAL;
                break;
            default:
                return -EINVAL;
            }
            break;
        case BPF_ST:
        case BPF_STX:
            if (BPF_SIZE(code) != BPF_W || BPF_MODE(code) != BPF_MEM)
                return -EINVAL;
            if (f->k >= BPF_MEMWORDS)
                return -EINVAL;
            break;
        case BPF_ALU:
            switch (BPF_OP(code)) {
            case BPF_ADD: case BPF_SUB: case BPF_MUL: case BPF_DIV:
            case BPF_OR: case BPF_AND: case BPF_LSH: case BPF_RSH:
            case BPF_MOD: case BPF_XOR:
                if (BPF_SRC(code) != BPF_K && BPF_SRC(code) != BPF_X)
                    return -EINVAL;
                break;
            case BPF_NEG:
                break;
            default:
                return -EINVAL;
            }
            break;
        case BPF_JMP:
            switch (BPF_OP(code)) {
            case BPF_JA:
                if ((int)i + 1 + (int)f->k >= len)
                    return -EINVAL;
                break;
            case BPF_JEQ: case BPF_JGT: case BPF_JGE: case BPF_JSET:
                if (BPF_SRC(code) != BPF_K && BPF_SRC(code) != BPF_X)
                    return -EINVAL;
                if ((int)i + 1 + f->jt >= len)
                    return -EINVAL;
                if ((int)i + 1 + f->jf >= len)
                    return -EINVAL;
                break;
            default:
                return -EINVAL;
            }
            break;
        case BPF_RET:
            if (BPF_RVAL(code) != BPF_K && BPF_RVAL(code) != BPF_RVAL_A &&
                BPF_RVAL(code) != BPF_RVAL_X)
                return -EINVAL;
            break;
        case BPF_MISC:
            switch (BPF_MISCOP(code)) {
            case BPF_TAX: case BPF_TXA:
                break;
            default:
                return -EINVAL;
            }
            break;
        default:
            return -EINVAL;
        }
    }
    return 0;
}

static uint32_t bpf_run(const sock_filter_t *insns, uint16_t len,
                        const seccomp_data_wire_t *sd)
{
    const unsigned char *data = (const unsigned char *)sd;
    uint32_t A = 0, X = 0;
    uint32_t M[BPF_MEMWORDS] = {0};

    for (uint32_t pc = 0; pc < len; ) {
        const sock_filter_t *f = &insns[pc];
        uint32_t k = f->k;
        pc++;

        switch (BPF_CLASS(f->code)) {
        case BPF_LD: case BPF_LDX:
            switch (BPF_MODE(f->code)) {
            case BPF_IMM:
                A = k;
                break;
            case BPF_ABS: {
                uint32_t n = k;
                if (n > SD_SIZE || SD_SIZE - n < 4)
                    return SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
                memcpy(&A, data + n, 4);
                break;
            }
            case BPF_IND: {
                uint32_t n = X + k;
                if (n < X || n > SD_SIZE || SD_SIZE - n < 4)
                    return SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
                memcpy(&A, data + n, 4);
                break;
            }
            case BPF_MEM:
                A = M[k & (BPF_MEMWORDS - 1)];
                break;
            case BPF_LEN:
                A = SD_SIZE;
                break;
            default:
                return SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
            }
            break;
        case BPF_ST:
            M[k & (BPF_MEMWORDS - 1)] = A;
            break;
        case BPF_STX:
            M[k & (BPF_MEMWORDS - 1)] = X;
            break;
        case BPF_ALU: {
            uint32_t src = (BPF_SRC(f->code) == BPF_X) ? X : k;
            switch (BPF_OP(f->code)) {
            case BPF_ADD: A += src; break;
            case BPF_SUB: A -= src; break;
            case BPF_MUL: A *= src; break;
            case BPF_DIV: A = src ? A / src : 0; break;
            case BPF_MOD: A = src ? A % src : 0; break;
            case BPF_OR:  A |= src; break;
            case BPF_AND: A &= src; break;
            case BPF_LSH: A = src < 32 ? A << src : 0; break;
            case BPF_RSH: A = src < 32 ? A >> src : 0; break;
            case BPF_NEG: A = (uint32_t)(-(int32_t)A); break;
            case BPF_XOR: A ^= src; break;
            }
            break;
        }
        case BPF_JMP: {
            uint32_t cond = 0;
            int taken;
            switch (BPF_OP(f->code)) {
            case BPF_JA:
                pc += k;
                continue;
            case BPF_JEQ: cond = A == ((BPF_SRC(f->code) == BPF_X) ? X : k); break;
            case BPF_JGT: cond = A >  ((BPF_SRC(f->code) == BPF_X) ? X : k); break;
            case BPF_JGE: cond = A >= ((BPF_SRC(f->code) == BPF_X) ? X : k); break;
            case BPF_JSET: cond = A & ((BPF_SRC(f->code) == BPF_X) ? X : k); break;
            }
            taken = cond ? f->jt : f->jf;
            pc += taken;
            break;
        }
        case BPF_RET:
            if (BPF_RVAL(f->code) == BPF_RVAL_A)
                return A;
            if (BPF_RVAL(f->code) == BPF_RVAL_X)
                return X;
            return k;
        case BPF_MISC:
            if (BPF_MISCOP(f->code) == BPF_TAX)
                X = A;
            else
                A = X;
            break;
        }
    }
    return SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA);
}

#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif

typedef struct {
    uint64_t call_addr;
    int32_t syscall;
    uint32_t arch;
} sigsys_fields_t;

int64_t seccomp_gate(trap_context_t *ctx, uint64_t nr, int64_t *ret_out)
{
    task_t *t = proc_current();
    if (!t || (!t->seccomp_chain && t->seccomp_mode != SECCOMP_MODE_STRICT))
        return 0;

    uint64_t args[6] = {
        TRAP_CTX_ARG0(ctx), TRAP_CTX_ARG1(ctx), TRAP_CTX_ARG2(ctx),
        TRAP_CTX_ARG3(ctx), TRAP_CTX_ARG4(ctx), TRAP_CTX_ARG5(ctx),
    };
    uint32_t act = seccomp_evaluate(t, nr, TRAP_CTX_EPC(ctx), args);
    uint32_t data = act & SECCOMP_RET_DATA;

    switch (act & SECCOMP_RET_ACTION_FULL) {
    case SECCOMP_RET_ALLOW:
        return 0;
    case SECCOMP_RET_LOG:
        kdebug("[SECCOMP] pid=%d nr=%lu logged\n", t->pid,
               (unsigned long)nr);
        return 0;
    case SECCOMP_RET_ERRNO:
        *ret_out = -(int64_t)data;
        return 1;
    case SECCOMP_RET_KILL_PROCESS:
    case SECCOMP_RET_KILL_THREAD:
        proc_exit_group(-SIGKILL);
        *ret_out = -EINTR;
        return 1;
    case SECCOMP_RET_TRAP: {
        arch_siginfo_t si;
        memset(&si, 0, sizeof(si));
        si.si_signo = SIGSYS;
        si.si_code = SYS_SECCOMP;
        si.si_errno = (int)data;
        sigsys_fields_t f = {
            .call_addr = (uint64_t)TRAP_CTX_EPC(ctx),
            .syscall = (int32_t)nr,
            .arch = SECCOMP_ARCH_TOKEN,
        };
        memcpy(si._sifields, &f, sizeof(f));
        signal_send_info(t->pid, SIGSYS, &si, sizeof(si));
        *ret_out = -ENOSYS;
        return 1;
    }
    case SECCOMP_RET_TRACE:
    case SECCOMP_RET_USER_NOTIF:
    default:
        /* No ptrace-stop notification channel is wired for TRACE and the
         * USER_NOTIF fd does not exist; Linux also degrades TRACE to
         * -ENOSYS when nobody listens. */
        *ret_out = -ENOSYS;
        return 1;
    }
}

void seccomp_inherit(task_t *child, const task_t *parent)
{
    child->seccomp_chain = NULL;
    child->seccomp_mode = SECCOMP_MODE_DISABLED;
    if (!parent || !parent->seccomp_chain)
        return;
    seccomp_filter_t *f = (seccomp_filter_t *)parent->seccomp_chain;
    __atomic_fetch_add(&f->refs, 1, __ATOMIC_ACQ_REL);
    child->seccomp_chain = f;
    child->seccomp_mode = parent->seccomp_mode;
}

void seccomp_release(task_t *t)
{
    seccomp_filter_t *f = (seccomp_filter_t *)t->seccomp_chain;
    t->seccomp_chain = NULL;
    t->seccomp_mode = SECCOMP_MODE_DISABLED;
    while (f && __atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        seccomp_filter_t *dead = f;
        f = dead->prev;
        kfree(dead->insns);
        kfree(dead);
    }
}

int seccomp_get_mode(const task_t *t)
{
    if (!t || !t->seccomp_chain)
        return -EINVAL;
    return t->seccomp_mode;
}

int seccomp_set_strict(task_t *t)
{
    if (!t)
        return -ESRCH;
    if (t->seccomp_mode == SECCOMP_MODE_FILTER && t->seccomp_chain)
        return -EACCES;
    t->seccomp_mode = SECCOMP_MODE_STRICT;
    return 0;
}

int seccomp_install_filter(task_t *t, const void *ufprog)
{
    if (!t)
        return -ESRCH;

    uint16_t usrlen = 0;
    if (copy_from_user(&usrlen, ufprog, sizeof(usrlen)) < 0)
        return -EFAULT;
    const void *uinsns = (const void *)((uintptr_t)ufprog +
                                        sizeof(uint16_t));
    if (usrlen < 1 || usrlen > BPF_MAXINSNS)
        return -EINVAL;

    seccomp_filter_t *f = kmalloc(sizeof(*f));
    if (!f)
        return -ENOMEM;
    memset(f, 0, sizeof(*f));
    f->insns = kmalloc((size_t)usrlen * sizeof(sock_filter_t));
    if (!f->insns) {
        kfree(f);
        return -ENOMEM;
    }
    if (copy_from_user(f->insns, uinsns,
                       (size_t)usrlen * sizeof(sock_filter_t)) < 0) {
        kfree(f->insns);
        kfree(f);
        return -EFAULT;
    }
    f->len = usrlen;
    f->refs = 1;

    int rc = bpf_verify(f->insns, f->len);
    if (rc < 0) {
        kfree(f->insns);
        kfree(f);
        return rc;
    }

    f->prev = (seccomp_filter_t *)t->seccomp_chain;
    t->seccomp_chain = f;
    t->seccomp_mode = SECCOMP_MODE_FILTER;
    return 0;
}

uint32_t seccomp_evaluate(const task_t *t, uint64_t nr, uint64_t ip,
                          const uint64_t args[6])
{
    seccomp_filter_t *f = t ? (seccomp_filter_t *)t->seccomp_chain : NULL;
    if (!f) {
        if (t && t->seccomp_mode == SECCOMP_MODE_STRICT) {
            switch (nr) {
            case SYS_read: case SYS_write: case SYS_exit:
            case SYS_exit_group: case SYS_sigreturn:
                return SECCOMP_RET_ALLOW;
            default:
                return SECCOMP_RET_KILL_THREAD;
            }
        }
        return SECCOMP_RET_ALLOW;
    }

    seccomp_data_wire_t sd;
    memset(&sd, 0, sizeof(sd));
    sd.nr = (uint32_t)nr;
    sd.arch = SECCOMP_ARCH_TOKEN;
    sd.instruction_pointer = ip;
    for (int i = 0; i < 6; i++)
        sd.args[i] = args[i];

    for (; f; f = f->prev) {
        uint32_t act = bpf_run(f->insns, f->len, &sd);
        if ((act & SECCOMP_RET_ACTION_FULL) != SECCOMP_RET_ALLOW)
            return act;
    }
    return SECCOMP_RET_ALLOW;
}
