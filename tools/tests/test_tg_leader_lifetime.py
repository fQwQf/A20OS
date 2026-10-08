#!/usr/bin/env python3
"""Exercise the production thread-group link against leader reclamation.

The actual proc_tg_link_locked/proc_tg_unlink_locked bodies are extracted from
proc.c.  The host mock gives each task an allocation ref and models the new
member-owned leader anchor; ASan's negative control disables that increment and
must catch unlink writing through the leader's freed tg_next field.
"""
from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
PROC_SOURCE = ROOT / "kernel/proc/proc.c"


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

#define ESRCH 3
typedef struct task_t {
    struct task_t *tg_leader;
    struct task_t *tg_next;
    struct task_t **tg_prev_ptr;
    int tg_leader_ref_held;
    int refs;
} task_t;

static int broken_ref;
static task_t *proc_get(task_t *t) {
    if (!t || t->refs <= 0)
        return NULL;
    if (!broken_ref)
        t->refs++;
    return t;
}
static void proc_put(task_t *t) {
    assert(t && t->refs > 0);
    if (--t->refs == 0)
        free(t);
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

#ifndef TEST_BROKEN_REF
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
#endif
    return 0;
}
"""


def main() -> None:
    source = PROC_SOURCE.read_text()
    link = extract_function(source, "int proc_tg_link_locked(task_t *t)")
    unlink = extract_function(source, "void proc_tg_unlink_locked(task_t *t)")
    release = extract_function(source, "void proc_tg_leader_ref_release(task_t *t)")
    with tempfile.TemporaryDirectory(prefix="a20-tg-anchor-") as tmp:
        tmp = Path(tmp)
        cc = ["cc", "-std=gnu11", "-O0", "-g", "-Wall", "-Wextra",
              "-Werror", "-fsanitize=address"]
        unit = tmp / "tg-lifetime.c"
        unit.write_text(PRELUDE + link + "\n" + unlink + "\n" + release +
                        "\n" + HARNESS)
        good = tmp / "tg-lifetime"
        subprocess.run(cc + [str(unit), "-o", str(good)], check=True)
        subprocess.run([str(good)], check=True,
                       env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})

        broken = tmp / "tg-lifetime-broken"
        subprocess.run(cc + ["-DTEST_BROKEN_REF", str(unit), "-o", str(broken)],
                       check=True)
        result = subprocess.run(
            [str(broken)], check=False, capture_output=True, text=True,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        if result.returncode == 0 or "heap-use-after-free" not in result.stderr:
            raise SystemExit("missing leader anchor did not trigger ASan UAF")
    print("test-tg-leader-lifetime: PASS (production link/unlink + ASan negative control)")


if __name__ == "__main__":
    main()
