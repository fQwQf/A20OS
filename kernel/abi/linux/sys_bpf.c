#include "syscall_impl.h"

#include "ext/kep.h"
#include "fs/memfd.h"

/*
 * LINUX_ABI_BPF_STUB_BOUNDARY: the Linux bpf(2) syscall is a thin wrapper
 * over the ABI-agnostic KEP extension engine (kernel/ext/kep.c).  Only
 * program operations are supported (BPF_PROG_LOAD / BPF_PROG_ATTACH /
 * BPF_PROG_DETACH); the map commands of the former placeholder bpf
 * implementation are intentionally not provided.
 *
 * A20-specific adaptation: BPF_PROG_ATTACH's target_fd argument carries
 * the KEP extension-point id (Linux attaches to sockets instead).  The
 * program fd is a memfd; the KEP registry keeps the (owner, fd) mapping
 * and sweeps fds closed by the owner on each call.
 */

#define BPF_PROG_LOAD        5
#define BPF_PROG_ATTACH      8
#define BPF_PROG_DETACH      9

typedef struct {
    uint32_t prog_type;
    uint32_t insn_cnt;
    uint64_t insns;
    uint64_t license;
    uint32_t log_level;
    uint32_t log_size;
    uint64_t log_buf;
} bpf_attr_prog_load_t;

typedef struct {
    int32_t  target_fd;      /* KEP extension-point id (A20 adaptation) */
    int32_t  attach_bpf_fd;  /* program fd from BPF_PROG_LOAD */
    uint32_t attach_type;    /* ignored, must be 0 */
    uint32_t flags;
} bpf_attr_attach_t;

static int bpf_copy_attr(void *dst, size_t dst_size, void *uattr, unsigned size)
{
    memset(dst, 0, dst_size);
    size_t n = size < dst_size ? size : dst_size;
    if (n && copy_from_user(dst, uattr, n) < 0)
        return -EFAULT;
    return 0;
}

static int64_t sys_bpf_prog_load(void *uattr, unsigned size)
{
    bpf_attr_prog_load_t attr;
    int r = bpf_copy_attr(&attr, sizeof(attr), uattr, size);
    if (r < 0) return r;
    if (attr.insn_cnt == 0 || attr.insn_cnt > KEP_MAX_PROG_INSNS ||
        !attr.insns)
        return -EINVAL;

    kep_sweep_fds();

    int id = kep_prog_load((const bpf_insn_t *)(uintptr_t)attr.insns,
                           attr.insn_cnt);
    if (id < 0)
        return id;

    /* The program is exposed as a process-local fd (memfd); the KEP
     * registry keeps the (owner, fd) alias and sweeps closed fds on the
     * next call. */
    int fd = memfd_create_file(0);
    if (fd < 0) {
        (void)kep_prog_release(id);
        return fd;
    }
    (void)kep_prog_set_fd(id, fd);
    return fd;
}

static int64_t sys_bpf_prog_attach(void *uattr, unsigned size, int detach)
{
    bpf_attr_attach_t attr;
    int r = bpf_copy_attr(&attr, sizeof(attr), uattr, size);
    if (r < 0) return r;
    if (attr.attach_type != 0 || attr.flags != 0)
        return -EINVAL;
    if (attr.attach_bpf_fd < 0 || attr.target_fd <= 0)
        return -EINVAL;

    kep_sweep_fds();
    int id = kep_prog_find_by_fd(attr.attach_bpf_fd);
    if (id < 0)
        return -EBADF;

    if (detach)
        return kep_prog_detach(id, (uint32_t)attr.target_fd);
    return kep_prog_attach(id, (uint32_t)attr.target_fd);
}

int64_t sys_bpf(int cmd, void *attr, unsigned size)
{
    if (size && !attr) return -EFAULT;
    if (size) {
        uint8_t probe;
        if (copy_from_user(&probe, attr, 1) < 0) return -EFAULT;
    }
    switch (cmd) {
    case BPF_PROG_LOAD:
        return sys_bpf_prog_load(attr, size);
    case BPF_PROG_ATTACH:
        return sys_bpf_prog_attach(attr, size, 0);
    case BPF_PROG_DETACH:
        return sys_bpf_prog_attach(attr, size, 1);
    default:
        return -EOPNOTSUPP;
    }
}
