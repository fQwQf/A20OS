import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
PAGE = 4096


HOST = r"""
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#define PAGE 4096u
static unsigned char arena[2 * PAGE];
static int mapped[2] = {1, 1};
static int cow_once;
static int cow_fail;
static int switch_mm_during_cow;
static int switch_mm_after_dirty;
static void *current_task_mm;
static unsigned mm_gets, mm_puts, frame_gets, frame_puts, cow_calls, dirty_calls;
static void *cow_mm_seen;
static int test_mm;
static int other_mm;
static int test_task;
static unsigned target_gets, target_puts;
static unsigned char scratch[65536];
void *a20_process_vm_mm_get(void *task) {
    if (!task) return NULL;
    mm_gets++; return current_task_mm;
}
void a20_process_vm_mm_put(void *mm) {
    if (mm == &test_mm || mm == &other_mm) mm_puts++;
}
int a20_process_vm_range_ok(uintptr_t address, size_t length) {
    uintptr_t base = (uintptr_t)arena;
    if (!length) return 1;
    return address >= base && address - base < sizeof(arena) &&
           length <= sizeof(arena) - (address - base);
}
int a20_process_vm_pin_leaf(void *mm, uintptr_t address, int write,
                            void **kernel, size_t *available, uintptr_t *token,
                            int *write_guard) {
    if (mm != &test_mm || !a20_process_vm_range_ok(address, 1)) return -EFAULT;
    uintptr_t off = address - (uintptr_t)arena;
    unsigned page = off / PAGE;
    if (!mapped[page]) return -EFAULT;
    if (write && page == 0 && cow_once) {
        if (switch_mm_during_cow) current_task_mm = &other_mm;
        return -EAGAIN;
    }
    *kernel = arena + off;
    *available = PAGE - (off % PAGE);
    *token = page + 1;
    *write_guard = 0;
    frame_gets++;
    return 0;
}
void mm_shared_write_leave(void) {}
void a20_process_vm_frame_put(uintptr_t token) { if (token) frame_puts++; }
int a20_process_vm_break_cow(void *task, void *mm, uintptr_t address) {
    (void)task; (void)address; cow_calls++; cow_mm_seen = mm; cow_once = 0;
    return cow_fail ? -EFAULT : 0;
}
int a20_process_vm_test_wait_exec_switch(void *task, void *mm) {
    (void)task; (void)mm; return 0;
}
void a20_process_vm_mark_dirty(void *mm, uintptr_t address) {
    (void)mm; (void)address; dirty_calls++;
    if (switch_mm_after_dirty) {
        current_task_mm = &other_mm;
        switch_mm_after_dirty = 0;
    }
}
int a20_process_vm_target_get(int pid, void **out) {
    (void)pid; target_gets++; *out = &test_task; return 0;
}
void a20_process_vm_target_put(void *task) {
    if (task == &test_task) target_puts++;
}
long a20_process_vm_copy_from_user(void *dst, const void *src, size_t len) {
    if (!dst || !src) return -EFAULT;
    memcpy(dst, src, len);
    return (long)len;
}
long a20_process_vm_copy_to_user(void *dst, const void *src, size_t len) {
    if (!dst || !src) return -EFAULT;
    memcpy(dst, src, len);
    return (long)len;
}
void *a20_process_vm_scratch_buffer(size_t size) {
    return size <= sizeof(scratch) ? scratch : NULL;
}
static int reset_state(void) {
    memset(arena, 0x2a, sizeof(arena)); mapped[0] = mapped[1] = 1;
    cow_once = cow_fail = switch_mm_during_cow = switch_mm_after_dirty = 0;
    current_task_mm = &test_mm;
    cow_mm_seen = NULL; mm_gets = mm_puts = frame_gets = frame_puts = 0;
    cow_calls = dirty_calls = target_gets = target_puts = 0; return 0;
}
int run_tests(void);
"""


MAIN = r"""
long a20_process_vm_read_kernel_rust(void *, const void *, void *, size_t);
long a20_process_vm_write_kernel_rust(void *, void *, const void *, size_t);
long a20_sys_process_vm_readv_rust(int, const void *, size_t, const void *, size_t, size_t);
long a20_sys_process_vm_writev_rust(int, const void *, size_t, const void *, size_t, size_t);
static int fail(const char *name) { fprintf(stderr, "FAIL %s\n", name); return 1; }
int main(void) {
    int task;
    unsigned char local[PAGE + 8];
    int bad = 0;
    reset_state();
    if (a20_process_vm_read_kernel_rust(&task, arena + PAGE - 3, local, 7) != 7 ||
        memcmp(local, arena + PAGE - 3, 7) || mm_gets != 1 || mm_puts != 1 ||
        frame_gets != 2 || frame_puts != 2) bad |= fail("read-cross-page-refs");
    reset_state(); mapped[1] = 0;
    if (a20_process_vm_read_kernel_rust(&task, arena, local, PAGE + 1) != PAGE ||
        mm_gets != mm_puts || frame_gets != frame_puts || frame_puts != 1)
        bad |= fail("partial-read-and-unwind");
    reset_state(); cow_once = 1;
    memset(local, 0x7b, sizeof(local));
    if (a20_process_vm_write_kernel_rust(&task, arena, local, sizeof(local)) != sizeof(local) ||
        memcmp(arena, local, sizeof(local)) || cow_calls != 1 || dirty_calls != 2 ||
        mm_gets != mm_puts || frame_gets != frame_puts)
        bad |= fail("write-cow-dirty-and-unwind");
    reset_state(); cow_once = switch_mm_during_cow = 1;
    memset(local, 0x45, sizeof(local));
    if (a20_process_vm_write_kernel_rust(&task, arena, local, sizeof(local)) != sizeof(local) ||
        cow_mm_seen != &test_mm || current_task_mm != &other_mm ||
        mm_gets != 1 || mm_puts != 1 || frame_gets != frame_puts)
        bad |= fail("exec-mm-swap-keeps-cow-on-pinned-mm");
    reset_state(); cow_once = cow_fail = 1;
    if (a20_process_vm_write_kernel_rust(&task, arena, local, sizeof(local)) != -EFAULT ||
        cow_calls != 1 || mm_gets != mm_puts || frame_gets != frame_puts ||
        frame_gets != 0)
        bad |= fail("cow-failure-releases-mm-and-frame-refs");
    reset_state();
    if (a20_process_vm_read_kernel_rust(&task, arena, local, 0) != 0 ||
        mm_gets != 1 || mm_puts != 1 || frame_gets || frame_puts)
        bad |= fail("zero-length-mm-lifecycle");
    reset_state(); mapped[1] = 0;
    if (a20_process_vm_write_kernel_rust(&task, arena + PAGE - 2, local, 8) != 2 ||
        mm_gets != mm_puts || frame_gets != frame_puts || frame_puts != 1)
        bad |= fail("partial-write-result");
    reset_state();
    struct iovec { void *base; size_t length; } remote = { arena + PAGE - 4, 8 };
    unsigned char out[8] = {0};
    struct iovec local_iov = { out, sizeof(out) };
    struct iovec remote_iov = { remote.base, remote.length };
    if (a20_sys_process_vm_readv_rust(1, &local_iov, 1, &remote_iov, 1, 0) != 8 ||
        memcmp(out, remote.base, 8) || target_gets != 1 || target_puts != 1 ||
        mm_gets != mm_puts || frame_gets != frame_puts)
        bad |= fail("readv-iovec-and-target-lifecycle");
    reset_state();
    unsigned char in2[8]; memset(in2, 0x6e, sizeof(in2));
    struct iovec local_multi[2] = {{in2, 4}, {in2 + 4, 4}};
    struct iovec remote_multi[2] = {{arena, 4}, {arena + 8, 4}};
    switch_mm_after_dirty = 1;
    if (a20_sys_process_vm_writev_rust(1, local_multi, 2, remote_multi, 2, 0) != 8 ||
        memcmp(arena, in2, 4) || memcmp(arena + 8, in2 + 4, 4) ||
        current_task_mm != &other_mm || target_gets != target_puts ||
        mm_gets != 1 || mm_puts != 1 || frame_gets != frame_puts)
        bad |= fail("readv-retains-one-mm-across-iovecs-and-exec");
    reset_state(); mapped[1] = 0;
    remote_iov.base = arena + PAGE - 4; remote_iov.length = 8;
    local_iov.length = 8;
    if (a20_sys_process_vm_readv_rust(1, &local_iov, 1, &remote_iov, 1, 0) != 4 ||
        target_gets != target_puts || mm_gets != mm_puts || frame_gets != frame_puts)
        bad |= fail("readv-partial-iovec-result");
    reset_state();
    unsigned char input[8]; memset(input, 0x6c, sizeof(input));
    local_iov.base = input; local_iov.length = sizeof(input);
    remote_iov.base = arena + PAGE - 4; remote_iov.length = sizeof(input);
    if (a20_sys_process_vm_writev_rust(1, &local_iov, 1, &remote_iov, 1, 0) != 8 ||
        memcmp(remote_iov.base, input, sizeof(input)) || target_gets != target_puts ||
        mm_gets != mm_puts || frame_gets != frame_puts)
        bad |= fail("writev-iovec-and-target-lifecycle");
    if (bad) return 1;
    puts("process-vm-host: production Rust iteration, partial counts, and refs PASS");
    return 0;
}
"""


class ProcessVmHostTests(unittest.TestCase):
    def test_production_module_partial_results_and_references(self):
        rustc, gcc = shutil.which("rustc"), shutil.which("gcc")
        if not rustc or not gcc:
            self.skipTest("rustc and gcc are required for the host oracle")
        with tempfile.TemporaryDirectory(prefix="a20-process-vm-") as directory:
            tmp = pathlib.Path(directory)
            rust_obj = tmp / "process_vm.o"
            harness = tmp / "host.c"
            exe = tmp / "host"
            subprocess.run(
                [rustc, "--edition=2021", "--crate-type", "lib", "--emit=obj",
                 "-C", "panic=abort", "-C", "opt-level=2",
                 str(ROOT / "kernel/mm/process_vm.rs"), "-o", str(rust_obj)],
                check=True, cwd=ROOT,
            )
            harness.write_text(HOST + MAIN)
            subprocess.run(
                [gcc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                 str(harness), str(rust_obj), "-o", str(exe)],
                check=True, cwd=ROOT,
            )
            result = subprocess.run([str(exe)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("production Rust iteration", result.stdout)


if __name__ == "__main__":
    unittest.main()
