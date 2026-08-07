/*
 * Linux ABI bpf(2) smoke test: bpf(2) is a thin wrapper over the KEP
 * extension engine (kernel/ext/kep.c).
 *
 * Loads a verified eBPF program that denies the getppid syscall, attaches
 * it to the syscall-filter extension point (A20 adaptation: BPF_PROG_ATTACH
 * carries the extension-point id in target_fd), verifies getppid fails
 * with EACCES, detaches and verifies it works again.
 *
 * Prints "BPF_SMOKE: PASS" only if every scenario holds.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define BPF_PROG_LOAD   5
#define BPF_PROG_ATTACH 8
#define BPF_PROG_DETACH 9

#define BPF_LDX  0x01
#define BPF_ALU64 0x07
#define BPF_JMP  0x05
#define BPF_DW   0x18
#define BPF_MEM  0x60
#define BPF_K    0x00
#define BPF_MOV  0xb0
#define BPF_JEQ  0x10
#define BPF_EXIT 0x90

#define KEP_SCF_NR 0

struct bpf_insn {
    uint8_t  code;
    uint8_t  dst_reg;
    uint8_t  src_reg;
    int16_t  off;
    int32_t  imm;
};

/* BPF_PROG_LOAD attribute (Linux bpf_attr layout, first fields). */
struct bpf_attr_load {
    uint32_t prog_type;
    uint32_t insn_cnt;
    uint64_t insns;
    uint64_t license;
    uint32_t log_level;
    uint32_t log_size;
    uint64_t log_buf;
};

/* BPF_PROG_ATTACH/DETACH attribute. */
struct bpf_attr_attach {
    int32_t  target_fd;      /* KEP extension-point id (A20 adaptation) */
    int32_t  attach_bpf_fd;
    uint32_t attach_type;
    uint32_t flags;
};

static int fail(const char *what)
{
    printf("BPF_SMOKE: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int sys_bpf(int cmd, void *attr, unsigned size)
{
    return (int)syscall(SYS_bpf, cmd, attr, size);
}

int main(void)
{
    printf("BPF_SMOKE: start\n");

    /* getppid must work before the filter is attached. */
    if (getppid() < 0)
        return fail("getppid-baseline");

    /*
     * eBPF program: deny syscall 173 (getppid), allow everything else.
     *   LDX r0 = *(u64 *)(r1 + 0)     ; R1 = context, words[0] = syscall nr
     *   JEQ r0, 173, +2
     *   MOV r0, 0                     ; ALLOW
     *   EXIT
     *   MOV r0, 1                     ; DENY
     *   EXIT
     */
    struct bpf_insn insns[6] = {
        { BPF_LDX | BPF_DW | BPF_MEM, 0, 1, KEP_SCF_NR * 8, 0 },
        { BPF_JMP | BPF_JEQ | BPF_K, 0, 0, 2, 173 },
        { BPF_ALU64 | BPF_MOV | BPF_K, 0, 0, 0, 0 },
        { BPF_JMP | BPF_EXIT, 0, 0, 0, 0 },
        { BPF_ALU64 | BPF_MOV | BPF_K, 0, 0, 0, 1 },
        { BPF_JMP | BPF_EXIT, 0, 0, 0, 0 },
    };

    struct bpf_attr_load load = {
        .prog_type = 1, /* socket-filter type id is irrelevant to KEP */
        .insn_cnt = 6,
        .insns = (uint64_t)(uintptr_t)insns,
        .license = 0,
        .log_level = 0,
        .log_size = 0,
        .log_buf = 0,
    };
    int prog_fd = sys_bpf(BPF_PROG_LOAD, &load, sizeof(load));
    if (prog_fd < 0)
        return fail("prog-load");

    struct bpf_attr_attach attach = {
        .target_fd = 1, /* KEP_POINT_SYSCALL_FILTER */
        .attach_bpf_fd = prog_fd,
        .attach_type = 0,
        .flags = 0,
    };
    if (sys_bpf(BPF_PROG_ATTACH, &attach, sizeof(attach)) < 0)
        return fail("prog-attach");

    /* The denied syscall must now fail with EACCES. */
    long r = syscall(SYS_getppid);
    if (r != -1 || errno != EACCES) {
        printf("BPF_SMOKE: FAIL denied syscall not blocked (r=%ld errno=%d)\n",
               r, errno);
        return 1;
    }

    if (sys_bpf(BPF_PROG_DETACH, &attach, sizeof(attach)) < 0)
        return fail("prog-detach");

    if (getppid() < 0)
        return fail("getppid-after-detach");

    printf("BPF_SMOKE: PASS\n");
    return 0;
}
