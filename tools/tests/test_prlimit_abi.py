"""Production prlimit adapter + Rust policy: aliases, access and task references."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from tools.tests.test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "kernel/abi/linux/rlimit_policy.h"
#define CAP_SYS_RESOURCE 24
typedef struct task_t {
    struct { uint64_t stack, nofile, as, nproc, rtprio, rtprio_max; } limits;
    struct { int uid; } cred;
    uint64_t core;
    int refs;
} task_t;
static task_t self, other;
static int capability, fromfail, tofail, locks, gets, puts;
static task_t *proc_current(void) { return &self; }
static task_t *proc_find_get_user(int pid) {
    task_t *t = pid == 10 ? &self : pid == 20 ? &other : NULL;
    if (t) { t->refs++; gets++; }
    return t;
}
static void proc_put(task_t *t) { assert(t->refs > 0); t->refs--; puts++; }
static int proc_has_cap(task_t *t, int cap) {
    assert(t == &self && cap == CAP_SYS_RESOURCE); return capability;
}
static int copy_from_user(void *dst, const void *src, size_t size) {
    if (fromfail) return -1;
    memcpy(dst, src, size); return 0;
}
static int copy_to_user(void *dst, const void *src, size_t size) {
    if (tofail) return -1;
    memcpy(dst, src, size); return 0;
}
static uint64_t signal_task_rlim_core(task_t *t) { return t ? t->core : 0; }
uint64_t a20_rl_park_lock(task_t *t) { (void)t; assert(!locks); locks++; return 0; }
void a20_rl_park_unlock(task_t *t, uint64_t f) { (void)t; (void)f; assert(locks == 1); locks--; }
int a20_rl_has_cap(task_t *t, int cap) { return proc_has_cap(t, cap); }
void a20_rl_core_set(task_t *t, uint64_t limit) { t->core = limit; }
'''
HARNESS = r'''
int main(void) {
    /* Linux accepts overlapping new/old pairs and applies the input before
     * returning the old pair. Lower only this test process's soft limit. */
    uint64_t linux_old[2], linux_pair[2], linux_now[2];
    assert(syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, NULL, linux_old) == 0);
    assert(linux_old[0] > 32);
    linux_pair[0] = 32; linux_pair[1] = linux_old[1];
    assert(syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, linux_pair, linux_pair) == 0);
    assert(!memcmp(linux_pair, linux_old, sizeof(linux_old)));
    assert(syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, NULL, linux_now) == 0 && linux_now[0] == 32);
    assert(syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, linux_old, NULL) == 0);
    errno = 0;
    assert(syscall(SYS_prlimit64, 0x3fffffff, RLIMIT_NOFILE, NULL, NULL) == -1 && errno == ESRCH);

    self.cred.uid = 1000; other.cred.uid = 2000;
    self.limits.nofile = 128; other.limits.nofile = 64;
    uint64_t pair[2] = {32, 64};
    assert(sys_prlimit64(10, RLIMIT_NOFILE, pair, pair) == 0);
    assert(self.limits.nofile == 32 && pair[0] == 128 && pair[1] == 128);
    assert(!self.refs && gets == puts);
    pair[0] = 3; pair[1] = 2;
    assert(sys_prlimit64(10, RLIMIT_NOFILE, pair, pair) == -EINVAL);
    assert(self.limits.nofile == 32 && pair[0] == 3 && pair[1] == 2);
    assert(!self.refs && gets == puts);

    pair[0] = pair[1] = 0x12345678;
    assert(sys_prlimit64(20, RLIMIT_NOFILE, NULL, pair) == -EPERM);
    assert(pair[0] == 0x12345678 && pair[1] == 0x12345678 && !other.refs);
    capability = 1;
    assert(sys_prlimit64(20, RLIMIT_NOFILE, NULL, pair) == 0 && pair[0] == 64);
    capability = 0; other.cred.uid = self.cred.uid;
    assert(sys_prlimit64(20, RLIMIT_NOFILE, NULL, pair) == 0 && pair[0] == 64);
    assert(sys_prlimit64(999, RLIMIT_NOFILE, NULL, NULL) == -ESRCH);
    assert(sys_prlimit64(10, RLIMIT_NOFILE, NULL, NULL) == 0);
    assert(!self.refs && !other.refs && gets == puts);

    fromfail = 1; pair[0] = pair[1] = 20;
    assert(sys_prlimit64(10, RLIMIT_NOFILE, pair, NULL) == -EFAULT);
    assert(self.limits.nofile == 32);
    fromfail = 0; tofail = 1;
    /* Linux performs the set before copying the previous value to userland. */
    assert(sys_prlimit64(10, RLIMIT_NOFILE, pair, pair) == -EFAULT);
    assert(self.limits.nofile == 20 && !self.refs && gets == puts && !locks);
    return 0;
}
'''


class PrlimitABI(unittest.TestCase):
    def test_alias_permissions_and_reference_release(self):
        source = (ROOT / "kernel/abi/linux/sys_proc.c").read_text()
        signatures = ["int a20_rl_view_fill(", "int a20_rl_current_uid(",
                      "task_t *a20_rl_current(", "static int rlimit_get(",
                      "static int rlimit_set(", "int64_t sys_prlimit64("]
        functions = "\n".join(extract_function(source, signature)
                              for signature in signatures)
        with tempfile.TemporaryDirectory(prefix="a20-prlimit-") as directory:
            work = Path(directory)
            rust = work / "host.rs"
            rust.write_text(f'#[path="{ROOT / "kernel/abi/linux/rlimit_policy.rs"}"] mod policy;\n')
            library = work / "libpolicy.a"
            subprocess.run(["rustc", "--edition=2021", "--crate-type=staticlib",
                            "-C", "opt-level=1", str(rust), "-o", str(library)],
                           check=True, capture_output=True)
            test = work / "test.c"
            test.write_text(PRELUDE + functions + HARNESS)
            binary = work / "test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT), "-idirafter", str(ROOT / "kernel/include"),
                            str(test), str(library), "-lpthread", "-ldl", "-lm",
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
