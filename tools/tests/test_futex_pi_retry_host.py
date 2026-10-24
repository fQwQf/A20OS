"""Deterministic host regression for PI futex retry behavior.

The tested futex_pi_acquire body is extracted verbatim from kernel/ipc/futex.c;
only its kernel interactions are replaced with small stateful mocks.
"""
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/ipc/futex.c"


def extract_c_function(source: str, signature: str) -> str:
    """Extract exactly one C function while ignoring braces in literals/comments."""
    if source.count(signature) != 1:
        raise AssertionError(f"expected exactly one occurrence of {signature!r}")
    start = source.index(signature)
    brace = source.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing body for {signature!r}")
    depth = 0
    i = brace
    state = "code"
    while i < len(source):
        c = source[i]
        n = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if c == "/" and n == "*":
                state = "block_comment"
                i += 1
            elif c == "/" and n == "/":
                state = "line_comment"
                i += 1
            elif c == '"':
                state = "string"
            elif c == "'":
                state = "char"
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return source[start:i + 1]
        elif state == "block_comment":
            if c == "*" and n == "/":
                state = "code"
                i += 1
        elif state == "line_comment":
            if c == "\n":
                state = "code"
        elif state == "string":
            if c == "\\":
                i += 1
            elif c == '"':
                state = "code"
        elif state == "char":
            if c == "\\":
                i += 1
            elif c == "'":
                state = "code"
        i += 1
    raise AssertionError(f"unterminated body for {signature!r}")


HOST_PRELUDE = r"""
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define ERESTARTSYS 512
#define FUTEX_TID_MASK 0x3fffffff
#define FUTEX_WAITERS 0x80000000u
#define FUTEX_OWNER_DIED 0x40000000u
#define FUTEX_BITSET_MATCH_ANY 0xffffffffu
#define FUTEX_BUCKETS 64

typedef struct { int unused; } test_lock_t;
typedef struct { test_lock_t lock; } mm_struct_t;
typedef struct task { int pid; mm_struct_t *mm; } task_t;
typedef struct { test_lock_t lock; } wait_queue_t;
static task_t current_task, owner_task;
static mm_struct_t test_mm;
static wait_queue_t g_futex_buckets[FUTEX_BUCKETS];
static volatile int futex_word;
static unsigned lock_depth, lock_calls, unlock_calls;
static unsigned writable_calls, wait_calls, boost_calls, put_calls;
static int try_wait_result, wait_mutation, test_error;

static void fail(const char *name) {
    fprintf(stderr, "FAIL: %s\n", name);
    test_error = 1;
}
static uint64_t spin_lock_irqsave(test_lock_t *lock) {
    (void)lock;
    lock_depth++;
    lock_calls++;
    return lock_depth;
}
static void spin_unlock_irqrestore(test_lock_t *lock, uint64_t flags) {
    (void)lock;
    if (!lock_depth || flags != lock_depth) fail("lock flag/depth mismatch");
    lock_depth--;
    unlock_calls++;
}
static task_t *proc_current(void) { return &current_task; }
static unsigned a20_futex_bucket_index(uintptr_t vkey, uintptr_t pkey) {
    return (unsigned)((vkey ^ pkey) & (FUTEX_BUCKETS - 1));
}
static int futex_writable_word_locked(task_t *task, int *uaddr,
        volatile int **word_out, uintptr_t *pkey_out, uint64_t *mm_flags) {
    (void)uaddr;
    if (task != &current_task) fail("writable lookup task mismatch");
    writable_calls++;
    *mm_flags = spin_lock_irqsave(&task->mm->lock);
    *word_out = &futex_word;
    *pkey_out = 0x123400;
    return 0;
}
static task_t *proc_find_get(int tid) {
    if (tid != owner_task.pid) fail("PI boost owner tid mismatch");
    return &owner_task;
}
static void proc_sched_pi_boost(task_t *owner, task_t *waiter) {
    if (lock_depth || owner != &owner_task || waiter != &current_task)
        fail("PI boost lock/state mismatch");
    boost_calls++;
}
static void proc_put(task_t *task) {
    if (task != &owner_task) fail("proc_put task mismatch");
    put_calls++;
}
static int futex_wait_ticks(int *uaddr, int expected, uint64_t timeout,
                            uint32_t bitset) {
    (void)uaddr;
    if (lock_depth) fail("park entered with mm/bucket lock held");
    if (timeout != 0 || bitset != FUTEX_BITSET_MATCH_ANY)
        fail("wait arguments changed");
    if ((int)futex_word != expected) fail("wait expected word mismatch");
    wait_calls++;
    if (wait_mutation && wait_calls == 1) {
        futex_word = 0;
        return -EAGAIN;
    }
    return try_wait_result;
}
static void reset(int word, int wait_result, int mutate_on_first_wait) {
    memset(&test_mm, 0, sizeof(test_mm));
    memset(g_futex_buckets, 0, sizeof(g_futex_buckets));
    current_task = (task_t){ .pid = 42, .mm = &test_mm };
    owner_task = (task_t){ .pid = 77, .mm = &test_mm };
    futex_word = word;
    lock_depth = lock_calls = unlock_calls = 0;
    writable_calls = wait_calls = boost_calls = put_calls = 0;
    try_wait_result = wait_result;
    wait_mutation = mutate_on_first_wait;
    test_error = 0;
}
static int balanced(void) {
    return lock_depth == 0 && lock_calls == unlock_calls;
}
"""


HOST_MAIN = r"""
int futex_pi_acquire(int *uaddr, int try_only);
int main(void) {
    int word_arg = 0;
    int failed = 0;

    /* Deterministic owner handoff between wait preparation and sleeping:
       the mock clears the word and returns EAGAIN exactly on first wait. */
    reset(77, 0, 1);
    int rc = futex_pi_acquire(&word_arg, 0);
    if (rc != 0 || futex_word != 42 || wait_calls != 1 ||
        writable_calls != 2 || !balanced() || boost_calls != 1 || put_calls != 1)
        { fprintf(stderr, "FAIL: EAGAIN owner-handoff rc=%d word=%d waits=%u maps=%u\n",
                  rc, futex_word, wait_calls, writable_calls); failed = 1; }

    /* Timeout continues through the existing retry path and reacquires a word
       made free by the deterministic wait mock. */
    reset(77, -ETIMEDOUT, 1);
    rc = futex_pi_acquire(&word_arg, 0);
    if (rc != 0 || futex_word != 42 || wait_calls != 1 || writable_calls != 2 ||
        !balanced()) { fprintf(stderr, "FAIL: timeout retry rc=%d\n", rc); failed = 1; }

    /* TRYLOCK must remain nonblocking and report contention once. */
    reset(77, 0, 0);
    rc = futex_pi_acquire(&word_arg, 1);
    if (rc != -EAGAIN || wait_calls != 0 || writable_calls != 1 || !balanced())
        { fprintf(stderr, "FAIL: try-only rc=%d waits=%u\n", rc, wait_calls); failed = 1; }

    /* Signals and invalid-address failures from the wait layer still escape. */
    reset(77, -ERESTARTSYS, 0);
    rc = futex_pi_acquire(&word_arg, 0);
    if (rc != -ERESTARTSYS || wait_calls != 1 || writable_calls != 1 || !balanced())
        { fprintf(stderr, "FAIL: restart propagation rc=%d\n", rc); failed = 1; }
    reset(77, -EFAULT, 0);
    rc = futex_pi_acquire(&word_arg, 0);
    if (rc != -EFAULT || wait_calls != 1 || writable_calls != 1 || !balanced())
        { fprintf(stderr, "FAIL: EFAULT propagation rc=%d\n", rc); failed = 1; }

    /* Existing fast paths: self-lock is deadlock; free word is acquired. */
    reset(42, 0, 0);
    rc = futex_pi_acquire(&word_arg, 0);
    if (rc != -EDEADLK || wait_calls != 0 || !balanced())
        { fprintf(stderr, "FAIL: self-deadlock rc=%d\n", rc); failed = 1; }
    reset(0, 0, 0);
    rc = futex_pi_acquire(&word_arg, 0);
    if (rc != 0 || futex_word != 42 || wait_calls != 0 || !balanced())
        { fprintf(stderr, "FAIL: free-word acquire rc=%d word=%d\n", rc, futex_word); failed = 1; }

    if (failed || test_error) return 1;
    puts("futex-pi-retry-host: extracted production function PASS");
    return 0;
}
"""


class FutexPiRetryHostTests(unittest.TestCase):
    def test_extracted_production_pi_acquire(self):
        gcc = shutil.which("gcc")
        if not gcc:
            self.skipTest("gcc is required for this host oracle")
        function = extract_c_function(SOURCE.read_text(), "int futex_pi_acquire(")
        with tempfile.TemporaryDirectory(prefix="a20-futex-pi-retry-") as tmp:
            c_file = pathlib.Path(tmp) / "host.c"
            exe = pathlib.Path(tmp) / "test-futex-pi-retry"
            c_file.write_text(HOST_PRELUDE + "\n" + function + "\n" + HOST_MAIN)
            subprocess.run(
                [gcc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                 str(c_file), "-o", str(exe)],
                check=True, cwd=ROOT,
            )
            result = subprocess.run([str(exe)], check=False, text=True,
                                    capture_output=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("extracted production function PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
