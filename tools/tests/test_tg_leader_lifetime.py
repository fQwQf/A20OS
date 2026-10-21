#!/usr/bin/env python3
"""Exercise the production thread-group link against leader reclamation.

The thread-group membership bodies now live in Rust
(kernel/proc/tasklist.rs); kernel/proc/proc.c keeps only the three forwarding
entry points, which this harness still extracts and compiles.  The extracted
forwarders are linked against the production Rust object, so the tested code is
the same object the kernel links, and the C accessor stubs stand in for the
task-field accessors that object takes from the C side.

The host mock gives each task an allocation ref and models the member-owned
leader anchor.  ASan cannot instrument the Rust body, so the negative control
does not rely on it: the mock's proc_put overwrites a task block with a canary
instead of releasing it, and the control asserts that dropping the anchor makes
the unlink write through that overwritten block.
"""
from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PROC_SOURCE = ROOT / "kernel/proc/proc.c"
TASKLIST_RUST = ROOT / "kernel/proc/tasklist.rs"


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start:end + 1]
    raise ValueError(f"unterminated function: {signature}")


PRELUDE = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESRCH 3

typedef struct task_t {
    struct task_t *tg_leader;
    struct task_t *tg_next;
    struct task_t **tg_prev_ptr;
    int tg_leader_ref_held;
    int refs;
} task_t;

/* The production thread-group bodies are Rust (kernel/proc/tasklist.rs); these
 * are the entry points the extracted proc.c forwarders call. */
int a20_tasklist_tg_link_locked(task_t *t);
void a20_tasklist_tg_unlink_locked(task_t *t);
void a20_tasklist_tg_leader_ref_release(task_t *t);

static int broken_ref;

/* A released task block is overwritten, not returned to the allocator: the
 * negative control reads it back to prove the unlink wrote through it, and
 * ASan cannot see the write itself (it happens in uninstrumented Rust). */
#define TASK_CANARY 0xA5
static task_t *freed_blocks[64];
static int freed_count;

static void proc_put(task_t *t) {
    assert(t && t->refs > 0);
    if (--t->refs == 0) {
        assert(freed_count < (int)(sizeof(freed_blocks) / sizeof(*freed_blocks)));
        freed_blocks[freed_count++] = t;
        memset(t, TASK_CANARY, sizeof(*t));
    }
}

#ifdef TEST_BROKEN_REF
static int task_was_released(const task_t *t) {
    for (int i = 0; i < freed_count; i++)
        if (freed_blocks[i] == t)
            return 1;
    return 0;
}

static int task_canary_intact(const task_t *t) {
    const unsigned char *bytes = (const unsigned char *)t;
    for (size_t i = 0; i < sizeof(*t); i++)
        if (bytes[i] != TASK_CANARY)
            return 0;
    return 1;
}
#endif

static task_t *proc_get(task_t *t) {
    if (!t || t->refs <= 0)
        return NULL;
    if (!broken_ref)
        t->refs++;
    return t;
}

/* Task-field accessors the Rust membership object imports from the C side. */
task_t *a20_tl_tg_leader(const void *p) {
    return ((const task_t *)p)->tg_leader;
}
task_t *a20_tl_tg_next(const void *p) {
    return ((const task_t *)p)->tg_next;
}
task_t **a20_tl_tg_prev_ptr(const void *p) {
    return ((const task_t *)p)->tg_prev_ptr;
}
int a20_tl_tg_leader_ref_held(const void *p) {
    return ((const task_t *)p)->tg_leader_ref_held;
}
void a20_tl_set_tg_leader(void *p, void *x) {
    ((task_t *)p)->tg_leader = x;
}
void a20_tl_set_tg_next(void *p, void *x) {
    ((task_t *)p)->tg_next = x;
}
void a20_tl_set_tg_prev_ptr(void *p, void **x) {
    ((task_t *)p)->tg_prev_ptr = (task_t **)x;
}
void a20_tl_set_tg_leader_ref_held(void *p, int x) {
    ((task_t *)p)->tg_leader_ref_held = x;
}
void **a20_tl_tg_next_slot(void *p) {
    return (void **)&((task_t *)p)->tg_next;
}
void *a20_tl_task_get(void *x) {
    return proc_get(x);
}
void a20_tl_task_put(void *x) {
    proc_put(x);
}
"""


HARNESS = r"""
int main(void) {
#ifdef TEST_BROKEN_REF
    broken_ref = 1;
#endif
    task_t *leader = calloc(1, sizeof(*leader));
    task_t *member = calloc(1, sizeof(*member));
    assert(leader && member);
    leader->refs = 1;             /* task-list/allocation ownership */
    member->refs = 1;
    leader->tg_leader = leader;
    member->tg_leader = leader;

    assert(proc_tg_link_locked(member) == 0);
#ifndef TEST_BROKEN_REF
    assert(leader->refs == 2);
#endif
    proc_put(leader);              /* waiter reaps the zombie leader */
    proc_tg_unlink_locked(member); /* final member detach writes leader->tg_next */
#ifdef TEST_BROKEN_REF
    /* Without the member anchor the leader was released above, so this detach
     * must have written through the overwritten block. */
    if (!task_was_released(leader))
        return 0;                  /* anchor still held: nothing to reproduce */
    if (task_canary_intact(leader))
        return 0;                  /* released but never written: control lost */
    return 3;
#else
    assert(leader->refs == 1);     /* member anchor kept embedded link alive */
    proc_tg_leader_ref_release(member); /* production final-release helper */
    proc_put(member);

    /* Both list orders remain safe with two members sharing the anchor. */
    for (int unlink_head_first = 0; unlink_head_first <= 1;
         unlink_head_first++) {
        leader = calloc(1, sizeof(*leader));
        task_t *first = calloc(1, sizeof(*first));
        task_t *second = calloc(1, sizeof(*second));
        assert(leader && first && second);
        leader->refs = first->refs = second->refs = 1;
        leader->tg_leader = leader;
        first->tg_leader = second->tg_leader = leader;
        assert(proc_tg_link_locked(first) == 0);
        assert(proc_tg_link_locked(second) == 0);
        assert(leader->refs == 3);
        proc_put(leader);          /* reap leader while both links remain */
        if (unlink_head_first) {
            proc_tg_unlink_locked(second);
            proc_tg_leader_ref_release(second);
            proc_tg_unlink_locked(first);
            proc_tg_leader_ref_release(first);
        } else {
            proc_tg_unlink_locked(first);
            proc_tg_leader_ref_release(first);
            assert(leader->refs == 1);
            assert(second->tg_prev_ptr == &leader->tg_next);
            proc_tg_unlink_locked(second);
            proc_tg_leader_ref_release(second);
        }
        proc_put(first);
        proc_put(second);
    }

    /* A clone that linked successfully and then fails initialization follows
     * destroy's unlink + final-release sequence and must balance the anchor. */
    leader = calloc(1, sizeof(*leader));
    member = calloc(1, sizeof(*member));
    assert(leader && member);
    leader->refs = member->refs = 1;
    leader->tg_leader = member->tg_leader = leader;
    assert(proc_tg_link_locked(member) == 0);
    proc_tg_unlink_locked(member);
    proc_tg_leader_ref_release(member);
    assert(leader->refs == 1);
    proc_put(member);
    proc_put(leader);

    /* If acquiring the anchor fails, link publishes no chain pointers and
     * final cleanup has no reference to drop. */
    leader = calloc(1, sizeof(*leader));
    member = calloc(1, sizeof(*member));
    assert(leader && member);
    leader->refs = 0;
    member->refs = 1;
    leader->tg_leader = member->tg_leader = leader;
    assert(proc_tg_link_locked(member) == -ESRCH);
    assert(!member->tg_prev_ptr && !member->tg_leader_ref_held);
    proc_tg_leader_ref_release(member);
    proc_put(member);
    free(leader);
    return 0;
#endif
}
"""


def build(tmp: Path, cc: list[str], extra: list[str], name: str) -> Path:
    source = PROC_SOURCE.read_text()
    link = extract_function(source, "int proc_tg_link_locked(task_t *t)")
    unlink = extract_function(source, "void proc_tg_unlink_locked(task_t *t)")
    release = extract_function(source,
                               "void proc_tg_leader_ref_release(task_t *t)")
    for text, needle in ((link, "a20_tasklist_tg_link_locked"),
                         (unlink, "a20_tasklist_tg_unlink_locked"),
                         (release, "a20_tasklist_tg_leader_ref_release")):
        if needle not in text:
            raise ValueError(f"proc.c forwarder lost {needle!r}")

    unit = tmp / "tg-lifetime.c"
    unit.write_text(PRELUDE + link + "\n" + unlink + "\n" + release + "\n"
                    + HARNESS)
    binary = tmp / name
    subprocess.run(cc + extra + [str(unit), str(tmp / "tasklist_host.o"),
                                 "-o", str(binary)], check=True)
    return binary


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="a20-tg-anchor-") as tmp:
        tmp = Path(tmp)
        # The production membership object, built for the host.
        subprocess.run([
            "rustc", "--edition=2021", "--crate-type", "lib", "--emit=obj",
            "--target", "x86_64-unknown-linux-gnu",
            "-C", "panic=abort", "-C", "opt-level=2", "-C", "codegen-units=1",
            str(TASKLIST_RUST), "-o", str(tmp / "tasklist_host.o"),
        ], check=True, cwd=ROOT)

        cc = ["cc", "-std=gnu11", "-O0", "-g", "-Wall", "-Wextra",
              "-Werror", "-fsanitize=address", "-Wl,--gc-sections"]
        asan_env = {**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}

        good = build(tmp, cc, [], "tg-lifetime")
        subprocess.run([str(good)], check=True, env=asan_env)

        broken = build(tmp, cc, ["-DTEST_BROKEN_REF"], "tg-lifetime-broken")
        result = subprocess.run([str(broken)], check=False, capture_output=True,
                                text=True, env=asan_env)
        if result.returncode != 3:
            raise SystemExit(
                "missing leader anchor was not detected (status "
                f"{result.returncode})\n{result.stderr}")
    print("test-tg-leader-lifetime: PASS (production link/unlink + "
          "overwritten-block negative control)")


if __name__ == "__main__":
    main()
