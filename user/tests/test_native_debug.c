/*
 * Native ABI Debug (0x0900) smoke test.
 *
 * Two modes, selected by argv[1] == "child":
 *   debugger: spawns a child copy, attaches (debug_attach), waits for the
 *             SIGSTOP stop (debug_wait), round-trips registers and memory
 *             (debug_read_regs/write_regs, debug_read/write), resumes
 *             (debug_resume), receives the EXIT event, detaches and reaps.
 *   child:    debug_traceme() then spins on yield syscalls and exits with
 *             the distinctive code 7.
 *
 * A separate process is used instead of a thread because native threads
 * share the signal state: a process-level SIGSTOP would also stop the
 * debugger itself.
 *
 * Prints "NATIVE_DEBUG: PASS" only if every scenario holds.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "liba20rt/a20_debug.h"

#define CHILD_EXIT_CODE 7

static a20_handle_t g_out = A20_HANDLE_NULL;
static a20_handle_t g_root = A20_HANDLE_NULL;

static int fail(int code, const char *msg, uint32_t len)
{
    a20_hdl_write_buf(g_out, "NATIVE_DEBUG: FAIL ", 19, (void *)0);
    a20_hdl_write_buf(g_out, msg, len, (void *)0);
    a20_hdl_write_buf(g_out, "\n", 1, (void *)0);
    return code;
}

/* ---- child mode ---- */

static int child_main(void)
{
    /* Long sleep keeps the child alive and parked for the debugger's
     * attach window; the attach SIGSTOP interrupts the sleep.  Note: the
     * child does not call debug_traceme — TRACEME and ATTACH are mutually
     * exclusive (like Linux), and the Native ABI has no self-signal trigger
     * to exercise the TRACEME stop end to end. */
    a20_time_t delay = { .secs = 2, .nsecs = 0 };
    a20_thread_sleep(delay);
    return CHILD_EXIT_CODE;
}

/* ---- debugger mode ---- */

static a20_status_t spawn_child(a20_handle_t *out_task)
{
    static const char path[] = "/bin/native-debug-rv";
    a20_path_open_args_t oa;
    oa.size = sizeof(oa);
    oa.version = 1;
    oa.dir = A20_HANDLE_NULL;
    oa.flags = 0;
    oa.rights = A20_RIGHT_READ | A20_RIGHT_EXEC;
    oa.path = (uint64_t)(uintptr_t)path;
    oa.path_len = sizeof(path) - 1;
    oa.mode = 0;
    oa.out_handle = A20_HANDLE_NULL;
    a20_status_t st = a20_path_open(&oa);
    if (st != A20_OK)
        return st;

    char *argv[] = { (char *)path, "child", NULL };
    a20_task_spawn_args_t ta;
    ta.size = sizeof(ta);
    ta.version = 2;
    ta.image = oa.out_handle;
    ta.root_dir = g_root;
    ta.cwd_dir = A20_HANDLE_NULL;
    ta.event_queue = A20_HANDLE_NULL;
    ta.argv = (uint64_t)(uintptr_t)argv;
    ta.envp = 0;
    ta.argc = 2;
    ta.envc = 0;
    ta.handles = 0;
    ta.handle_count = 0;
    ta.flags = 0;
    ta.out_task = A20_HANDLE_NULL;
    ta.stdin_handle = A20_HANDLE_NULL;
    ta.stdout_handle = g_out;
    ta.stderr_handle = g_out;
    ta.reserved = 0;

    st = a20_syscall6(A20_SYS_task_spawn, (uint64_t)(uintptr_t)&ta, 0, 0, 0, 0, 0);
    a20_hdl_close(oa.out_handle);
    if (st < 0)
        return st;
    *out_task = ta.out_task;
    return A20_OK;
}

static int debugger_main(void)
{
    a20_handle_t task_h = A20_HANDLE_NULL;
    a20_status_t st = spawn_child(&task_h);
    if (st != A20_OK)
        return fail(1, "spawn failed", 12);

    a20_handle_t dbg = A20_HANDLE_NULL;
    st = a20_debug_attach(task_h, &dbg);
    if (st != A20_OK)
        return fail(2, "debug_attach failed", 20);

    a20_debug_event_info_t ev;
    st = a20_debug_wait(dbg, A20_TIMEOUT_INFINITE, &ev);
    if (st != A20_OK)
        return fail(3, "debug_wait failed", 18);
    if (ev.kind != A20_DEBUG_STOP_SIGNAL || ev.sig != 19 /* SIGSTOP */)
        return fail(4, "unexpected stop kind/sig", 24);

    st = a20_debug_event(dbg, &ev);
    if (st != A20_OK || ev.kind != A20_DEBUG_STOP_SIGNAL)
        return fail(5, "debug_event mismatch", 21);

    /* Register round trip: flip the PC and restore it. */
    a20_regs_t regs;
    st = a20_debug_read_regs(dbg, &regs);
    if (st != A20_OK || regs.pc == 0)
        return fail(6, "debug_read_regs failed", 23);
    uint64_t saved_pc = regs.pc;
    regs.pc ^= 0x4;
    st = a20_debug_write_regs(dbg, &regs);
    if (st != A20_OK)
        return fail(7, "debug_write_regs failed", 24);
    st = a20_debug_read_regs(dbg, &regs);
    if (st != A20_OK || regs.pc != (saved_pc ^ 0x4))
        return fail(8, "regs roundtrip mismatch", 24);
    regs.pc = saved_pc;
    st = a20_debug_write_regs(dbg, &regs);
    if (st != A20_OK)
        return fail(9, "regs restore failed", 20);

    /* Memory round trip at the stop PC (same static binary layout in both
     * processes): read an instruction word, flip it, restore it. */
    uint32_t word = 0;
    st = a20_debug_read(dbg, saved_pc & ~3ULL, &word, sizeof(word));
    if (st != A20_OK)
        return fail(10, "debug_read failed", 18);
    uint32_t flip = ~word;
    st = a20_debug_write(dbg, saved_pc & ~3ULL, &flip, sizeof(flip));
    if (st != A20_OK)
        return fail(11, "debug_write failed", 19);
    st = a20_debug_read(dbg, saved_pc & ~3ULL, &word, sizeof(word));
    if (st != A20_OK || word != flip)
        return fail(12, "debug_write roundtrip", 22);
    flip = ~flip;
    st = a20_debug_write(dbg, saved_pc & ~3ULL, &flip, sizeof(flip));
    if (st != A20_OK)
        return fail(13, "debug_write restore failed", 26);

    st = a20_debug_resume(dbg, A20_DEBUG_RESUME_CONT);
    if (st != A20_OK)
        return fail(14, "debug_resume failed", 20);

    st = a20_debug_wait(dbg, 10ULL * 1000 * 1000, &ev);
    if (st != A20_OK)
        return fail(15, "no EXIT event", 14);
    if (ev.kind != A20_DEBUG_STOP_EVENT || ev.event != A20_DEBUG_EVENT_EXIT)
        return fail(16, "unexpected EXIT event", 22);

    st = a20_debug_detach(dbg);
    if (st != A20_OK)
        return fail(17, "debug_detach failed", 20);

    a20_task_status_t ts;
    st = a20_task_wait(task_h, 0, &ts);
    if (st != A20_OK)
        return fail(18, "task_wait failed", 17);
    if (ts.exit_code != CHILD_EXIT_CODE)
        return fail(19, "unexpected exit code", 21);

    a20_hdl_write_buf(g_out, "NATIVE_DEBUG: PASS\n", 19, (void *)0);
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    g_root = si ? si->root_dir : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 90;

    if (argc > 1 && argv[1] && argv[1][0] == 'c')
        return child_main();
    return debugger_main();
}
