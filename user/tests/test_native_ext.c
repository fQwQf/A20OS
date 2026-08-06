/*
 * Native ABI kernel extension points (0x0D00) smoke test.
 *
 * Loads a small verified KEP program (standard eBPF instruction encoding)
 * that denies one specific syscall (debug_kill, which the test itself never
 * calls), attaches it to the syscall-filter extension point, verifies the
 * denial, then detaches and verifies the syscall works again:
 *   1. ext_point_info reports the syscall-filter point;
 *   2. a malformed program (out-of-range jump) is rejected at load;
 *   3. the valid program denies A20_SYS_debug_kill with ACCESS;
 *   4. detach restores the syscall.
 *
 * Prints "NATIVE_EXT: PASS" only if every scenario holds.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "liba20rt/a20_ext.h"
#include "liba20rt/a20_debug.h"

static a20_handle_t g_out = A20_HANDLE_NULL;

static int fail(int code, const char *msg, uint32_t len)
{
    a20_hdl_write_buf(g_out, "NATIVE_EXT: FAIL ", 17, (void *)0);
    a20_hdl_write_buf(g_out, msg, len, (void *)0);
    a20_hdl_write_buf(g_out, "\n", 1, (void *)0);
    return code;
}

/*
 * Program (eBPF encoding): deny syscall `deny_nr`, allow everything else.
 * R1 is the context pointer (set by the kernel).
 *   LDX r0 = *(u64 *)(r1 + 0)        ; r0 = syscall number
 *   JEQ r0, deny_nr, +2              ; equal -> pc+2 (deny path)
 *   MOV r0 = ALLOW
 *   EXIT
 *   MOV r0 = DENY
 *   EXIT
 */
static a20_bpf_insn_t deny_prog[6];

static void build_deny_prog(uint32_t deny_nr)
{
    deny_prog[0] = A20_BPF_LDX_DW(0, 1, A20_KEP_SCF_NR * 8);
    deny_prog[1] = A20_BPF_JMP_IMM(A20_BPF_JEQ, 0, 2, (int32_t)deny_nr);
    deny_prog[2] = A20_BPF_MOV64_IMM(0, A20_KEP_SCF_ALLOW);
    deny_prog[3] = A20_BPF_EXIT();
    deny_prog[4] = A20_BPF_MOV64_IMM(0, A20_KEP_SCF_DENY);
    deny_prog[5] = A20_BPF_EXIT();
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 90;

    /* ---- 1. point info ---- */
    a20_ext_point_info_t info;
    a20_status_t st = a20_ext_point_info(1 /* syscall-filter */, &info);
    if (st != A20_OK)
        return fail(1, "ext_point_info failed", 22);
    if (info.id != 1 || info.nwords < A20_KEP_SCF_WORDS)
        return fail(2, "unexpected point info", 22);

    /* ---- 2. malformed program rejected (jump out of range) ---- */
    a20_bpf_insn_t bad[4] = {
        A20_BPF_LDX_DW(0, 1, A20_KEP_SCF_NR * 8),
        A20_BPF_JMP_IMM(A20_BPF_JEQ, 0, 100, 0x090B), /* target >= ninsns */
        A20_BPF_MOV64_IMM(0, 0),
        A20_BPF_EXIT(),
    };
    a20_handle_t bad_h = A20_HANDLE_NULL;
    st = a20_ext_prog_load(bad, 4, &bad_h);
    if (st == A20_OK)
        return fail(3, "malformed program accepted", 27);

    /* ---- 3. load + attach the valid program ---- */
    build_deny_prog(A20_SYS_debug_kill);
    a20_handle_t prog = A20_HANDLE_NULL;
    st = a20_ext_prog_load(deny_prog, 6, &prog);
    if (st != A20_OK)
        return fail(4, "ext_prog_load failed", 21);

    st = a20_ext_prog_attach(prog, 1);
    if (st != A20_OK)
        return fail(5, "ext_prog_attach failed", 23);

    /* ---- 4. denied syscall returns ACCESS ---- */
    st = a20_debug_kill(A20_HANDLE_NULL);
    if (st != -A20_ERR_ACCESS)
        return fail(6, "denied syscall not blocked", 27);

    /* ---- 5. detach restores the syscall ---- */
    st = a20_ext_prog_detach(prog, 1);
    if (st != A20_OK)
        return fail(7, "ext_prog_detach failed", 23);
    st = a20_debug_kill(A20_HANDLE_NULL);
    if (st == -A20_ERR_ACCESS)
        return fail(8, "syscall still blocked after detach", 35);

    /* ---- 6. release ---- */
    st = a20_ext_prog_release(prog);
    if (st != A20_OK)
        return fail(9, "ext_prog_release failed", 24);

    a20_hdl_write_buf(g_out, "NATIVE_EXT: PASS\n", 17, (void *)0);
    return 0;
}
